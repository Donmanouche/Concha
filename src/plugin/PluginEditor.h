#pragma once

// Concha's window, laid out like strudel.cc: the code takes the window, a side panel with tabs
// (Console, Reference, Sounds, Settings) folds away, the top bar holds update / stop, the transport
// and the meters of the 16 outputs, the bottom line the engine's status.

#include "plugin/LibraryPanel.h"
#include "plugin/PluginProcessor.h"
#include "plugin/ui/CodeView.h"
#include "plugin/ui/JsTokeniser.h"
#include "plugin/ui/Panels.h"
#include "plugin/ui/Theme.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_opengl/juce_opengl.h>

#include <memory>

namespace concha {

class ConchaEditor final : public juce::AudioProcessorEditor, private juce::Timer, private juce::CodeDocument::Listener {
public:
    explicit ConchaEditor(ConchaProcessor&);
    ~ConchaEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void codeDocumentTextInserted(const juce::String&, int) override;
    void codeDocumentTextDeleted(int, int) override;
    void evaluate();
    void applySettings();
    void togglePanel();
    void showFolders();
    void insert(const juce::String& text);

    ConchaProcessor& proc_;
    juce::OpenGLContext gl_;
    ui::LookAndFeel lookAndFeel_;
    juce::TooltipWindow tooltips_{this};

    juce::CodeDocument document_;
    ui::JsTokeniser tokeniser_;
    ui::CodeView code_{document_, tokeniser_};

    juce::TextButton update_{"update"}, stop_{"stop"}, panelButton_{"panel"};
    juce::Label transport_, status_;
    ui::Meters meters_;

    juce::TabbedComponent tabs_{juce::TabbedButtonBar::TabsAtTop};
    ui::ConsolePanel* console_ = nullptr;
    ui::ReferencePanel* reference_ = nullptr;
    ui::SoundsPanel* sounds_ = nullptr;
    ui::SettingsPanel* settings_ = nullptr;
    std::unique_ptr<LibraryPanel> library_;
    bool panelOpen_ = true;

    uint32_t shownGeneration_ = UINT32_MAX;
    juce::String syncedCode_; // the processor's code as this editor last wrote or read it
    int ticks_ = 0;
};

} // namespace concha
