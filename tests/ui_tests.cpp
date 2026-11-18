// The editor's pieces (src/plugin/ui), without a window.
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
