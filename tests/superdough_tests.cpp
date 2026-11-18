// superdough C++ (src/superdough) and the sample library (src/library).
#include "library/Soundfont.h"
#include "check.h"
#include "library/SampleLibrary.h"
#include "superdough/Convolver.h"
#include "superdough/Engine.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace concha;
using namespace concha::sd;
namespace fs = std::filesystem;

namespace {
constexpr double kSr = 48000.0;
const float NaN = std::numeric_limits<float>::quiet_NaN();

struct Out {
    std::vector<std::vector<float>> ch; // [bus*2 + c]
    explicit Out(int buses, int frames) : ch(size_t(buses) * 2, std::vector<float>(size_t(frames), 0.f)) {}
    float at(int bus, int c, int i) const { return ch[size_t(bus * 2 + c)][size_t(i)]; }
};

// Renders `frames` samples in blocks of 128 into `out` (numBuses stereo outputs).
void render(Engine& e, Out& out, int frames, int start = 0)
{
    const int buses = int(out.ch.size() / 2);
    std::vector<std::array<float*, 2>> ptr(static_cast<size_t>(buses), std::array<float*, 2>{});
    std::vector<float* const*> bp(static_cast<size_t>(buses), nullptr);
    for (int pos = start; pos < start + frames; pos += 128) {
        const int n = std::min(128, start + frames - pos);
        for (int b = 0; b < buses; ++b) {
            ptr[size_t(b)] = {out.ch[size_t(b * 2)].data() + pos, out.ch[size_t(b * 2 + 1)].data() + pos};
            bp[size_t(b)] = ptr[size_t(b)].data();
        }
        e.render(bp.data(), buses, n);
    }
}

SoundParams synth(SynthWave w, float freq)
{
    SoundParams p;
    p.type = SoundType::Synth;
    p.wave = w;
    p.frequency = freq;
    return p;
}

int firstNonZero(const std::vector<float>& v)
{
    for (size_t i = 0; i < v.size(); ++i)
        if (std::fabs(v[i]) > 1e-9f) return int(i);
    return -1;
}

void writeWav(const fs::path& path, const std::vector<float>& mono, int rate)
{
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(uint32_t(36 + mono.size() * 4));
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(3);
    u16(1);
    u32(uint32_t(rate));
    u32(uint32_t(rate * 4));
    u16(4);
    u16(32);
    f.write("data", 4);
    u32(uint32_t(mono.size() * 4));
    f.write(reinterpret_cast<const char*>(mono.data()), std::streamsize(mono.size() * 4));
}
} // namespace

// ---- WebAudio semantics ----

TEST("AudioParam: setValueAtTime, linear and exponential ramps as in the spec")
{
    AudioParam p(0.f);
    p.setValueAtTime(1.f, 0.0);
    p.linearRampToValueAtTime(3.f, 1.0);
    p.exponentialRampToValueAtTime(12.f, 2.0);
    CHECK_NEAR(p.valueAt(0.0), 1.f, 1e-6);
    CHECK_NEAR(p.valueAt(0.5), 2.f, 1e-6);
    CHECK_NEAR(p.valueAt(1.0), 3.f, 1e-6);
    CHECK_NEAR(p.valueAt(1.5), 6.f, 1e-5); // 3 * (12/3)^0.5
    CHECK_NEAR(p.valueAt(2.5), 12.f, 1e-6);
}

TEST("AudioParam: exponential ramp from 0 holds until the event (spec)")
{
    AudioParam p(0.f);
    p.setValueAtTime(0.f, 0.0);
    p.exponentialRampToValueAtTime(1.f, 1.0);
    CHECK_NEAR(p.valueAt(0.5), 0.f, 1e-9);
    CHECK_NEAR(p.valueAt(1.0), 1.f, 1e-9);
}

TEST("AudioParam: setTargetAtTime approaches its target")
{
    AudioParam p(1.f);
    p.setValueAtTime(1.f, 0.0);
    p.setTargetAtTime(0.f, 0.0, 0.1);
    for (int i = 0; i <= 10; ++i) p.valueAt(i * 0.01);
    CHECK_NEAR(p.valueAt(0.1), std::exp(-1.0), 1e-3);
}

TEST("getADSRValues: superdough's defaults rules")
{
    const ADSR synthDefaults{0.001, 0.05, 0.6, 0.01};
    auto a = getADSRValues(NaN, NaN, NaN, NaN, &synthDefaults);
    CHECK_NEAR(a.sustain, 0.6, 1e-9);
    a = getADSRValues(0.1f, NaN, NaN, NaN, &synthDefaults); // attack only: sustain 1
    CHECK_NEAR(a.attack, 0.1, 1e-6);
    CHECK_NEAR(a.sustain, 1.0, 1e-9);
    a = getADSRValues(NaN, 0.2f, NaN, NaN, &synthDefaults); // decay only: sustain 0.001
    CHECK_NEAR(a.sustain, 0.001, 1e-9);
    CHECK_NEAR(a.release, 0.01, 1e-9);
    a = getADSRValues(NaN, NaN, NaN, 0.5f, nullptr); // release only: sustain 1
    CHECK_NEAR(a.sustain, 1.0, 1e-9);
}

TEST("getParamADSR: attack, decay, sustain at the hold end, release")
{
    AudioParam g(1.f);
    getParamADSR(g, 0.1, 0.1, 0.5, 0.2, 0, 1, 0, 1.0, false);
    CHECK_NEAR(g.valueAt(0.05), 0.5f, 1e-5);
    CHECK_NEAR(g.valueAt(0.1), 1.f, 1e-5);
    CHECK_NEAR(g.valueAt(0.15), 0.75f, 1e-5);
    CHECK_NEAR(g.valueAt(0.5), 0.5f, 1e-5);
    CHECK_NEAR(g.valueAt(1.1), 0.25f, 1e-5);
    CHECK_NEAR(g.valueAt(1.3), 0.f, 1e-5);
}

TEST("Biquad lowpass: gain at the cutoff is 10^(Q/20) (Q in dB, as the spec)")
{
    for (double q : {0.0, 1.0, 10.0}) {
        Biquad b;
        b.type = FilterType::Lowpass;
        b.setParams(kSr, 1000.0, q, 0.0);
        double peak = 0;
        for (int i = 0; i < 48000; ++i) {
            const float y = b.process(0, float(std::sin(2 * kPi * 1000.0 * i / kSr)));
            if (i > 24000) peak = std::max(peak, double(std::fabs(y)));
        }
        CHECK_NEAR(peak, std::pow(10.0, q / 20.0), 0.01 * std::pow(10.0, q / 20.0));
    }
}

TEST("Oscillators: band-limited, normalised, right pitch")
{
    PeriodicWaves waves(kSr);
    for (Wave w : {Wave::Sawtooth, Wave::Square, Wave::Triangle}) {
        Oscillator o;
        o.wave = w;
        float peak = 0.f;
        int crossings = 0;
        float prev = 0.f;
        for (int i = 0; i < 48000; ++i) {
            const float s = o.next(waves, 110.0);
            peak = std::max(peak, std::fabs(s));
            if (i > 0 && (prev < 0) != (s < 0)) ++crossings;
            prev = s;
        }
        CHECK(peak <= 1.05f);
        CHECK(peak > 0.8f);
        CHECK(std::abs(crossings - 220) <= 2);
    }
}

TEST("StereoPanner: equal power, mono input")
{
    float l, r;
    stereoPan(0.f, 1, 1.f, 0.f, l, r);
    CHECK_NEAR(l, std::sqrt(0.5), 1e-6);
    CHECK_NEAR(r, std::sqrt(0.5), 1e-6);
    stereoPan(-1.f, 1, 1.f, 0.f, l, r);
    CHECK_NEAR(l, 1.0, 1e-6);
    CHECK_NEAR(r, 0.0, 1e-6);
}

// ---- engine ----

TEST("engine: a synth starts on its sample, lasts duration + release, on its orbit's output")
{
    Engine e;
    e.prepare(kSr);
    Out out(4, 48000);
    SoundParams p = synth(SynthWave::Sawtooth, 220.f);
    p.orbit = 3; // orbit 3 -> output 2
    e.trigger(70, p, 0.25, 0.5, 0.0);
    render(e, out, 48000);
    CHECK(firstNonZero(out.ch[0]) == -1);
    const int on = firstNonZero(out.ch[4]);
    CHECK(on >= 70 && on <= 72); // saw starts at 0, the attack ramps from 0
    // hold 0.25 s + default release 0.01 s: silent after ~0.27 s
    float late = 0.f;
    for (int i = 70 + int(0.28 * kSr); i < 48000; ++i) late = std::max(late, std::fabs(out.at(2, 0, i)));
    CHECK(late < 1e-6f);
    // sustain 0.6 * 0.3 (synth gain) * 0.8 (default gain): level around 0.144
    float peak = 0.f;
    for (int i = 70 + int(0.1 * kSr); i < 70 + int(0.2 * kSr); ++i) peak = std::max(peak, std::fabs(out.at(2, 0, i)));
    CHECK_NEAR(peak, 0.6 * 0.3 * 0.8, 0.02);
    CHECK(e.activeVoices() == 0);
}

TEST("engine: low-pass envelope opens and closes the filter")
{
    Engine e;
    e.prepare(kSr);
    Out a(1, 24000), b(1, 24000);
    SoundParams p = synth(SynthWave::Sawtooth, 110.f);
    p.cutoff = 200.f;
    e.trigger(0, p, 0.5, 0.5, 0.0);
    render(e, a, 24000);
    p.lpenv = 4.f; // up to 200 * 2^4 = 3200 Hz
    e.reset();
    e.trigger(0, p, 0.5, 0.5, 0.0);
    render(e, b, 24000);
    auto energyHigh = [](const std::vector<float>& v, int from, int to) { // crude: first difference energy
        double s = 0;
        for (int i = from + 1; i < to; ++i) s += double(v[size_t(i)] - v[size_t(i - 1)]) * (v[size_t(i)] - v[size_t(i - 1)]);
        return s;
    };
    CHECK(energyHigh(b.ch[0], 0, 2400) > 4 * energyHigh(a.ch[0], 0, 2400)); // bright at the start
}

TEST("engine: polyphony is capped at 128, the oldest fade out")
{
    Engine e;
    e.prepare(kSr);
    Out out(1, 128);
    for (int i = 0; i < 150; ++i) e.trigger(0, synth(SynthWave::Sine, 200.f + i), 10.0, 0.5, 0.0);
    render(e, out, 128);
    CHECK(e.activeVoices() <= Engine::kMaxVoices);
    Out later(1, int(0.3 * kSr));
    render(e, later, int(0.3 * kSr));
    CHECK(e.activeVoices() == Engine::kMaxPolyphony);
}

TEST("orbits: delay echoes, duckorbit dips the target orbit, djf filters")
{
    Engine e;
    e.prepare(kSr, true);
    Out out(2, 48000);
    // a long sine on orbit 2 (output 1)
    SoundParams pad = synth(SynthWave::Sine, 330.f);
    pad.orbit = 2;
    e.trigger(0, pad, 2.0, 0.5, 0.0);
    render(e, out, 24000);
    // at 0.5 s a sound on orbit 1 ducks orbit 2: depth .8 -> 1 - sqrt(.8), back to 1 over the attack
    SoundParams kick = synth(SynthWave::Sine, 60.f);
    kick.numDuck = 1;
    kick.duckorbit[0] = 2;
    kick.numDuckAttack = 1;
    kick.duckattack[0] = 0.1f;
    kick.numDuckDepth = 1;
    kick.duckdepth[0] = 0.8f;
    e.trigger(0, kick, 0.1, 0.5, 1.0);
    render(e, out, 24000, 24000);
    auto level = [&](int from) {
        float m = 0.f;
        for (int i = from; i < from + 240; ++i) m = std::max(m, std::fabs(out.at(1, 0, i)));
        return m;
    };
    const float before = level(20000), at = level(24000), after = level(24000 + 9600);
    CHECK(at < before * 0.2f);           // dipped to ~0.106
    CHECK(at > before * 0.05f);
    CHECK(std::fabs(after - before) < before * 0.05f); // recovered after the attack
}

TEST("orbits: feedback delay repeats at delaytime")
{
    Engine e;
    e.prepare(kSr, true);
    Out out(1, 96000);
    SoundParams p = synth(SynthWave::Sine, 440.f);
    p.delay = 1.f;
    p.delaytime = 0.25f;
    p.delayfeedback = 0.5f;
    e.trigger(0, p, 0.05, 0.5, 0.0);
    render(e, out, 96000);
    auto peakAround = [&](double t) {
        float m = 0.f;
        for (int i = int(t * kSr); i < int((t + 0.05) * kSr); ++i) m = std::max(m, std::fabs(out.at(0, 0, i)));
        return m;
    };
    const float dry = peakAround(0.0), e1 = peakAround(0.25), e2 = peakAround(0.5);
    CHECK_NEAR(e1, dry, 0.02f);       // first repeat: delay gain 1
    CHECK_NEAR(e2, dry * 0.5f, 0.02f); // then feedback 0.5
    CHECK(peakAround(0.12) < 1e-4f);  // nothing between repeats
}

TEST("buses: .bus() sends, s(\"bus\") reads in the same block, bmod adds (dc + bus) * depth / 0.3")
{
    Engine e;
    e.prepare(kSr, true);
    Out out(1, 24000);
    // the reader is triggered first: it must still see the writer's signal without delay
    SoundParams reader = synth(SynthWave::Sine, 440.f);
    reader.gain = 0.5f;
    Modulator m;
    m.kind = 3;
    m.target = ModTarget::Gain;
    m.bus = 5;
    m.dcoffset = 0.3f;
    m.depthabs = 0.3f;
    reader.modulators[0] = m;
    reader.numModulators = 1;
    e.trigger(0, reader, 0.4, 0.5, 0.0);
    SoundParams writer = synth(SynthWave::One, 0.f); // 1 * 0.3 * envelope * gain 0.8
    writer.bus = 5;
    writer.dry = 0.f;
    e.trigger(0, writer, 0.4, 0.5, 0.0);
    render(e, out, 24000);
    // sustain: writer = 0.6 * 0.3 * 0.8 = 0.144 on the bus (none on the orbit: dry 0);
    // reader gain 0.5 + (0.3 + 0.144) = 0.944 -> peak 0.6 * 0.3 * 0.944
    float peak = 0.f;
    for (int i = int(0.2 * kSr); i < int(0.3 * kSr); ++i) peak = std::max(peak, std::fabs(out.at(0, 0, i)));
    CHECK_NEAR(peak, 0.6 * 0.3 * 0.944, 0.004);

    // s("bus") plays what the bus carries
    Engine e2;
    e2.prepare(kSr, true);
    Out out2(1, 24000);
    SoundParams in = synth(SynthWave::Bus, 0.f);
    in.n = 2;
    in.sustain = 1;
    e2.trigger(0, in, 0.4, 0.5, 0.0);
    SoundParams w2 = synth(SynthWave::One, 0.f);
    w2.bus = 2;
    w2.busgain = 0.5f;
    w2.dry = 0.f;
    e2.trigger(0, w2, 0.4, 0.5, 0.0);
    render(e2, out2, 24000);
    // bus = 0.144 * 0.5, read with sustain 1, then gain 0.8
    CHECK_NEAR(out2.at(0, 0, int(0.25 * kSr)), 0.144 * 0.5 * 0.8, 0.002);
}

TEST("convolver: equals the direct convolution, no latency")
{
    auto ir = generateReverb(kSr, 1.0, 0.1, 15000, 1000, 3);
    Convolver c(*ir, false);
    const int N = 60000;
    std::vector<float> x(static_cast<size_t>(N), 0.f), yl(static_cast<size_t>(N), 0.f), yr(static_cast<size_t>(N), 0.f);
    x[0] = 1.f;
    for (int i = 0; i < 200; ++i) x[size_t(5000 + i)] = float(std::sin(i * 0.3));
    for (int p = 0; p < N; p += 100) c.process(x.data() + p, x.data() + p, yl.data() + p, yr.data() + p, std::min(100, N - p));
    const float* h = ir->channel(0);
    double err = 0;
    for (int t = 0; t < N; t += 53) {
        double s = 0;
        for (int k = 0; k <= t && k < ir->length; ++k) s += double(h[k]) * x[size_t(t - k)];
        err = std::max(err, std::fabs(s - yl[size_t(t)]));
    }
    CHECK(err < 1e-4);
    CHECK_NEAR(yl[0], h[0], 1e-6); // the impulse comes out on the same sample
}

TEST("sampler: plays the file, begin offset, reverse, speed, cut groups")
{
    const fs::path dir = fs::temp_directory_path() / "concha_sd_test";
    fs::create_directories(dir);
    std::vector<float> ramp(4800);
    for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = float(i) / 4800.f; // 0.1 s ramp 0..1
    writeWav(dir / "ramp.wav", ramp, 48000);

    auto& lib = lib::SampleLibrary::instance();
    const uint32_t id = lib.requestSample((dir / "ramp.wav").string());
    for (int i = 0; i < 500 && !lib.isLoaded(id); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(lib.isLoaded(id));

    Engine e;
    e.prepare(kSr);
    e.setSampleStore(&lib.store());
    SoundParams p;
    p.type = SoundType::Sample;
    p.sampleId = id;
    p.gain = 1.f;
    auto play = [&](SoundParams q) {
        e.reset();
        Out out(1, 9600);
        e.trigger(0, q, 1.0, 0.5, 0.0);
        render(e, out, 9600);
        return out.ch[0];
    };
    auto a = play(p);
    CHECK_NEAR(a[2400], 0.5f, 0.01f); // follows the file (no attack: envelope min 0.001 s)
    CHECK(std::fabs(a[5000]) < 1e-3f); // the slice ended (no clip/loop/release: sample length)
    SoundParams q = p;
    q.begin = 0.5f;
    a = play(q);
    CHECK_NEAR(a[1200], 0.75f, 0.01f);
    q = p;
    q.speed = -1.f;
    a = play(q);
    CHECK_NEAR(a[1200], 0.75f, 0.01f); // reversed
    q = p;
    q.speed = 2.f;
    a = play(q);
    CHECK_NEAR(a[1200], 0.5f, 0.01f);
    CHECK(std::fabs(a[2600]) < 1e-3f); // twice as short

    // cut group: the second sound stops the first within 10 ms
    e.reset();
    Out out(1, 9600);
    SoundParams c = p;
    c.cut = 1.f;
    c.loop = 1.f; // long sound
    e.trigger(0, c, 1.0, 0.5, 0.0);
    render(e, out, 1024);
    SoundParams d = c;
    d.gain = 0.f; // the second is silent: what we hear after 10 ms is the first
    e.trigger(0, d, 1.0, 0.5, 0.0);
    render(e, out, 9600 - 1024, 1024);
    CHECK(std::fabs(out.ch[0][1024 + 600]) < 1e-6f);
    fs::remove_all(dir);
}

TEST("library: folder layouts become sample maps")
{
    const fs::path root = fs::temp_directory_path() / "concha_lib_test";
    fs::remove_all(root);
    fs::create_directories(root / "bd");
    fs::create_directories(root / "MyKit" / "mykit-sd");
    fs::create_directories(root / "MyKit" / "mykit-hh");
    std::vector<float> z(10, 0.f);
    writeWav(root / "bd" / "b.wav", z, 48000);
    writeWav(root / "bd" / "a.wav", z, 48000);
    writeWav(root / "MyKit" / "mykit-sd" / "snare.wav", z, 48000);
    writeWav(root / "MyKit" / "mykit-hh" / "hat 1.wav", z, 48000);
    writeWav(root / "loose.wav", z, 48000);
    std::string err;
    const std::string json = lib::SampleLibrary::scanFolder(root.string(), "Lib", &err);
    CHECK(json.find("\"bd\":[\"bd/a.wav\",\"bd/b.wav\"]") != std::string::npos); // sorted, Dirt-Samples layout
    CHECK(json.find("\"MyKit_sd\":[\"MyKit/mykit-sd/snare.wav\"]") != std::string::npos); // bank layout
    CHECK(json.find("hat%201.wav") != std::string::npos);                                // URL-encoded
    CHECK(json.find("\"lib\":[\"loose.wav\"]") != std::string::npos);                   // root files
    CHECK(json.find("\"lib_bd\"") != std::string::npos);                                // .bank("Lib")
    CHECK(json.find("\"_base\":\"file://") != std::string::npos);
    // strudel.json wins
    std::ofstream(root / "strudel.json") << "{\"kick\": [\"bd/a.wav\"]}";
    const std::string j2 = lib::SampleLibrary::scanFolder(root.string(), "Lib", &err);
    CHECK(j2.find("\"kick\"") != std::string::npos && j2.find("\"_base\"") != std::string::npos);
    fs::remove_all(root);
}

int main() { return RUN_TESTS(); }

TEST("soundfont: a WebAudioFont preset's zones, base64 PCM, loop and base detune")
{
    // 4 int16 frames 1000, -1000, 2000, -2000 = e8 03 18 fc d0 07 30 f8
    const std::string text = "console.log('x');\nvar _tone_x={\n zones:[\n  {\n   midi:0\n   ,originalPitch:6000\n"
                             "   ,keyRangeLow:0\n   ,keyRangeHigh:59\n   ,loopStart:1\n   ,loopEnd:3\n   ,coarseTune:1\n"
                             "   ,fineTune:-11\n   ,sampleRate:22050\n   ,sample:'6AMY/NAHMPg='\n  }\n  ,{keyRangeLow:60,keyRangeHigh:127,"
                             "loopStart:2,loopEnd:4,sampleRate:44100,sample:\"6AMY/A==\"}\n ]\n};\n";
    std::string error;
    const auto zones = lib::parseSoundfont(text, error);
    CHECK(zones.size() == 2);
    CHECK(lib::findSoundfontZone(zones, 40) == 0 && lib::findSoundfontZone(zones, 60) == 0 && lib::findSoundfontZone(zones, 61) == 1);
    auto d = lib::decodeSoundfontZone(zones[0], error);
    CHECK(d && d->frames == 4 && d->sampleRate == 22050);
    CHECK(d && std::fabs(d->planar[2] - 2000.f / 65536.f) < 1e-7f);
    CHECK(d && d->loopEnd == 0 && d->loopStart == 0);              // loopStart 1 is not > 1: no loop
    CHECK(d && std::fabs(d->baseDetune - (6000 - 100 + 11)) < 1e-9); // originalPitch - 100 coarse - fine
    auto e = lib::decodeSoundfontZone(zones[1], error);
    CHECK(e && std::fabs(e->loopStart - 2.0 / 44100) < 1e-12 && std::fabs(e->loopEnd - 4.0 / 44100) < 1e-12);
    CHECK(lib::parseSoundfont("var x = {}", error).empty() && !error.empty());
}

TEST("analysers: .analyze(id) captures the voice after postgain; ids share slots; spectrum peak")
{
    CHECK(sd::analyserSlot("1") == sd::analyserSlot("1") && sd::analyserSlot("1") != sd::analyserSlot("_widget__scope_0"));
    CHECK(sd::analyserSlot("2") >= 1 && sd::analyserSlot("2") < sd::AnalyserTaps::kMaxIds);
    sd::Engine e;
    e.prepare(48000);
    sd::SoundParams p;
    p.type = sd::SoundType::Synth;
    p.wave = sd::SynthWave::Sine;
    p.frequency = 1500;
    p.analyze = int8_t(sd::analyserSlot("1"));
    p.fft = 5; // fftSize 1024
    e.trigger(0, p, 1.0, 0.5, 0);
    std::vector<float> l(512), r(512);
    float* ch[2] = {l.data(), r.data()};
    float* const* buses[1] = {ch};
    for (int b = 0; b < 40; ++b) e.render(buses, 1, 512);
    std::vector<float> t(1024);
    CHECK(e.analysers().read(p.analyze, t.data(), 1024) == 1024);
    float peak = 0;
    for (float v : t) peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.05f);
    std::vector<float> smooth, db(512);
    sd::analyserFrequencyData(t.data(), 1024, 0, smooth, db.data());
    const int bin = int(std::max_element(db.begin(), db.end()) - db.begin());
    CHECK(std::abs(bin - int(std::lround(1500.0 / (48000.0 / 1024)))) <= 1);
    CHECK(e.analysers().read(0, t.data(), 1024) == 1024); // slot 0: the main output
}
