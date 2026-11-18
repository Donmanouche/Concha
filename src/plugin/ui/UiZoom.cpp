#include "plugin/ui/UiZoom.h"

#include <cstdlib>

namespace concha::ui {

namespace {
juce::File configDir()
{
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    return xdg && *xdg ? juce::File(juce::String::fromUTF8(xdg)) : juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".config");
}

juce::File settingsFile() { return configDir().getChildFile("Concha").getChildFile("ui.txt"); }

double envNumber(const char* name)
{
    const char* v = std::getenv(name);
    return v && *v ? juce::String(v).getDoubleValue() : 0.0;
}

// the largest scale in the desktop's monitor configuration (KDE, GNOME)
double desktopConfigScale()
{
    double best = 0;
    // KDE Plasma 6: [{"name": "outputs", "data": [{"scale": 1.25, ...}]}]
    const auto kde = configDir().getChildFile("kwinoutputconfig.json");
    if (kde.existsAsFile()) {
        const auto text = kde.loadFileAsString();
        for (int i = text.indexOf("\"scale\""); i >= 0; i = text.indexOf(i + 1, "\"scale\"")) {
            const double s = text.substring(text.indexOfChar(i, ':') + 1).trimStart().getDoubleValue();
            if (s > 0 && s < 5) best = std::max(best, s);
        }
    }
    // GNOME: <logicalmonitor> ... <scale>1.25</scale>
    const auto gnome = configDir().getChildFile("monitors.xml");
    if (gnome.existsAsFile()) {
        const auto text = gnome.loadFileAsString();
        for (int i = text.indexOf("<scale>"); i >= 0; i = text.indexOf(i + 1, "<scale>")) {
            const double s = text.substring(i + 7).getDoubleValue();
            if (s > 0 && s < 5) best = std::max(best, s);
        }
    }
    return best;
}
} // namespace

double loadZoomSetting()
{
    const auto f = settingsFile();
    if (!f.existsAsFile()) return 0;
    juce::StringArray lines;
    f.readLines(lines);
    for (const auto& l : lines)
        if (l.startsWith("zoom=")) {
            const auto v = l.fromFirstOccurrenceOf("=", false, false).trim();
            if (v == "auto") return 0;
            return juce::jlimit(kZoomMin, kZoomMax, v.getDoubleValue());
        }
    return 0;
}

void saveZoomSetting(double zoom)
{
    const auto f = settingsFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText("# Concha's interface on this computer\nzoom=" + (zoom <= 0 ? juce::String("auto") : juce::String(zoom, 2)) + "\n");
}

double systemScale(juce::Rectangle<int> area, double hostScale)
{
    if (hostScale > 1.01) return hostScale;
    if (const auto* d = juce::Desktop::getInstance().getDisplays().getDisplayForRect(area))
        if (d->scale > 1.01) return d->scale;
    const double gdk = envNumber("GDK_SCALE") * (envNumber("GDK_DPI_SCALE") > 0 ? envNumber("GDK_DPI_SCALE") : 1.0);
    if (gdk > 1.01) return gdk;
    if (const double qt = envNumber("QT_SCALE_FACTOR"); qt > 1.01) return qt;
    if (const double desk = desktopConfigScale(); desk > 1.01) return desk;
    return 1.0;
}

} // namespace concha::ui
