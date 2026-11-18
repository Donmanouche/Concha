#include <algorithm>
#include <cmath>
#include "plugin/ui/Theme.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace concha::ui {

void applyTheme(juce::LookAndFeel_V4& lf)
{
    using namespace theme;
    lf.setColourScheme({panel, background, panel, panelLine, text, panelLine, background, accent, text});
    lf.setColour(juce::ResizableWindow::backgroundColourId, background);
    lf.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2c2c2c));
    lf.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff3a3a3a));
    lf.setColour(juce::TextButton::textColourOffId, text);
    lf.setColour(juce::TextButton::textColourOnId, accent);
    lf.setColour(juce::ToggleButton::textColourId, text);
    lf.setColour(juce::ToggleButton::tickColourId, accent);
    lf.setColour(juce::ToggleButton::tickDisabledColourId, dim);
    lf.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff1f1f1f));
    lf.setColour(juce::TextEditor::textColourId, text);
    lf.setColour(juce::TextEditor::outlineColourId, panelLine);
    lf.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xff555555));
    lf.setColour(juce::TextEditor::highlightColourId, selection);
    lf.setColour(juce::CaretComponent::caretColourId, caret);
    lf.setColour(juce::Label::textColourId, text);
    lf.setColour(juce::ListBox::backgroundColourId, panel);
    lf.setColour(juce::ListBox::outlineColourId, panelLine);
    lf.setColour(juce::Slider::thumbColourId, accent);
    lf.setColour(juce::Slider::trackColourId, juce::Colour(0xff555555));
    lf.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff333333));
    lf.setColour(juce::Slider::textBoxTextColourId, text);
    lf.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xff1f1f1f));
    lf.setColour(juce::Slider::textBoxOutlineColourId, panelLine);
    lf.setColour(juce::TabbedButtonBar::tabTextColourId, dim);
    lf.setColour(juce::TabbedButtonBar::frontTextColourId, foreground);
    lf.setColour(juce::TabbedButtonBar::tabOutlineColourId, juce::Colours::transparentBlack);
    lf.setColour(juce::TabbedButtonBar::frontOutlineColourId, juce::Colours::transparentBlack);
    lf.setColour(juce::TabbedComponent::backgroundColourId, panel);
    lf.setColour(juce::TabbedComponent::outlineColourId, panelLine);
    lf.setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff444444));
    lf.setColour(juce::CodeEditorComponent::backgroundColourId, background);
    lf.setColour(juce::CodeEditorComponent::defaultTextColourId, foreground);
    lf.setColour(juce::CodeEditorComponent::highlightColourId, selection);
    lf.setColour(juce::CodeEditorComponent::lineNumberBackgroundId, background);
    lf.setColour(juce::CodeEditorComponent::lineNumberTextId, gutter);
    lf.setColour(juce::PopupMenu::backgroundColourId, panel);
    lf.setColour(juce::PopupMenu::textColourId, text);
    lf.setColour(juce::PopupMenu::highlightedBackgroundColourId, juce::Colour(0xff3a3a3a));
    lf.setColour(juce::TooltipWindow::backgroundColourId, panel);
    lf.setColour(juce::TooltipWindow::textColourId, text);
}

int LookAndFeel::getTabButtonBestWidth(juce::TabBarButton& b, int)
{
    return juce::GlyphArrangement::getStringWidthInt(juce::Font(juce::FontOptions(15.f)), b.getButtonText()) + 28;
}

void LookAndFeel::drawTabButton(juce::TabBarButton& b, juce::Graphics& g, bool over, bool)
{
    const bool front = b.isFrontTab();
    auto r = b.getLocalBounds();
    g.setColour(front ? theme::foreground : over ? theme::text : theme::dim);
    g.setFont(juce::Font(juce::FontOptions(15.f)));
    g.drawText(b.getButtonText(), r, juce::Justification::centred);
    if (front) {
        g.setColour(theme::accent);
        g.fillRect(r.removeFromBottom(2).reduced(8, 0));
    }
}

void LookAndFeel::drawTabAreaBehindFrontButton(juce::TabbedButtonBar& bar, juce::Graphics& g, int w, int h)
{
    // (drawn above the other tabs: only the line under them)
    g.setColour(theme::panelLine);
    g.drawHorizontalLine(h - 1, 0.f, (float)w);
    juce::ignoreUnused(bar);
}

juce::Colour cssColour(const juce::String& cssIn, juce::Colour fallback)
{
    const juce::String css = cssIn.trim().toLowerCase();
    if (css.isEmpty()) return fallback;
    if (css.startsWithChar('#')) {
        juce::String hex = css.substring(1);
        if (hex.length() == 3 || hex.length() == 4) { // #rgb(a) -> #rrggbb(aa)
            juce::String full;
            for (auto ch : hex) full << juce::String::charToString(ch) << juce::String::charToString(ch);
            hex = full;
        }
        if (hex.length() == 6) return juce::Colour(0xff000000u | (juce::uint32)hex.getHexValue32());
        if (hex.length() == 8) {
            const auto v = (juce::uint32)hex.getHexValue32(); // rrggbbaa
            return juce::Colour((juce::uint8)((v >> 24) & 0xff), (juce::uint8)((v >> 16) & 0xff), (juce::uint8)((v >> 8) & 0xff), (juce::uint8)(v & 0xff));
        }
        return fallback;
    }
    if (css == "transparent") return juce::Colours::transparentBlack;
    if (css.startsWith("hsl")) { // hsl(h, s%, l%[, a]) / hsl(h s% l% / a)
        const auto inner = css.fromFirstOccurrenceOf("(", false, false).upToLastOccurrenceOf(")", false, false);
        juce::StringArray parts;
        parts.addTokens(inner, ", /", "");
        parts.removeEmptyStrings();
        if (parts.size() < 3) return fallback;
        const float h = std::fmod(std::fmod(parts[0].getFloatValue(), 360.0f) + 360.0f, 360.0f) / 360.0f;
        const float sat = juce::jlimit(0.0f, 1.0f, parts[1].getFloatValue() / 100.0f);
        const float l = juce::jlimit(0.0f, 1.0f, parts[2].getFloatValue() / 100.0f);
        float a = 1.0f;
        if (parts.size() > 3) a = parts[3].endsWithChar('%') ? parts[3].getFloatValue() / 100.0f : parts[3].getFloatValue();
        // HSL -> RGB
        auto f = [&](float n) {
            const float k = std::fmod(n + h * 12.0f, 12.0f);
            const float aa = sat * std::min(l, 1.0f - l);
            return l - aa * std::max(-1.0f, std::min({k - 3.0f, 9.0f - k, 1.0f}));
        };
        return juce::Colour::fromFloatRGBA(f(0), f(8), f(4), juce::jlimit(0.0f, 1.0f, a));
    }
    if (css.startsWith("rgb")) {
        const auto inner = css.fromFirstOccurrenceOf("(", false, false).upToLastOccurrenceOf(")", false, false);
        juce::StringArray parts;
        parts.addTokens(inner, ", /", "");
        parts.removeEmptyStrings();
        if (parts.size() < 3) return fallback;
        auto ch = [&](int i) { return (juce::uint8)juce::jlimit(0, 255, parts[i].getIntValue()); };
        const float a = parts.size() > 3 ? parts[3].getFloatValue() : 1.0f;
        return juce::Colour(ch(0), ch(1), ch(2), juce::jlimit(0.0f, 1.0f, a));
    }
    const juce::Colour named = juce::Colours::findColourForName(css, juce::Colour(0x00000001));
    return named == juce::Colour(0x00000001) ? fallback : named;
}

} // namespace concha::ui
