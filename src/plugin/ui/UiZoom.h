#pragma once

// The interface zoom: a setting of the computer (its screen), not of the project, kept in
// ~/.config/Concha/ui.txt ("zoom=auto" or "zoom=1.25"). Auto follows the system's scale, best effort:
//   1. the host's scale, when it gives one (VST3 / CLAP content scale);
//   2. JUCE's display scale (Xft.dpi / 96, GDK_SCALE), which JUCE does not apply to plug-in windows;
//   3. GDK_SCALE * GDK_DPI_SCALE, QT_SCALE_FACTOR;
//   4. the desktop's monitor configuration: KDE (kwinoutputconfig.json), GNOME (monitors.xml).
// Choices in Settings and Ctrl + = / - / 0 override it.

#include <juce_gui_basics/juce_gui_basics.h>

namespace concha::ui {

constexpr double kZoomMin = 1.0, kZoomMax = 2.5;

double loadZoomSetting(); // 0: auto
void saveZoomSetting(double zoom); // 0: auto
// the system's scale for the display holding `area` (screen coordinates), 1 when unknown
double systemScale(juce::Rectangle<int> area, double hostScale);

} // namespace concha::ui
