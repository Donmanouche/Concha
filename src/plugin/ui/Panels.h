#pragma once

// The side panel's tabs (as strudel.cc's panel): Console, Reference, Sounds, Settings; and the
// output meters of the top bar.

#include "plugin/ui/DocIndex.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace concha::ui {

class ConsolePanel final : public juce::Component {
public:
    ConsolePanel();
    void add(const juce::String& line, juce::Colour colour);
    void resized() override;

private:
    juce::TextEditor text_;
    juce::TextButton clear_{"Clear"};
    int lines_ = 0;
};

class ReferencePanel final : public juce::Component, private juce::ListBoxModel {
public:
    ReferencePanel();
    void show(const DocEntry* entry, const juce::String& as = {}); // selects it (from the editor's caret)
    std::function<void(const juce::String&)> onInsert;             // double click: insert the name
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    int getNumRows() override { return int(filtered_.size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
    void selectedRowsChanged(int row) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void filter();

    juce::TextEditor search_;
    juce::ListBox list_;
    juce::TextEditor detail_;
    std::vector<const DocEntry*> filtered_;
};

class SoundsPanel final : public juce::Component, private juce::ListBoxModel {
public:
    SoundsPanel();
    void setSounds(const juce::StringArray& names);
    std::function<void(const juce::String&)> onInsert; // double click: insert the name
    std::function<void()> onFolders;                   // the "Sample folders" button
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    int getNumRows() override { return filtered_.size(); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void filter();

    juce::TextEditor search_;
    juce::TextButton folders_{"Sample folders..."};
    juce::ListBox list_;
    juce::Label count_;
    juce::StringArray all_, filtered_;
};

// Settings stored with the project (the processor's state): Strudel sync, editor options.
class SettingsPanel final : public juce::Component {
public:
    explicit SettingsPanel(juce::AudioProcessorValueTreeState& state);
    ~SettingsPanel() override;
    std::function<void()> onChanged; // editor options changed
    // the interface zoom: `setting` 0 = auto, `effective` the zoom in use
    void setZoom(double setting, double effective);
    std::function<void(double)> onZoom;
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    struct Content final : juce::Component {
        SettingsPanel& owner;
        explicit Content(SettingsPanel& o) : owner(o) {}
        void resized() override { owner.layoutContent(); }
    };
    void layoutContent();
    Content content_{*this};
    juce::Viewport viewport_;
    juce::AudioProcessorValueTreeState& state_;
    juce::Label syncTitle_, editorTitle_, beatsLabel_, fontLabel_, zoomLabel_;
    juce::ComboBox zoom_;
    juce::Slider beats_, font_;
    juce::ToggleButton freeRun_{"Free run (play while the DAW is stopped)"}, probe_{"Sync probe (test signal)"};
    juce::ToggleButton highlight_{"Highlight playing notes"}, flash_{"Flash on evaluate"},
        completion_{"Autocompletion"}, lineNumbers_{"Line numbers"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> beatsAttach_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> freeRunAttach_, probeAttach_;
};

// Peak meters of the 16 stereo outputs (Main, Orbit 2..16).
class Meters final : public juce::Component, public juce::SettableTooltipClient {
public:
    void set(const std::array<float, 16>& peaks); // linear peaks since the last call
    void paint(juce::Graphics& g) override;

private:
    std::array<float, 16> level_{};
};

} // namespace concha::ui
