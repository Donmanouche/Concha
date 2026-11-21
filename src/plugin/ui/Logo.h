#pragma once

// Concha's logo: a concha (the Mexican sweet bun) drawn as a line, its shell of sugar grooves fanning
// from the lower left, followed by "CONCHA" in spaced capitals. One colour (black or white).

#include <juce_gui_basics/juce_gui_basics.h>

namespace concha::ui {

// the mark alone, in a square
void paintLogoMark(juce::Graphics& g, juce::Rectangle<float> square, juce::Colour colour);
// the mark and the word, vertically centred in `area`, from its left; returns the width used
float paintLogo(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour, float markSize = 28.0f);

} // namespace concha::ui
