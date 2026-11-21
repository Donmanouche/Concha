// The editor's pieces (src/plugin/ui), without a window.
#include "plugin/PluginProcessor.h"
#include "plugin/ui/Canvas2D.h"
#include "check.h"
#include "plugin/ui/CodeView.h"
#include "plugin/ui/DocIndex.h"
#include "plugin/ui/JsTokeniser.h"
#include "plugin/ui/Theme.h"

#include <juce_gui_extra/juce_gui_extra.h>

using namespace concha::ui;

namespace {
// the token kinds of a line, as "kind:text" separated by spaces (whitespace skipped)
std::string tokens(const juce::String& code)
{
    juce::CodeDocument doc;
    doc.replaceAllContent(code);
    JsTokeniser tok;
    juce::CodeDocument::Iterator it(doc);
    std::string out;
    static const char* names[] = {"err", "com", "kw", "id", "def", "lab", "bool", "num", "str", "op", "br", "pun", "cls"};
    while (!it.isEOF()) {
        it.skipWhitespace();
        if (it.isEOF()) break;
        const int start = it.getPosition();
        const int t = tok.readNextToken(it);
        const juce::String text = doc.getTextBetween(juce::CodeDocument::Position(doc, start), juce::CodeDocument::Position(doc, it.getPosition()));
        if (!out.empty()) out += ' ';
        out += std::string(names[t]) + ":" + text.toStdString();
    }
    return out;
}

juce::Slider* findSlider(juce::Component& c)
{
    for (auto* ch : c.getChildren()) {
        if (auto* s = dynamic_cast<juce::Slider*>(ch)) return s;
        if (auto* s = findSlider(*ch)) return s;
    }
    return nullptr;
}
} // namespace

#define CHECK_EQ_STR(a, b)                                                                                       \
    do {                                                                                                         \
        const std::string x_ = (a), y_ = (b);                                                                    \
        if (x_ != y_) {                                                                                          \
            std::printf("    %s:%d\n      got:      %s\n      expected: %s\n", __FILE__, __LINE__, x_.c_str(), y_.c_str()); \
            ++check::failures();                                                                                 \
        }                                                                                                        \
    } while (0)

TEST("tokeniser: strudel.cc's token kinds")
{
    CHECK_EQ_STR(tokens("$: s(\"bd hh\").lpf(800) // x"),
                 "lab:$ op:: id:s br:( str:\"bd hh\" br:) pun:. id:lpf br:( num:800 br:) com:// x");
    CHECK_EQ_STR(tokens("const drums = note('c3').gain(.5)"), "kw:const def:drums op:= id:note br:( str:'c3' br:) pun:. id:gain br:( num:.5 br:)");
    CHECK_EQ_STR(tokens("x ? true : 1e-3 /* c */"), "id:x op:? bool:true op:: num:1e-3 com:/* c */");
}

TEST("cssColour: names, hex, rgb(a), fallback")
{
    CHECK(cssColour("cyan", juce::Colours::black) == juce::Colours::cyan);
    CHECK(cssColour("#f80", juce::Colours::black) == juce::Colour(0xffff8800));
    CHECK(cssColour("#ff880080", juce::Colours::black) == juce::Colour(0x80ff8800));
    CHECK(cssColour("rgba(0, 255, 0, 0.5)", juce::Colours::black) == juce::Colour::fromRGBA(0, 255, 0, 128));
    CHECK(cssColour("not a colour", juce::Colours::red) == juce::Colours::red);
}

TEST("doc index: Strudel's functions, synonyms, completion order")
{
    const auto& d = DocIndex::instance();
    CHECK(d.entries().size() > 400);
    const DocEntry* lpf = d.find("lpf");
    CHECK(lpf != nullptr && lpf->synonyms.contains("cutoff"));
    CHECK(d.find("cutoff") == lpf);
    const auto c = d.complete("scal", 10);
    CHECK(!c.empty() && c.front()->label == "scale");
    CHECK(DocIndex::describe(*lpf).contains("Examples"));
}

TEST("code view: playing ranges follow the edits, sliders rewrite their value")
{
    juce::CodeDocument doc;
    doc.replaceAllContent("s(\"bd hh\").gain(slider(.5))");
    JsTokeniser tok;
    CodeView view(doc, tok);
    view.setSize(600, 200);
    view.setMiniLocations({{3, 5}, {6, 8}});
    doc.insertText(0, "// x\n"); // the ranges move with the text
    view.setPlaying({{3, 5, juce::Colours::white}}); // as evaluated
    // the outline is drawn where "bd" now is: paint into an image and look at the pixels around it
    juce::Image img(juce::Image::ARGB, 600, 200, true);
    juce::Graphics g(img);
    view.paintEntireComponent(g, false);
    const auto bd = view.getCharacterBounds(juce::CodeDocument::Position(doc, 8));
    float brightest = 0; // the outline's left edge, just before the text
    for (int x = bd.getX() - 4; x < bd.getX(); ++x) brightest = std::max(brightest, img.getPixelAt(x, bd.getCentreY()).getBrightness());
    CHECK(brightest > 0.8f);
    float elsewhere = 0; // and nothing around another word
    const auto hh = view.getCharacterBounds(juce::CodeDocument::Position(doc, 11));
    for (int x = hh.getX() - 4; x < hh.getX(); ++x) elsewhere = std::max(elsewhere, img.getPixelAt(x, hh.getCentreY()).getBrightness());
    CHECK(elsewhere < 0.5f);

    // slider(.5): moving it writes the value and reports it as slider_<from>
    juce::String id;
    double value = -1;
    view.onSlider = [&](const juce::String& i, double v) {
        id = i;
        value = v;
    };
    const int from = doc.getAllContent().indexOf(".5");
    view.setSliders({{from, from + 2, ".5", 0, 1, 0}});
    auto* slider = findSlider(view);
    CHECK(slider != nullptr);
    if (slider) slider->setValue(0.25, juce::sendNotificationSync);
    CHECK_EQ_STR(doc.getAllContent().toStdString(), "// x\ns(\"bd hh\").gain(slider(0.25))");
    CHECK_EQ_STR(id.toStdString(), "slider_" + std::to_string(from));
    CHECK(std::abs(value - 0.25) < 1e-9);
    if (slider) slider->setValue(0.5, juce::sendNotificationSync); // the new text is replaced, not appended
    CHECK_EQ_STR(doc.getAllContent().toStdString(), "// x\ns(\"bd hh\").gain(slider(0.5))");
}

TEST("code view: Ctrl+/ toggles comments, completion replaces the word")
{
    juce::CodeDocument doc;
    doc.replaceAllContent("a\n  b\n");
    JsTokeniser tok;
    CodeView view(doc, tok);
    view.setSize(600, 200);
    view.selectAll();
    view.keyPressed(juce::KeyPress('/', juce::ModifierKeys::commandModifier, '/'));
    CHECK_EQ_STR(doc.getAllContent().toStdString(), "// a\n  // b\n");
    view.selectAll();
    view.keyPressed(juce::KeyPress('/', juce::ModifierKeys::commandModifier, '/'));
    CHECK_EQ_STR(doc.getAllContent().toStdString(), "a\n  b\n");

    doc.replaceAllContent("n(\"0\").");
    view.moveCaretToEnd(false);
    for (auto ch : juce::String("scal")) view.keyPressed(juce::KeyPress(0, {}, ch));
    view.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey, juce::ModifierKeys::ctrlModifier, 0)); // (the popup opens asynchronously when typing)
    view.keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    CHECK_EQ_STR(doc.getAllContent().toStdString(), "n(\"0\").scale");
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    return check::run();
}

TEST("Canvas2D: Strudel's canvas calls replayed on a persistent image")
{
    using C = concha::ui::Canvas2D;
    concha::CanvasFrame f;
    f.width = 40;
    f.height = 20;
    f.strs = {"#ff0000", "rgba(0, 0, 255, 0.5)", "transparent"};
    // fillStyle red, fillRect 0 0 10 10; strokeStyle blue 50%, lineWidth 2, path, stroke
    f.nums = {C::Resize, 40, 20, C::FillStyle, 0, C::FillRect, 0, 0, 10, 10, C::StrokeStyle, 1, C::LineWidth, 2,
              C::BeginPath, C::MoveTo, 20, 15, C::LineTo, 39, 15, C::Stroke};
    concha::ui::Canvas2D c;
    c.run(f);
    CHECK(c.width() == 40 && c.height() == 20 && c.drawn());
    CHECK(c.image().getPixelAt(5, 5).getRed() == 255 && c.image().getPixelAt(5, 5).getAlpha() == 255);
    const auto stroke = c.image().getPixelAt(30, 15);
    CHECK(stroke.getBlue() > 100 && stroke.getAlpha() > 60 && stroke.getAlpha() < 200);
    CHECK(c.image().getPixelAt(15, 5).getAlpha() == 0);
    // the next frame keeps the pixels; clearRect and a snapshot put back 10 px to the left
    concha::CanvasFrame g = f;
    g.nums = {C::Snapshot, 1, 0, 0, 40, 20, C::ClearRect, 0, 0, 40, 20, C::PutImage, 1, -10, 0};
    c.run(g);
    CHECK(c.image().getPixelAt(20, 15).getBlue() > 100); // the stroke moved left by 10
    CHECK(c.image().getPixelAt(5, 5).getAlpha() == 0);   // the red square went out on the left... mostly
    // a translated arc filled: its centre is coloured
    concha::CanvasFrame h = f;
    h.nums = {C::ClearRect, 0, 0, 40, 20, C::FillStyle, 0, C::Translate, 20, 10, C::BeginPath, C::Arc, 0, 0, 5, 0, 6.283, 0, C::Fill};
    c.run(h);
    CHECK(c.image().getPixelAt(20, 10).getRed() == 255 && c.image().getPixelAt(2, 2).getAlpha() == 0);
}

TEST("cssColour: hsl and transparent")
{
    CHECK(concha::ui::cssColour("transparent", juce::Colours::red).getAlpha() == 0);
    const auto c = concha::ui::cssColour("hsl(120, 100%, 50%)", juce::Colours::red);
    CHECK(c.getGreen() == 255 && c.getRed() == 0 && c.getBlue() == 0);
    CHECK(concha::ui::cssColour("hsla(0 100% 50% / 0.5)", juce::Colours::black).getAlpha() == 128);
}

TEST("code view: a slider follows its call when the code scrolls (lines and columns)")
{
    juce::CodeDocument doc;
    juce::String code;
    for (int i = 0; i < 40; ++i) code << "// line " << i << "\n";
    code << "note(\"c3\").lpf(slider(1200, 100, 4000))\n";
    doc.replaceAllContent(code);
    JsTokeniser tok;
    CodeView view(doc, tok);
    view.setSize(300, 200);
    const int from = doc.getAllContent().indexOf("1200");
    view.setSliders({{from, from + 4, "1200", 100, 4000, 0}});
    auto* slider = findSlider(view);
    CHECK(slider != nullptr);
    if (!slider) return;
    auto callAt = [&] { return view.getCharacterBounds(juce::CodeDocument::Position(doc, doc.getAllContent().indexOf("slider("))); };
    view.scrollToLine(30); // the call comes into view
    CHECK(slider->isVisible() && slider->getX() == callAt().getX() && slider->getY() == callAt().getY());
    view.scrollToLine(32);
    CHECK(slider->getY() == callAt().getY());
    view.scrollToColumn(10);
    view.followScroll(); // (the window's timer does it)
    CHECK(slider->getX() == callAt().getX());
    view.scrollToLine(0); // out of view: hidden
    CHECK(!slider->isVisible());
}

TEST("processor: a MIDI CC moves the cutoff of a note while it plays (live controls)")
{
    juce::ScopedJuceInitialiser_GUI gui;
    concha::ConchaProcessor proc;
    const int block = 256;
    proc.setPlayConfigDetails(0, 2 * concha::kNumStereoBuses, 48000, block);
    proc.prepareToPlay(48000, block);
    if (auto* fr = proc.state().getParameter("freeRun")) fr->setValueNotifyingHost(1.0f);
    proc.evaluate(R"(const cc = await midin('knobs')
$: note("c2").s("sawtooth").lpf(cc(74).range(200, 6000)).sustain(1).slow(8).late(.25))"); // onset 0.5 s in
    juce::AudioBuffer<float> buffer(2 * concha::kNumStereoBuses, block);
    for (int i = 0; i < 1500; ++i) { // the engines start, then the code is evaluated and swapped in
        const auto st = proc.patternStatus();
        if (st.ready && !st.evaluating && st.evaluations >= 1 && !st.activeCode.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(proc.patternStatus().lastOk);
    std::vector<float> mono;
    const int blocks = int(3.0 * 48000 / block);
    auto next = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b) { // in real time, as a host: the pattern thread queries ahead
        juce::MidiBuffer midi;
        if (b == blocks / 2) midi.addEvent(juce::MidiMessage::controllerEvent(1, 74, 127), 0); // the knob turned
        buffer.clear();
        proc.processBlock(buffer, midi);
        for (int i = 0; i < block; ++i) mono.push_back(buffer.getSample(0, i));
        next += std::chrono::microseconds(block * 1000000 / 48000);
        std::this_thread::sleep_until(next);
    }
    // brightness: the share of energy above ~2 kHz (a one-pole low-pass removed)
    auto brightness = [&](size_t from, size_t to) {
        double lp = 0, high = 0, all = 0;
        for (size_t i = from; i < to && i < mono.size(); ++i) {
            lp += (mono[i] - lp) * 0.25;
            high += (mono[i] - lp) * (mono[i] - lp);
            all += double(mono[i]) * mono[i];
        }
        return all > 0 ? high / all : 0.0;
    };
    const size_t change = size_t(blocks / 2) * size_t(block);
    const double before = brightness(change - 19200, change), after = brightness(change + 4800, change + 24000);
    std::printf("    brightness before %.4f, after %.4f (%s)\n", before, after, proc.patternStatus().lastError.c_str());
    CHECK(before > 0);
    CHECK(after > before * 3); // 200 Hz -> 6 kHz on the same note, no new onset
}
