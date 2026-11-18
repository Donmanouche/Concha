#pragma once

// Concha's code editor: JUCE's CodeEditorComponent with what strudel.cc's CodeMirror setup adds
// (packages/codemirror): Ctrl/Alt+Enter evaluates, Ctrl/Alt+. stops, the code flashes on
// evaluation, the mini-notation ranges of the playing haps are outlined (highlight.mjs), and
// completion of Strudel's functions with their documentation (autocomplete.mjs). Ctrl+/ toggles
// comments, Ctrl+Space opens the completion.

#include "plugin/ui/DocIndex.h"
#include "plugin/ui/JsTokeniser.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace concha::ui {

class CodeView final : public juce::CodeEditorComponent, private juce::CodeDocument::Listener, private juce::Timer {
public:
    CodeView(juce::CodeDocument& document, JsTokeniser& tokeniser);
    ~CodeView() override;

    std::function<void()> onEvaluate, onHush;
    // the function under the caret changed (nullptr: none), for the Reference panel
    std::function<void(const DocEntry*, const juce::String& word)> onWordAtCaret;

    // After an evaluation: the mini-notation ranges of the evaluated code, followed through the
    // edits made since (as CodeMirror maps its decorations).
    void setMiniLocations(const std::vector<std::pair<int, int>>& ranges);
    // The ranges (as evaluated) of the haps playing now, with their colours.
    struct Mark {
        int start = 0, end = 0;
        juce::Colour colour;
    };
    void setPlaying(const std::vector<Mark>& marks);
    // slider(value, min, max, step) of the evaluated code: a slider at the end of its line; moving
    // it rewrites the value in the code and calls onSlider("slider_<from>", value) (slider.mjs).
    struct SliderInfo {
        int from = 0, to = 0;
        juce::String value;
        double min = 0, max = 1, step = 0;
    };
    void setSliders(const std::vector<SliderInfo>& sliders);
    std::function<void(const juce::String& id, double value)> onSlider;
    void flash();

    void setHighlightEnabled(bool on);
    void setFlashEnabled(bool on) { flashEnabled_ = on; }
    void setCompletionEnabled(bool on);
    void setFontSize(float size);

    bool keyPressed(const juce::KeyPress& key) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;
    void focusLost(FocusChangeType cause) override;

private:
    class Completion;

    void caretPositionMoved() override;
    void editorViewportPositionChanged() override;
    void codeDocumentTextInserted(const juce::String& newText, int insertIndex) override;
    void codeDocumentTextDeleted(int startIndex, int endIndex) override;
    void timerCallback() override;

    juce::Range<int> wordBeforeCaret() const;
    void updateCompletion(bool explicitRequest);
    void acceptCompletion();
    void toggleComment();
    void outline(juce::Graphics& g, int from, int to, juce::Colour colour) const;
    void layoutSliders();

    juce::CodeDocument& doc_;
    struct Range {
        std::unique_ptr<juce::CodeDocument::Position> from, to;
    };
    std::vector<Range> ranges_;
    std::map<std::pair<int, int>, size_t> rangeIndex_;
    std::vector<std::pair<size_t, juce::Colour>> playing_;
    bool highlight_ = true, flashEnabled_ = true, completionEnabled_ = true;
    double flashStart_ = -1;
    bool typing_ = false;
    juce::String lastWord_;
    std::unique_ptr<Completion> completion_;
    struct InlineSlider {
        std::unique_ptr<juce::CodeDocument::Position> from;
        int originalFrom = 0, length = 0;
        double step = 0.001;
        std::unique_ptr<juce::Slider> slider;
    };
    std::vector<InlineSlider> sliders_;
};

} // namespace concha::ui
