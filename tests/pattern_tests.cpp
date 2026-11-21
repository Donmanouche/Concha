// PatternEngine: Strudel in QuickJS, as the pattern thread uses it.
#include "check.h"
#include "pattern/PatternEngine.h"

#include <chrono>
#include <string>

using namespace concha;

namespace {
// Sounds used by the tests (no file is loaded here: the engine only resolves names).
const std::string kSamples =
    "samples({bd:['bd.wav'], sd:['sd.wav'], cp:['cp.wav'], hh:['hh.wav'], piano:['p.wav']}, 'file:///x/');\n";

PatternEngine& engine()
{
    static PatternEngine e; // one engine for most tests, as in the plug-in
    return e;
}

std::vector<PatternEvent> run(const std::string& code, double begin = 0, double end = 1, double cps = 0.5)
{
    std::vector<PatternEvent> ev;
    const auto r = engine().evaluate(kSamples + code);
    if (!r.ok) std::printf("    evaluate: %s\n", r.error.c_str());
    std::string err;
    if (!engine().query(begin, end, cps, ev, &err)) std::printf("    query: %s\n", err.c_str());
    engine().takeLog();
    return ev;
}

std::string str(const PatternEvent& e, const char* key)
{
    const auto* v = e.find(key);
    return v ? (v->type == ParamValue::Type::String ? v->string : std::to_string(v->number)) : "";
}
double num(const PatternEvent& e, const char* key)
{
    const auto* v = e.find(key);
    return v && v->type == ParamValue::Type::Number ? v->number : -1e9;
}
} // namespace

TEST("engine starts")
{
    CHECK(engine().ok());
    if (!engine().ok()) std::printf("    %s\n", engine().initError().c_str());
    CHECK(engine().initMilliseconds() < 2000.0);
}

TEST("events: times, values, kinds")
{
    const auto ev = run(R"(s("bd*2 [sd cp]").n("<0 3>").gain(.8))");
    CHECK(ev.size() == 4);
    if (ev.size() != 4) return;
    CHECK_NEAR(ev[0].begin, 0.0, 1e-12);
    CHECK_NEAR(ev[1].begin, 0.25, 1e-12);
    CHECK_NEAR(ev[2].begin, 0.5, 1e-12);
    CHECK_NEAR(ev[3].end, 1.0, 1e-12);
    CHECK(ev[0].kind == "sound");
    CHECK(str(ev[0], "s") == "bd");
    CHECK(str(ev[3], "s") == "cp");
    CHECK_NEAR(num(ev[0], "n"), 0.0, 1e-12);
    CHECK_NEAR(num(ev[0], "gain"), 0.8, 1e-12);
}

TEST("query window: onsets only, second cycle")
{
    const auto ev = run(R"(s("bd sd").n("<0 3>"))", 1.25, 2.0);
    CHECK(ev.size() == 1); // sd at 1.5; bd at 1.0 started before the window
    if (ev.size() == 1) {
        CHECK_NEAR(ev[0].begin, 1.5, 1e-12);
        CHECK_NEAR(num(ev[0], "n"), 3.0, 1e-12);
    }
}

TEST("code locations for highlighting")
{
    const std::string code = R"(note("c e"))";
    const auto ev = run(code);
    CHECK(ev.size() == 2);
    if (ev.size() == 2) {
        CHECK(!ev[1].locations.empty());
        if (!ev[1].locations.empty()) CHECK((kSamples + code).substr(size_t(ev[1].locations[0].first), 1) == "e");
    }
}

TEST("labels: $: patterns are stacked, _$: muted")
{
    const auto ev = run("$: s(\"bd\")\n$: s(\"hh*2\")\n_$: s(\"cp*8\")");
    CHECK(ev.size() == 3);
}

TEST("syntax error is reported, engine keeps working")
{
    const auto r = engine().evaluate("s(\"bd\"");
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    engine().takeLog();
    CHECK(run(R"(s("bd"))").size() == 1);
}

TEST("endless loop is stopped by the timeout")
{
    PatternEngine::Options o;
    o.evaluateTimeout = std::chrono::milliseconds(300);
    PatternEngine e(o);
    CHECK(e.ok());
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = e.evaluate("while (true) {}");
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(!r.ok);
    CHECK(r.error.find("timeout") != std::string::npos);
    CHECK(ms < 2000.0);
    CHECK(e.evaluate(kSamples + R"(s("bd"))").ok);
    std::vector<PatternEvent> ev;
    CHECK(e.query(0, 1, 0.5, ev));
    CHECK(ev.size() == 1);
}

TEST("error inside a query is caught")
{
    engine().evaluate(kSamples + R"(s("bd sd").fmap(v => { if (v.s === 'sd') throw new Error('boom'); return v; }))");
    std::vector<PatternEvent> ev;
    std::string err;
    const bool ok = engine().query(0, 1, 0.5, ev, &err);
    const auto log = engine().takeLog();
    CHECK(!ok || ev.size() <= 1);
    CHECK(!ok ? err.find("boom") != std::string::npos : !log.empty());
}

TEST("live controls: parameters that follow midin() / macro() carry their slope (__live)")
{
    // no host: every control reads 0; cutoff = 200 + 3800 * cc, gain = macro(2), note does not follow
    const auto ev = run(R"(const cc = await midin('knobs')
note("c3 e3").s("sawtooth").lpf(cc(74).range(200, 4000)).gain(macro(2)).room(.3))");
    CHECK(ev.size() == 2);
    for (const auto& e : ev) {
        const std::string live = str(e, "__live");
        CHECK(live.find("cutoff:0:74:0:0:3800") == 0);
        CHECK(live.find(";gain:1:2:0:0:1") != std::string::npos);
        CHECK(live.find("room") == std::string::npos);
    }
    // a control that changes the structure gets no binding; code without controls gets none
    const auto fast = run(R"(const cc = await midin('knobs')
s("bd").fast(cc(1).range(1, 3)))");
    for (const auto& e : fast) CHECK(str(e, "__live").empty());
    for (const auto& e : run(R"(s("bd sd").lpf(800))")) CHECK(str(e, "__live").empty());
}

TEST("midi(): raw messages instead of the default output")
{
    const auto ev = run(R"(note("c e").midi())");
    CHECK(ev.size() == 4); // two note-ons, two note-offs, no "sound"
    for (const auto& e : ev) CHECK(e.kind == "midimsg");
    if (ev.size() == 4) {
        CHECK(str(ev[0], "bytes") == "[144,48,114]"); // c3, velocity 0.9 * 127
        CHECK_NEAR(ev[1].begin, 0.5, 1e-12);          // note-off at the end of the hap
        CHECK(str(ev[1], "bytes") == "[128,48,0]");
    }
}

TEST("other output methods (osc...) leave the default output")
{
    const auto ev = run(R"(s("bd").osc())");
    CHECK(ev.size() == 1);
    if (!ev.empty()) CHECK(ev[0].kind == "osc");
}

TEST("strings, notes, objects in values")
{
    const auto ev = run(R"(note("c4").s("piano").sysex(["0x43", [1,2]]))");
    CHECK(ev.size() == 1);
    if (!ev.empty()) {
        CHECK(str(ev[0], "note") == "c4");
        CHECK_NEAR(num(ev[0], "sysexid"), 67.0, 1e-12); // "0x43"
        CHECK(str(ev[0], "sysexdata") == "[1,2]");     // arrays arrive as JSON
    }
}

TEST("tonal, mini-notation, signals")
{
    const auto ev = run(R"(n("0 2 4").scale("C:major").s("sine").lpf(sine.range(100, 200)))");
    CHECK(ev.size() == 3);
    if (ev.size() == 3) {
        CHECK(str(ev[1], "note").size() > 0 || num(ev[1], "note") > 0);
        CHECK(num(ev[0], "cutoff") >= 100 && num(ev[0], "cutoff") <= 200);
    }
}

TEST("deterministic: same window, same events")
{
    engine().evaluate(kSamples + R"(s("hh*8").degrade().gain(rand))");
    std::vector<PatternEvent> a, b;
    engine().query(3, 4, 0.5, a);
    engine().query(3, 4, 0.5, b);
    CHECK(a.size() == b.size());
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        CHECK(a[i].begin == b[i].begin);
        CHECK(num(a[i], "gain") == num(b[i], "gain"));
    }
    engine().takeLog();
}

TEST("two engines are independent")
{
    PatternEngine other;
    CHECK(other.ok());
    other.evaluate(kSamples + R"(s("cp*3"))");
    engine().evaluate(kSamples + R"(s("bd"))");
    std::vector<PatternEvent> a, b;
    engine().query(0, 1, 0.5, a);
    other.query(0, 1, 0.5, b);
    CHECK(a.size() == 1);
    CHECK(b.size() == 3);
    engine().takeLog();
}

TEST("sounds: unknown names are skipped, samples resolve to files, synths get a frequency")
{
    const auto ev = run(R"(stack(s("nope"), s("bd:0"), note("a4").s("sine")))");
    CHECK(ev.size() == 2);
    for (const auto& e : ev) {
        if (str(e, "__type") == "sample") CHECK(str(e, "__url") == "file:///x/bd.wav");
        else CHECK_NEAR(num(e, "__freq"), 440.0, 1e-9);
    }
}

int main() { return RUN_TESTS(); }

TEST("Tidal (tree-sitter in C++) and Mondo")
{
    auto ev = run("await initTidal()\ntidal`d1 $ note \"c3 e3\" # s \"sawtooth\" # lpf 1000`");
    CHECK(ev.size() == 2);
    if (ev.size() == 2) {
        CHECK_NEAR(ev[1].begin, 0.5, 1e-12);
        const ParamValue* note = ev[1].find("note");
        CHECK(note && note->string == "e3");
        CHECK(ev[1].find("cutoff") && ev[1].find("cutoff")->number == 1000);
        CHECK(!ev[1].locations.empty()); // mini-notation ranges inside the Haskell code
    }
    ev = run("mondo`note [c3 e3 g3] # s sawtooth`");
    CHECK(ev.size() == 3);
    if (ev.size() == 3) CHECK(ev[2].find("note") && ev[2].find("note")->string == "g3");
}

TEST("visuals: .pianoroll() & co. mark their haps (no drawing in JS); scope / spectrum ask for screens")
{
    auto& e = engine();
    auto r = e.evaluate("drums: s(\"bd*2\").pianoroll()\nbass: note(\"c2 e2\").s(\"sine\")._punchcard()\nlead: note(\"g4\").s(\"sine\")");
    CHECK(r.ok && r.visuals.channels && r.visuals.scope.empty() && !r.visuals.custom);
    CHECK(!e.hasFrames()); // nothing animated in JS
    std::vector<PatternEvent> ev;
    CHECK(e.query(0, 1, 0.5, ev));
    int marked = 0, plain = 0;
    for (const auto& x : ev) (x.visual ? marked : plain) += 1;
    CHECK(marked == 4 && plain == 1); // bd bd c2 e2 marked, g4 not
    r = e.evaluate("note(\"c3\").s(\"sine\").scope()._spectrum()");
    CHECK(r.ok && !r.visuals.channels && r.visuals.scope.size() == 1 && r.visuals.fft.size() == 1);
    CHECK(r.visuals.scope[0] == "1");
    r = e.evaluate("note(\"c3\").s(\"sine\")");
    CHECK(r.ok && !r.visuals.channels && r.visuals.scope.empty() && r.visuals.fft.empty());
    // a user's own drawing keeps Strudel's canvas
    e.setViewport(400, 200, 1.0);
    r = e.evaluate("note(\"c3 e3\").s(\"sine\").draw((haps) => { getDrawContext().fillRect(0, 0, 10, 10) }, { id: 2 })");
    CHECK(r.ok && r.visuals.custom && e.hasFrames());
    std::vector<CanvasFrame> frames;
    CHECK(e.frame(0.5, 16, frames) && !frames.empty());
}

TEST("shader`...`: the GLSL text arrives untouched, not as mini-notation")
{
    const std::string glsl = "void mainImage(out vec4 c, in vec2 p) { c = vec4(p.x * 0.5, 1.0, 0.0, 1.0); }";
    const auto r = engine().evaluate("shader`" + glsl + "`\nnote(\"c3\").uniform(\"kick\")");
    CHECK(r.ok && r.shader == glsl);
    for (const auto& [a, b] : r.miniLocations) CHECK(a > int(glsl.size()) + 7); // none inside the shader
    const auto r2 = engine().evaluate("shader(glsl`" + glsl + "`)\nnote(\"c3\")");
    CHECK(r2.ok && r2.shader == glsl);
    CHECK(engine().evaluate("note(\"c3\")").shader.empty()); // a new evaluation has none
}
