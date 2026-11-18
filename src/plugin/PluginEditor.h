#pragma once

#include "plugin/LibraryPanel.h"
#include "plugin/PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_opengl/juce_opengl.h>

#include <functional>

namespace concha {

// Phase 2 window: code box (Ctrl+Enter evaluates, Ctrl+. hushes), status line, console.
// The full editor (highlighting, playing haps, completion) is phase 5.
class ConchaEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit ConchaEditor(ConchaProcessor&);
    ~ConchaEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    // TextEditor that hands Ctrl+Enter / Ctrl+. to the editor instead of typing them.
    struct CodeBox final : juce::TextEditor {
        std::function<void()> onEvaluate, onHush;
        bool keyPressed(const juce::KeyPress& key) override;
    };

    void timerCallback() override;
    void appendConsole(const juce::String& line, juce::Colour colour);

    ConchaProcessor& proc_;
    juce::OpenGLContext gl_;

    juce::Label transport_, status_;
    juce::Slider beatsPerCycle_;
    juce::ToggleButton freeRun_{"Free run"}, probe_{"Sync probe"};
    juce::TextButton evalButton_{"Evaluer (Ctrl+Entree)"}, hushButton_{"Couper (Ctrl+.)"}, samplesButton_{"Samples"};
    std::unique_ptr<LibraryPanel> library_;
    CodeBox code_;
    juce::TextEditor console_;
    int consoleLines_ = 0;
    uint64_t seenEvaluations_ = 0;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<SliderAttachment> beatsAttach_;
    std::unique_ptr<ButtonAttachment> freeRunAttach_, probeAttach_;
};

} // namespace concha
