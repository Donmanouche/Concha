#include "plugin/ui/Logo.h"

namespace concha::ui {

void paintLogoMark(juce::Graphics& g, juce::Rectangle<float> square, juce::Colour colour)
{
    // drawn on an 80 x 80 grid
    const float k = square.getWidth() / 80.0f;
    const auto at = [&](float x, float y) { return juce::Point<float>(square.getX() + x * k, square.getY() + y * k); };
    g.setColour(colour);
    const auto c = at(40, 40);
    g.drawEllipse(juce::Rectangle<float>(65 * k, 65 * k).withCentre(c), 3.0f * k);
    // the grooves: arcs centred low on the left, inside the rim
    juce::Path inside;
    inside.addEllipse(juce::Rectangle<float>(58 * k, 58 * k).withCentre(c));
    juce::Graphics::ScopedSaveState save(g);
    g.reduceClipRegion(inside);
    const auto p = at(4.3f, 51.9f);
    juce::Path arcs;
    for (float r : {18.7f, 28.9f, 39.1f, 49.3f, 59.5f, 69.7f})
        arcs.addEllipse(juce::Rectangle<float>(2 * r * k, 2 * r * k).withCentre(p));
    g.strokePath(arcs, juce::PathStrokeType(2.4f * k));
}

float paintLogo(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour, float markSize)
{
    const auto mark = juce::Rectangle<float>(markSize, markSize).withPosition(area.getX(), area.getCentreY() - markSize / 2);
    paintLogoMark(g, mark, colour);
    const juce::Font font(juce::FontOptions(markSize * 0.5f, juce::Font::plain).withKerningFactor(0.42f));
    g.setFont(font);
    g.setColour(colour);
    const float x = mark.getRight() + markSize * 0.38f;
    const float w = juce::GlyphArrangement::getStringWidth(font, "CONCHA");
    g.drawText("CONCHA", juce::Rectangle<float>(x, area.getY(), w + 4, area.getHeight()), juce::Justification::centredLeft, false);
    return x + w - area.getX();
}

} // namespace concha::ui
