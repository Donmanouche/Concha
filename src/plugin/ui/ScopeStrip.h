#pragma once

// The visual band under the code, as a laboratory oscilloscope:
//  - the main screen: one channel per `$:` block that calls .pianoroll() & co. Drums are pulses,
//    a melody a step trace (its height is the pitch), chords one segment per note. The trigger (now)
//    is the centre, 1/4 cycle per division; what has played stays dimmed (the phosphor's afterglow);
//  - SCOPE (.scope()): the analysed sound, aligned on a rising zero crossing;
//  - FFT (.spectrum() / .fscope()): its spectrum on a logarithmic frequency axis.
// Painted in software like the rest of the window: the screens and graticules are drawn once into an
// image, each frame adds the traces (batched paths) and the texts. No OpenGL: on Linux, an OpenGL
// context in a plug-in window holds the X11 lock while the driver waits for a buffer, which froze the
// whole interface. Nothing exists when the code asks for none of these visuals (the band is hidden).

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>
#include <vector>

namespace concha::ui {

class ScopeStrip final : public juce::Component {
public:
    ScopeStrip();

    struct Hap {
        double begin = 0, end = 0;
        int block = 0;
        float note = 0; // NaN: no pitch
        float gain = 1;
        std::string s;
    };
    struct Model {
        bool channels = false, scope = false, fft = false;
        std::vector<std::string> blockNames;
        std::vector<Hap> haps; // around now
        double cycle = 0;
        std::vector<float> scopeData; // time domain, oldest first
        std::vector<float> fftDb;     // fftSize / 2 bins
        double sampleRate = 48000;
    };
    // message thread, about 60 times a second: rebuilds the traces, paints the band again
    void update(const Model& m);
    bool wanted(const Model& m) const { return m.channels || m.scope || m.fft; }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    struct Layout {
        juce::Rectangle<float> main, scope, fft, readouts;
    };
    struct Seg {
        juce::Point<float> a, b;
        float width;
        juce::Colour colour;
        float halo;
    };
    struct Screen {
        juce::Rectangle<float> r;
        int divX, divY;
    };
    struct Frame {
        std::vector<Seg> segs;
        std::vector<Screen> screens;
        std::vector<std::pair<juce::Rectangle<float>, juce::String>> labels;
        std::vector<juce::Colour> labelColours;
        std::vector<std::pair<juce::Rectangle<float>, std::pair<juce::String, juce::String>>> boxes;
        std::vector<juce::Colour> boxColours;
    };
    struct Readout {
        juce::String title, info;
        juce::Colour colour;
    };

    Layout layoutFor(const Model& m) const;
    void line(Frame& v, juce::Point<float> a, juce::Point<float> b, float width, juce::Colour c, float halo) const;
    void screen(Frame& v, juce::Rectangle<float> r, int divX, int divY) const;
    static void paintStatic(juce::Graphics& g, const std::vector<Screen>& screens);

    Frame frame_;
    std::vector<float> lastScope_; // the previous trace (afterglow)
    juce::Image staticImage_;      // panel, screens, graticules
    juce::String staticKey_;
};

} // namespace concha::ui
