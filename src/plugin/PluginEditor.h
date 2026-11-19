#pragma once

// Concha's window, laid out like strudel.cc: the code takes the window, a side panel with tabs
// (Console, Reference, Sounds, Settings) folds away, the top bar holds update / stop, the transport
// and the meters of the 16 outputs, the bottom line the engine's status.

#include "plugin/LibraryPanel.h"
#include "plugin/PluginProcessor.h"
#include "plugin/ui/CodeView.h"
#include "plugin/ui/JsTokeniser.h"
#include "plugin/ui/Panels.h"
#include "plugin/ui/ScopeStrip.h"
#include "plugin/ui/ShaderLayer.h"
#include "plugin/ui/Theme.h"
#include "plugin/ui/UiZoom.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_opengl/juce_opengl.h>

#include <map>
#include <memory>
#include <mutex>

namespace concha {

class ConchaEditor final : public juce::AudioProcessorEditor,
                           private juce::Timer,
                           private juce::CodeDocument::Listener,
                           private juce::OpenGLRenderer {
public:
    explicit ConchaEditor(ConchaProcessor&);
    ~ConchaEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    void setScaleFactor(float newScale) override; // the host's display scale (VST3 / CLAP)
    void parentHierarchyChanged() override;

    // Interface zoom (Settings, Ctrl + = / - / 0): `setting` 0 = auto (the system's scale), else 1..2.5.
    // Saved for the computer (ui::saveZoomSetting), not in the project.
    void setZoomSetting(double setting, bool save = true);
    double zoom() const { return zoom_; }

    // For tools (concha_ui_shot): what OpenGL drew (shader, oscilloscope band) under the components,
    // after each frame (GL thread).
    std::function<void(const juce::Image&)> onGLFrame;


private:
    void timerCallback() override;
    void codeDocumentTextInserted(const juce::String&, int) override;
    void codeDocumentTextDeleted(int, int) override;
    void evaluate();
    void applySettings();
    void togglePanel();
    void showFolders();
    void insert(const juce::String& text);
    // everything lives in content_, scaled by the zoom (the editor itself is the host's window)
    class Content;
    void paintChrome(juce::Graphics& g, juce::Rectangle<int> area);
    void layout(juce::Rectangle<int> area);
    void applyZoom();
    bool zoomKey(const juce::KeyPress& key);
    double zoomSetting_ = 0, zoom_ = 1, hostScale_ = 1;
    std::unique_ptr<Content> content_;
    // shader(`...`) behind the code (ui::ShaderLayer), on the OpenGL thread
    void newOpenGLContextCreated() override { gl_.setSwapInterval(0); } // no vsync wait while JUCE holds the locks
    void renderOpenGL() override;
    void openGLContextClosing() override
    {
        shader_.release();
    }
    void updateStrip();
    ui::ScopeStrip strip_;
    bool stripWanted_ = false;
    ui::ScopeStrip::Model stripModel_;      // what the evaluated code asked for
    juce::Rectangle<int> stripArea_;        // guarded by shaderAreaMutex_
    std::string stripScope_, stripFft_;     // their analysers' ids
    void updateShader(const std::vector<PatternWorker::VisualHap>& haps, double cycle, double cps);
    ui::ShaderLayer shader_;
    std::mutex shaderAreaMutex_;
    juce::Rectangle<int> shaderArea_; // the code view in the editor (logical pixels)
    std::map<std::string, std::pair<double, double>> uniformOnsets_; // .uniform(name): last onset (cycle), value

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

    uint32_t shownGeneration_ = UINT32_MAX, shownSounds_ = UINT32_MAX;
    juce::String syncedCode_; // the processor's code as this editor last wrote or read it
    int ticks_ = 0;
};

} // namespace concha
