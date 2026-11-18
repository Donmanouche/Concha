#include "plugin/ui/CodeView.h"

#include "plugin/ui/Theme.h"

namespace concha::ui {

// ---------------------------------------------------------------------------------------------
// The completion popup: matching functions on the left, the documentation of the selected one on
// the right (strudel.cc's autocomplete info).
class CodeView::Completion final : public juce::Component, private juce::ListBoxModel {
public:
    explicit Completion(CodeView& owner) : owner_(owner)
    {
        list_.setModel(this);
        list_.setRowHeight(20);
        list_.setColour(juce::ListBox::backgroundColourId, theme::panel);
        list_.setColour(juce::ListBox::outlineColourId, theme::panelLine);
        list_.setOutlineThickness(1);
        info_.setMultiLine(true, true);
        info_.setReadOnly(true);
        info_.setCaretVisible(false);
        info_.setColour(juce::TextEditor::backgroundColourId, theme::panel);
        info_.setColour(juce::TextEditor::outlineColourId, theme::panelLine);
        info_.setColour(juce::TextEditor::textColourId, theme::text);
        info_.setWantsKeyboardFocus(false);
        list_.setWantsKeyboardFocus(false);
        setWantsKeyboardFocus(false);
        addAndMakeVisible(list_);
        addAndMakeVisible(info_);
    }

    void show(std::vector<const DocIndex::Completion*> items)
    {
        items_ = std::move(items);
        list_.updateContent();
        list_.selectRow(0);
        showInfo();
        setVisible(true);
    }
    bool empty() const { return items_.empty(); }
    void move(int delta)
    {
        if (items_.empty()) return;
        const int row = juce::jlimit(0, int(items_.size()) - 1, list_.getSelectedRow() + delta);
        list_.selectRow(row);
        list_.scrollToEnsureRowIsOnscreen(row);
    }
    const DocIndex::Completion* selected() const
    {
        const int r = list_.getSelectedRow();
        return r >= 0 && r < int(items_.size()) ? items_[size_t(r)] : nullptr;
    }
    void setFont(float size)
    {
        font_ = size;
        list_.setRowHeight(int(size * 1.45f));
        info_.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), size * 0.85f, juce::Font::plain)));
    }

    void resized() override
    {
        auto r = getLocalBounds();
        list_.setBounds(r.removeFromLeft(juce::jmin(260, r.getWidth() / 2)));
        info_.setBounds(r);
    }

private:
    int getNumRows() override { return int(items_.size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= int(items_.size())) return;
        if (selected) g.fillAll(juce::Colour(0xff3a3a3a));
        g.setColour(selected ? theme::accent : theme::foreground);
        g.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), font_ * 0.85f, juce::Font::plain)));
        g.drawText(items_[size_t(row)]->label, 8, 0, w - 16, h, juce::Justification::centredLeft, true);
    }
    void selectedRowsChanged(int) override { showInfo(); }
    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override { owner_.acceptCompletion(); }
    void listBoxItemClicked(int, const juce::MouseEvent&) override { showInfo(); }
    void showInfo()
    {
        if (const auto* c = selected()) info_.setText(DocIndex::describe(*c->entry, c->label), false);
        else info_.clear();
        info_.moveCaretToTop(false);
    }

    CodeView& owner_;
    juce::ListBox list_;
    juce::TextEditor info_;
    std::vector<const DocIndex::Completion*> items_;
    float font_ = 18.f;
};

// ---------------------------------------------------------------------------------------------
CodeView::CodeView(juce::CodeDocument& document, JsTokeniser& tokeniser)
    : juce::CodeEditorComponent(document, &tokeniser), doc_(document)
{
    setColourScheme(tokeniser.getDefaultColourScheme());
    // the background is painted by paint(): the visuals go between it and the code
    setColour(juce::CodeEditorComponent::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::CodeEditorComponent::defaultTextColourId, theme::foreground);
    setColour(juce::CodeEditorComponent::highlightColourId, theme::selection);
    setColour(juce::CodeEditorComponent::lineNumberBackgroundId, theme::background);
    setColour(juce::CodeEditorComponent::lineNumberTextId, theme::gutter);
    setColour(juce::CaretComponent::caretColourId, theme::caret);
    setLineNumbersShown(false);
    setTabSize(2, true);
    setScrollbarThickness(10);
    completion_ = std::make_unique<Completion>(*this);
    addChildComponent(*completion_);
    doc_.addListener(this);
    setFontSize(18.f); // strudel.cc's default
}

CodeView::~CodeView() { doc_.removeListener(this); }

void CodeView::setFontSize(float size)
{
    setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), size, juce::Font::plain)));
    completion_->setFont(size);
}

void CodeView::setHighlightEnabled(bool on)
{
    highlight_ = on;
    if (!on) playing_.clear();
    repaint();
}

void CodeView::setCompletionEnabled(bool on)
{
    completionEnabled_ = on;
    if (!on) completion_->setVisible(false);
}

void CodeView::setMiniLocations(const std::vector<std::pair<int, int>>& ranges)
{
    ranges_.clear();
    rangeIndex_.clear();
    playing_.clear();
    const int len = doc_.getNumCharacters();
    ranges_.reserve(ranges.size());
    for (const auto& [from, to] : ranges) {
        if (from >= len || to <= from) continue;
        Range r;
        r.from = std::make_unique<juce::CodeDocument::Position>(doc_, from);
        r.to = std::make_unique<juce::CodeDocument::Position>(doc_, std::min(to, len));
        r.from->setPositionMaintained(true);
        r.to->setPositionMaintained(true);
        rangeIndex_[{from, to}] = ranges_.size();
        ranges_.push_back(std::move(r));
    }
    repaint();
}

void CodeView::setSliders(const std::vector<SliderInfo>& sliders)
{
    for (auto& s : sliders_) removeChildComponent(s.slider.get());
    sliders_.clear();
    const int len = doc_.getNumCharacters();
    for (const auto& info : sliders) {
        if (info.from >= len || info.to <= info.from) continue;
        InlineSlider s;
        s.from = std::make_unique<juce::CodeDocument::Position>(doc_, info.from);
        s.from->setPositionMaintained(true);
        s.originalFrom = info.from;
        s.length = info.to - info.from;
        s.step = info.step > 0 ? info.step : (info.max - info.min) / 1000.0;
        s.slider = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::NoTextBox);
        auto& sl = *s.slider;
        sl.setRange(std::min(info.min, info.max), std::max(info.min, info.max), 0);
        sl.setValue(std::strtod(info.value.toRawUTF8(), nullptr), juce::dontSendNotification); // (".5": not getDoubleValue)
        sl.setTooltip("slider(" + info.value + ", " + juce::String(info.min) + ", " + juce::String(info.max) + ")");
        sl.setWantsKeyboardFocus(false);
        sl.setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        const size_t index = sliders_.size();
        sl.onValueChange = [this, index] {
            auto& is = sliders_[index];
            const double step = is.step;
            double v = is.slider->getValue();
            if (step > 0) v = std::round(v / step) * step;
            // the value as an <input type=range> gives it: shortest decimals
            const int decimals = juce::jlimit(0, 8, int(std::ceil(-std::log10(step > 0 ? step : 0.001)) + 0.5));
            juce::String text(v, decimals);
            if (text.containsChar('.')) text = text.trimCharactersAtEnd("0").trimCharactersAtEnd(".");
            const int start = is.from->getPosition();
            doc_.replaceSection(start, start + is.length, text);
            is.from->setPosition(start); // (an insertion at a position pushes it)
            is.length = text.length();
            if (onSlider) onSlider("slider_" + juce::String(is.originalFrom), v);
        };
        addAndMakeVisible(sl);
        sliders_.push_back(std::move(s));
    }
    layoutSliders();
}

void CodeView::layoutSliders()
{
    std::map<int, int> onLine; // sliders already placed on each line: side by side
    for (auto& s : sliders_) {
        const int line = s.from->getLineNumber();
        const int k = onLine[line]++;
        const auto end = getCharacterBounds(juce::CodeDocument::Position(doc_, line, std::numeric_limits<int>::max()));
        const int h = juce::jmax(10, getLineHeight() - 4);
        const juce::Rectangle<int> r(end.getRight() + 12 + k * 72, end.getY() + (getLineHeight() - h) / 2, 64, h); // 64 px, as slider.mjs
        s.slider->setBounds(r);
        s.slider->setVisible(r.getBottom() > 0 && r.getY() < getHeight() && r.getX() < getWidth() - 20);
    }
}

void CodeView::setPlaying(const std::vector<Mark>& marks)
{
    std::vector<std::pair<size_t, juce::Colour>> next;
    if (highlight_)
        for (const auto& m : marks)
            if (auto it = rangeIndex_.find({m.start, m.end}); it != rangeIndex_.end()) next.emplace_back(it->second, m.colour);
    if (next == playing_) return;
    playing_ = std::move(next);
    repaint();
}

void CodeView::flash()
{
    if (!flashEnabled_) return;
    flashStart_ = juce::Time::getMillisecondCounterHiRes();
    startTimerHz(60);
    repaint();
}

void CodeView::timerCallback()
{
    if (flashStart_ < 0 || juce::Time::getMillisecondCounterHiRes() - flashStart_ > 200) {
        flashStart_ = -1;
        stopTimer();
    }
    repaint();
}

void CodeView::outline(juce::Graphics& g, int from, int to, juce::Colour colour) const
{
    // one box per line the range covers (CSS outline: 2 px around the inline text)
    juce::CodeDocument::Position p(doc_, from), end(doc_, to);
    while (p.getPosition() < end.getPosition()) {
        const int line = p.getLineNumber();
        juce::CodeDocument::Position lineEnd(doc_, line, std::numeric_limits<int>::max());
        const int stop = std::min(end.getPosition(), line == end.getLineNumber() ? end.getPosition() : lineEnd.getPosition());
        const auto a = getCharacterBounds(p);
        const auto b = getCharacterBounds(juce::CodeDocument::Position(doc_, stop));
        juce::Rectangle<float> r((float)a.getX(), (float)a.getY(), (float)juce::jmax(2, b.getX() - a.getX()), (float)a.getHeight());
        g.setColour(colour);
        g.drawRect(r.expanded(1.5f), 2.0f);
        if (line == end.getLineNumber()) break;
        p = juce::CodeDocument::Position(doc_, line + 1, 0);
    }
}

void CodeView::addFrames(const std::vector<PatternWorker::Frame>& frames)
{
    bool changed = false;
    for (const auto& f : frames) {
        if (f.generation != canvasGeneration_) { // new code: draw.mjs cleanupDraw clears the screen
            canvases_.clear();
            canvasGeneration_ = f.generation;
            changed = true;
        }
        for (const auto& c : f.canvases) {
            auto& canvas = canvases_[c.id];
            if (!canvas) canvas = std::make_unique<Canvas2D>();
            canvas->run(c);
            changed = true;
        }
    }
    if (changed) repaint();
}

void CodeView::setWidgets(const std::vector<WidgetInfo>& widgets)
{
    widgets_.clear();
    const int size = doc_.getNumCharacters();
    for (const auto& w : widgets) {
        Widget x;
        x.canvas = w.canvas;
        x.to = std::make_unique<juce::CodeDocument::Position>(doc_, juce::jlimit(0, size, w.to));
        x.to->setPositionMaintained(true);
        widgets_.push_back(std::move(x));
    }
    repaint();
}

void CodeView::paint(juce::Graphics& g)
{
    if (!shaderActive_) g.fillAll(theme::background);
    bool visuals = shaderActive_;
    for (const auto& [id, c] : canvases_)
        if (c && c->drawn() && !isWidgetCanvas(id)) {
            g.drawImage(c->image(), getLocalBounds().toFloat(), juce::RectanglePlacement::stretchToFit);
            visuals = true;
        }
    if (visuals) { // strudel.cc: the code's lines get a translucent background over the visuals
        const int first = getFirstLineOnScreen(), lines = getNumLinesOnScreen() + 1;
        const float cw = getCharWidth();
        const int lh = getLineHeight();
        g.setColour(theme::lineBackground);
        for (int l = 0; l < lines && first + l < doc_.getNumLines(); ++l) {
            const juce::String text = doc_.getLine(first + l).trimEnd();
            if (text.isEmpty()) continue;
            const int start = text.length() - text.trimStart().length();
            const auto r0 = getCharacterBounds(juce::CodeDocument::Position(doc_, first + l, start));
            g.fillRect(juce::Rectangle<float>(float(r0.getX()), float(r0.getY()), cw * float(text.length() - start), float(lh)));
        }
    }
    juce::CodeEditorComponent::paint(g);
    paintWidgets(g);
}

bool CodeView::isWidgetCanvas(const std::string& id) const
{
    for (const auto& w : widgets_)
        if (w.canvas == id) return true;
    return id.find("_widget_") != std::string::npos;
}

// strudel.cc shows a widget under its line; JUCE's editor cannot open space between lines, so the
// widgets stand in a column at the right of the code, each at its line's height (pushed down so they
// do not overlap), scaled to the room left. With no room, they overlap the code's right side.
void CodeView::paintWidgets(juce::Graphics& g)
{
    if (widgets_.empty()) return;
    const float cw = getCharWidth();
    const float right = float(getWidth() - getScrollbarThickness() - 8);
    const int first = getFirstLineOnScreen(), count = getNumLinesOnScreen() + 1;
    float codeRight = 0;
    for (int l = first; l < first + count && l < doc_.getNumLines(); ++l) {
        const int len = doc_.getLine(l).trimEnd().length();
        if (len > 0)
            codeRight = std::max(codeRight, float(getCharacterBounds(juce::CodeDocument::Position(doc_, l, 0)).getX()) + cw * float(len));
    }
    codeRight += 24.0f;
    const bool column = right - codeRight >= 160.0f;
    float bottom = -1e9f;
    for (const auto& w : widgets_) {
        auto it = canvases_.find(w.canvas);
        if (it == canvases_.end() || !it->second || !it->second->drawn()) continue;
        const Canvas2D& c = *it->second;
        const float maxW = column ? right - codeRight : float(getWidth()) * 0.45f;
        const float scale = std::min(1.0f, maxW / float(c.width()));
        const float wpx = float(c.width()) * scale, hpx = float(c.height()) * scale;
        float y = float(getCharacterBounds(*w.to).getY());
        y = std::max(y, bottom + 6.0f);
        bottom = y + hpx;
        const float x = column ? codeRight : right - wpx;
        if (y + hpx < 0 || y > float(getHeight())) continue;
        const juce::Rectangle<float> r(x, y, wpx, hpx);
        g.setColour(theme::background.withAlpha(column ? 0.6f : 0.85f));
        g.fillRect(r);
        g.drawImage(c.image(), r, juce::RectanglePlacement::stretchToFit);
        g.setColour(theme::panelLine);
        g.drawRect(r, 1.0f);
    }
}

void CodeView::paintOverChildren(juce::Graphics& g)
{
    if (highlight_) {
        for (const auto& [index, colour] : playing_) {
            const Range& r = ranges_[index];
            if (r.to->getPosition() > r.from->getPosition()) outline(g, r.from->getPosition(), r.to->getPosition(), colour);
        }
    }
    if (flashStart_ >= 0) { // flash.mjs: rgba(255, 255, 255, .4) for 200 ms
        const double age = juce::Time::getMillisecondCounterHiRes() - flashStart_;
        g.setColour(juce::Colours::white.withAlpha(float(0.4 * juce::jlimit(0.0, 1.0, 1.0 - age / 200.0))));
        g.fillRect(getLocalBounds());
    }
}

void CodeView::resized()
{
    juce::CodeEditorComponent::resized();
    if (completion_ && completion_->isVisible()) updateCompletion(false); // (also called while constructing)
    layoutSliders();
}

void CodeView::focusLost(FocusChangeType cause)
{
    juce::CodeEditorComponent::focusLost(cause);
    if (completion_) completion_->setVisible(false);
}

juce::Range<int> CodeView::wordBeforeCaret() const
{
    const int caret = getCaretPos().getPosition();
    int start = caret;
    juce::CodeDocument::Position p(doc_, caret);
    while (start > 0) {
        p.moveBy(-1);
        if (!JsTokeniser::isIdentifierChar(p.getCharacter())) break;
        --start;
    }
    return {start, caret};
}

void CodeView::updateCompletion(bool explicitRequest)
{
    if (!completionEnabled_) return;
    const auto word = wordBeforeCaret();
    const juce::String text = doc_.getTextBetween(juce::CodeDocument::Position(doc_, word.getStart()),
                                                  juce::CodeDocument::Position(doc_, word.getEnd()));
    if (text.isEmpty() && !explicitRequest) {
        completion_->setVisible(false);
        return;
    }
    if (text.isNotEmpty() && juce::CharacterFunctions::isDigit(text[0])) { // a number, not a name
        completion_->setVisible(false);
        return;
    }
    auto items = text.isEmpty() ? std::vector<const DocIndex::Completion*>{} : DocIndex::instance().complete(text, 200);
    if (text.isEmpty())
        for (const auto& c : DocIndex::instance().completions()) items.push_back(&c);
    if (items.empty() || (items.size() == 1 && items[0]->label == text)) {
        completion_->setVisible(false);
        return;
    }
    completion_->show(std::move(items));
    // under the word, or above it when there's no room
    const auto at = getCharacterBounds(juce::CodeDocument::Position(doc_, word.getStart()));
    const int w = juce::jmin(getWidth() - 8, 640), h = juce::jmin(240, getHeight() / 2);
    int x = juce::jlimit(4, juce::jmax(4, getWidth() - w - 4), at.getX());
    int y = at.getBottom() + 2;
    if (y + h > getHeight()) y = juce::jmax(0, at.getY() - h - 2);
    completion_->setBounds(x, y, w, h);
    completion_->toFront(false);
}

void CodeView::acceptCompletion()
{
    const auto* c = completion_->selected();
    completion_->setVisible(false);
    if (!c) return;
    const auto word = wordBeforeCaret();
    doc_.replaceSection(word.getStart(), word.getEnd(), c->label);
    moveCaretTo(juce::CodeDocument::Position(doc_, word.getStart() + c->label.length()), false);
    grabKeyboardFocus();
}

void CodeView::toggleComment()
{
    const auto sel = getHighlightedRegion();
    const int firstLine = juce::CodeDocument::Position(doc_, sel.getStart()).getLineNumber();
    int lastLine = juce::CodeDocument::Position(doc_, sel.getEnd()).getLineNumber();
    if (!sel.isEmpty() && juce::CodeDocument::Position(doc_, sel.getEnd()).getIndexInLine() == 0 && lastLine > firstLine) --lastLine;
    bool allCommented = true;
    for (int l = firstLine; l <= lastLine; ++l) {
        const auto line = doc_.getLine(l).trimStart();
        if (line.isNotEmpty() && !line.startsWith("//")) allCommented = false;
    }
    doc_.newTransaction();
    for (int l = firstLine; l <= lastLine; ++l) {
        const juce::String line = doc_.getLine(l);
        const int start = juce::CodeDocument::Position(doc_, l, 0).getPosition();
        const int indent = line.length() - line.trimStart().length();
        if (allCommented) {
            const int at = start + indent;
            const int n = line.substring(indent).startsWith("// ") ? 3 : 2;
            if (line.substring(indent).startsWith("//")) doc_.deleteSection(at, at + n);
        } else if (line.trim().isNotEmpty()) {
            doc_.insertText(start + indent, "// ");
        }
    }
    doc_.newTransaction();
}

bool CodeView::keyPressed(const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    const int code = key.getKeyCode();
    if (completion_->isVisible()) {
        if (code == juce::KeyPress::upKey) { completion_->move(-1); return true; }
        if (code == juce::KeyPress::downKey) { completion_->move(1); return true; }
        if (code == juce::KeyPress::pageUpKey) { completion_->move(-8); return true; }
        if (code == juce::KeyPress::pageDownKey) { completion_->move(8); return true; }
        if ((code == juce::KeyPress::returnKey || code == juce::KeyPress::tabKey) && !mods.isCommandDown() && !mods.isAltDown()) {
            acceptCompletion();
            return true;
        }
        if (code == juce::KeyPress::escapeKey) {
            completion_->setVisible(false);
            return true;
        }
    }
    // strudel.cc's keymap
    if ((mods.isCommandDown() || mods.isAltDown()) && code == juce::KeyPress::returnKey) {
        completion_->setVisible(false);
        if (onEvaluate) onEvaluate();
        return true;
    }
    if ((mods.isCommandDown() || mods.isAltDown()) && key.getTextCharacter() == '.') {
        if (onHush) onHush();
        return true;
    }
    if (mods.isCommandDown() && (key.getTextCharacter() == '/' || code == '/')) {
        toggleComment();
        return true;
    }
    if (mods.isCtrlDown() && code == juce::KeyPress::spaceKey) {
        updateCompletion(true);
        return true;
    }
    typing_ = true;
    const bool handled = juce::CodeEditorComponent::keyPressed(key);
    typing_ = false;
    return handled;
}

void CodeView::codeDocumentTextInserted(const juce::String& newText, int)
{
    if (!sliders_.empty())
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<CodeView>(this)] {
            if (safe) safe->layoutSliders();
        });
    if (!typing_) return;
    if (newText.length() == 1 && JsTokeniser::isIdentifierChar(newText[0])) {
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<CodeView>(this)] {
            if (safe) safe->updateCompletion(false);
        });
    } else {
        completion_->setVisible(false);
    }
}

void CodeView::codeDocumentTextDeleted(int, int)
{
    if (!sliders_.empty())
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<CodeView>(this)] {
            if (safe) safe->layoutSliders();
        });
    if (completion_->isVisible())
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<CodeView>(this)] {
            if (safe) safe->updateCompletion(false);
        });
}

void CodeView::caretPositionMoved()
{
    juce::CodeEditorComponent::caretPositionMoved();
    if (!onWordAtCaret || !completion_) return;
    // the identifier around the caret
    const int caret = getCaretPos().getPosition();
    int a = caret, b = caret;
    juce::CodeDocument::Position p(doc_, caret);
    while (a > 0) {
        p.setPosition(a - 1);
        if (!JsTokeniser::isIdentifierChar(p.getCharacter())) break;
        --a;
    }
    const int len = doc_.getNumCharacters();
    while (b < len) {
        p.setPosition(b);
        if (!JsTokeniser::isIdentifierChar(p.getCharacter())) break;
        ++b;
    }
    const juce::String word = b > a ? doc_.getTextBetween(juce::CodeDocument::Position(doc_, a), juce::CodeDocument::Position(doc_, b)) : juce::String();
    if (word == lastWord_) return;
    lastWord_ = word;
    onWordAtCaret(word.isNotEmpty() ? DocIndex::instance().find(word) : nullptr, word);
}

void CodeView::editorViewportPositionChanged()
{
    juce::CodeEditorComponent::editorViewportPositionChanged();
    if (completion_) completion_->setVisible(false);
    layoutSliders();
    repaint();
}

} // namespace concha::ui
