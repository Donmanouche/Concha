#pragma once

// strudel.cc's default theme (packages/codemirror/themes/strudel-theme.mjs) and the colours of
// its panels, shared by Concha's editor components.

#include <juce_gui_basics/juce_gui_basics.h>

namespace concha::ui {

namespace theme {
// editor settings
inline const juce::Colour background{0xff222222};
inline const juce::Colour foreground{0xffffffff};
inline const juce::Colour caret{0xffffcc00};
inline const juce::Colour selection = juce::Colour::fromRGBA(128, 203, 196, 128);
inline const juce::Colour lineHighlight = juce::Colour::fromRGBA(0, 0, 0, 0x50);
inline const juce::Colour gutter = juce::Colour(0x668a9199);
// syntax (strudel-theme styles)
inline const juce::Colour keyword{0xffc792ea};
inline const juce::Colour variable{0xffc792ea};
inline const juce::Colour property{0xffc792ea};
inline const juce::Colour definition{0xff82aaff};
inline const juce::Colour operatorColour{0xff89ddff};
inline const juce::Colour label{0xff89ddff};
inline const juce::Colour boolean{0xff89ddff};
inline const juce::Colour number{0xffc3e88d};
inline const juce::Colour string{0xffc3e88d};
inline const juce::Colour comment{0xff7d8799};
inline const juce::Colour bracket{0xff525154};
inline const juce::Colour punctuation{0xff82aaff};
inline const juce::Colour className{0xffdecb6b};
// strudel-theme.mjs lineBackground (#22222299): behind the code when visuals are drawn
inline const juce::Colour lineBackground{0x99222222};
// panels around the editor
inline const juce::Colour panel{0xff1b1b1b};
inline const juce::Colour panelLine{0xff333333};
inline const juce::Colour text{0xffdddddd};
inline const juce::Colour dim{0xff8a9199};
inline const juce::Colour accent{0xffffcc00};
inline const juce::Colour error{0xffff5370};
inline const juce::Colour warning{0xffffcb6b};
inline const juce::Colour ok{0xff00ca28};
} // namespace theme

// Colours a juce widget tree like the theme (buttons, text boxes, lists, sliders).
void applyTheme(juce::LookAndFeel_V4& lf);

// The theme, with strudel.cc's flat tabs (text, the current one underlined).
class LookAndFeel final : public juce::LookAndFeel_V4 {
public:
    LookAndFeel() { applyTheme(*this); }
    int getTabButtonBestWidth(juce::TabBarButton& b, int depth) override;
    void drawTabButton(juce::TabBarButton& b, juce::Graphics& g, bool over, bool down) override;
    void drawTabAreaBehindFrontButton(juce::TabbedButtonBar& bar, juce::Graphics& g, int w, int h) override;
};

// A CSS colour as Strudel accepts in .color() ("red", "#f80", "#ff8800aa", "rgb(...)"); fallback
// when it can't be read.
juce::Colour cssColour(const juce::String& css, juce::Colour fallback);

} // namespace concha::ui
