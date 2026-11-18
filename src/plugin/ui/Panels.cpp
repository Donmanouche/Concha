#include "plugin/ui/Panels.h"

#include "plugin/ui/Theme.h"

namespace concha::ui {

namespace {
juce::Font mono(float size) { return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), size, juce::Font::plain)); }

void styleReadOnly(juce::TextEditor& t, float size)
{
    t.setMultiLine(true, true);
    t.setReadOnly(true);
    t.setCaretVisible(false);
    t.setScrollbarsShown(true);
    t.setFont(mono(size));
    t.setColour(juce::TextEditor::backgroundColourId, theme::panel);
    t.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
}

void styleSearch(juce::TextEditor& t, const juce::String& hint)
{
    t.setTextToShowWhenEmpty(hint, theme::dim);
    t.setFont(mono(14.f));
    t.setIndents(8, 6);
}

// the panel title line
void title(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text)
{
    g.setColour(theme::dim);
    g.setFont(juce::Font(juce::FontOptions(13.f, juce::Font::plain)));
    g.drawText(text, r, juce::Justification::centredLeft);
}
} // namespace

// ---------------------------------------------------------------------------------------------
ConsolePanel::ConsolePanel()
{
    styleReadOnly(text_, 13.f);
    clear_.onClick = [this] {
        text_.clear();
        lines_ = 0;
    };
    addAndMakeVisible(text_);
    addAndMakeVisible(clear_);
}

void ConsolePanel::add(const juce::String& line, juce::Colour colour)
{
    if (++lines_ > 500) { // keep it short
        text_.clear();
        lines_ = 1;
    }
    text_.moveCaretToEnd();
    text_.setColour(juce::TextEditor::textColourId, colour);
    text_.insertTextAtCaret(line + "\n");
}

void ConsolePanel::resized()
{
    auto r = getLocalBounds().reduced(6);
    clear_.setBounds(r.removeFromTop(26).removeFromRight(70));
    r.removeFromTop(4);
    text_.setBounds(r);
}

// ---------------------------------------------------------------------------------------------
ReferencePanel::ReferencePanel()
{
    styleSearch(search_, "Search functions");
    search_.onTextChange = [this] { filter(); };
    list_.setModel(this);
    list_.setRowHeight(22);
    styleReadOnly(detail_, 13.f);
    detail_.setColour(juce::TextEditor::textColourId, theme::text);
    addAndMakeVisible(search_);
    addAndMakeVisible(list_);
    addAndMakeVisible(detail_);
    filter();
}

void ReferencePanel::filter()
{
    const juce::String q = search_.getText().trim();
    filtered_.clear();
    for (const auto& e : DocIndex::instance().entries()) {
        if (q.isEmpty() || e.name.containsIgnoreCase(q)) {
            filtered_.push_back(&e);
            continue;
        }
        for (const auto& s : e.synonyms)
            if (s.containsIgnoreCase(q)) {
                filtered_.push_back(&e);
                break;
            }
    }
    list_.updateContent();
    list_.repaint();
}

void ReferencePanel::show(const DocEntry* entry, const juce::String& as)
{
    if (!entry) return;
    for (size_t i = 0; i < filtered_.size(); ++i)
        if (filtered_[i] == entry) {
            list_.selectRow(int(i));
            list_.scrollToEnsureRowIsOnscreen(int(i));
            break;
        }
    detail_.setText(DocIndex::describe(*entry, as), false);
    detail_.moveCaretToTop(false);
}

void ReferencePanel::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= int(filtered_.size())) return;
    if (selected) g.fillAll(juce::Colour(0xff333333));
    g.setColour(selected ? theme::accent : theme::text);
    g.setFont(mono(13.5f));
    g.drawText(filtered_[size_t(row)]->name, 10, 0, w - 20, h, juce::Justification::centredLeft, true);
}

void ReferencePanel::selectedRowsChanged(int row)
{
    if (row < 0 || row >= int(filtered_.size())) return;
    detail_.setText(DocIndex::describe(*filtered_[size_t(row)]), false);
    detail_.moveCaretToTop(false);
}

void ReferencePanel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    if (row >= 0 && row < int(filtered_.size()) && onInsert) onInsert(filtered_[size_t(row)]->name);
}

void ReferencePanel::resized()
{
    auto r = getLocalBounds().reduced(6);
    r.removeFromTop(18);
    search_.setBounds(r.removeFromTop(28));
    r.removeFromTop(6);
    list_.setBounds(r.removeFromTop(r.getHeight() * 2 / 5));
    r.removeFromTop(6);
    detail_.setBounds(r);
}

void ReferencePanel::paint(juce::Graphics& g)
{
    title(g, getLocalBounds().reduced(8, 4).removeFromTop(16),
          juce::String(int(DocIndex::instance().entries().size())) + " functions - double click to insert");
}

// ---------------------------------------------------------------------------------------------
SoundsPanel::SoundsPanel()
{
    styleSearch(search_, "Search sounds");
    search_.onTextChange = [this] { filter(); };
    folders_.onClick = [this] {
        if (onFolders) onFolders();
    };
    list_.setModel(this);
    list_.setRowHeight(22);
    count_.setColour(juce::Label::textColourId, theme::dim);
    addAndMakeVisible(search_);
    addAndMakeVisible(folders_);
    addAndMakeVisible(list_);
    addAndMakeVisible(count_);
}

void SoundsPanel::setSounds(const juce::StringArray& names)
{
    if (names == all_) return;
    all_ = names;
    all_.sortNatural();
    filter();
}

void SoundsPanel::filter()
{
    const juce::String q = search_.getText().trim();
    filtered_.clear();
    for (const auto& n : all_)
        if (q.isEmpty() || n.containsIgnoreCase(q)) filtered_.add(n);
    count_.setText(juce::String(filtered_.size()) + " sounds - double click to insert", juce::dontSendNotification);
    list_.updateContent();
    list_.repaint();
}

void SoundsPanel::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= filtered_.size()) return;
    if (selected) g.fillAll(juce::Colour(0xff333333));
    g.setColour(selected ? theme::accent : theme::text);
    g.setFont(mono(13.5f));
    g.drawText(filtered_[row], 10, 0, w - 20, h, juce::Justification::centredLeft, true);
}

void SoundsPanel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    if (row >= 0 && row < filtered_.size() && onInsert) onInsert(filtered_[row]);
}

void SoundsPanel::resized()
{
    auto r = getLocalBounds().reduced(6);
    auto top = r.removeFromTop(28);
    folders_.setBounds(top.removeFromRight(130));
    top.removeFromRight(6);
    search_.setBounds(top);
    r.removeFromTop(4);
    count_.setBounds(r.removeFromTop(20));
    list_.setBounds(r);
}

void SoundsPanel::paint(juce::Graphics&) {}

// ---------------------------------------------------------------------------------------------
SettingsPanel::SettingsPanel(juce::AudioProcessorValueTreeState& state) : state_(state)
{
    syncTitle_.setText("Sync with the DAW", juce::dontSendNotification);
    editorTitle_.setText("Editor", juce::dontSendNotification);
    for (auto* l : {&syncTitle_, &editorTitle_}) {
        l->setColour(juce::Label::textColourId, theme::accent);
        l->setFont(juce::Font(juce::FontOptions(15.f, juce::Font::bold)));
    }
    beatsLabel_.setText("Beats per cycle (1 cycle = N quarter notes)", juce::dontSendNotification);
    fontLabel_.setText("Font size", juce::dontSendNotification);
    zoomLabel_.setText("Interface zoom", juce::dontSendNotification);
    zoomLabel_.setTooltip("Saved for this computer, not in the project. Ctrl + = / - / 0");
    zoom_.addItem("Auto", 1);
    for (int pc : {100, 110, 125, 150, 175, 200, 225, 250}) zoom_.addItem(juce::String(pc) + " %", pc);
    zoom_.onChange = [this] {
        const int id = zoom_.getSelectedId();
        if (onZoom && id > 0) onZoom(id == 1 ? 0.0 : id / 100.0);
    };
    beats_.setSliderStyle(juce::Slider::IncDecButtons);
    beats_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 50, 24);
    font_.setSliderStyle(juce::Slider::LinearHorizontal);
    font_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 22);
    font_.setRange(10, 36, 1);

    auto& ui = state_.state;
    font_.setValue((double)ui.getProperty("fontSize", 18), juce::dontSendNotification);
    highlight_.setToggleState((bool)ui.getProperty("highlight", true), juce::dontSendNotification);
    flash_.setToggleState((bool)ui.getProperty("flash", true), juce::dontSendNotification);
    completion_.setToggleState((bool)ui.getProperty("completion", true), juce::dontSendNotification);
    lineNumbers_.setToggleState((bool)ui.getProperty("lineNumbers", false), juce::dontSendNotification);
    auto changed = [this] {
        auto& v = state_.state;
        v.setProperty("fontSize", font_.getValue(), nullptr);
        v.setProperty("highlight", highlight_.getToggleState(), nullptr);
        v.setProperty("flash", flash_.getToggleState(), nullptr);
        v.setProperty("completion", completion_.getToggleState(), nullptr);
        v.setProperty("lineNumbers", lineNumbers_.getToggleState(), nullptr);
        if (onChanged) onChanged();
    };
    font_.onValueChange = changed;
    for (auto* b : {&highlight_, &flash_, &completion_, &lineNumbers_}) b->onClick = changed;

    beatsAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(state_, "beatsPerCycle", beats_);
    freeRunAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state_, "freeRun", freeRun_);
    probeAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(state_, "syncProbe", probe_);

    for (auto* c : std::initializer_list<juce::Component*>{&syncTitle_, &beatsLabel_, &beats_, &freeRun_, &probe_, &editorTitle_,
                                                            &fontLabel_, &font_, &zoomLabel_, &zoom_, &highlight_, &flash_,
                                                            &completion_, &lineNumbers_})
        content_.addAndMakeVisible(c);
    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);
}

SettingsPanel::~SettingsPanel() = default;

void SettingsPanel::setZoom(double setting, double effective)
{
    // Auto shows the zoom it found
    zoom_.changeItemText(1, "Auto (" + juce::String(juce::roundToInt(effective * 100)) + " %)");
    const int id = setting <= 0 ? 1 : juce::roundToInt(setting * 100);
    if (zoom_.indexOfItemId(id) < 0) zoom_.addItem(juce::String(id) + " %", id);
    zoom_.setSelectedId(id, juce::dontSendNotification);
}

void SettingsPanel::resized()
{
    viewport_.setBounds(getLocalBounds());
    content_.setSize(juce::jmax(200, getWidth() - viewport_.getScrollBarThickness()), 400);
}

void SettingsPanel::layoutContent()
{
    auto r = content_.getLocalBounds().reduced(12, 10);
    auto row = [&](int h) {
        auto x = r.removeFromTop(h);
        r.removeFromTop(4);
        return x;
    };
    syncTitle_.setBounds(row(24));
    beatsLabel_.setBounds(row(20));
    beats_.setBounds(row(26).removeFromLeft(140));
    freeRun_.setBounds(row(24));
    probe_.setBounds(row(24));
    r.removeFromTop(12);
    editorTitle_.setBounds(row(24));
    auto f = row(26);
    fontLabel_.setBounds(f.removeFromLeft(80));
    font_.setBounds(f);
    auto z = row(26);
    zoomLabel_.setBounds(z.removeFromLeft(110));
    zoom_.setBounds(z.removeFromLeft(150));
    highlight_.setBounds(row(24));
    flash_.setBounds(row(24));
    completion_.setBounds(row(24));
    lineNumbers_.setBounds(row(24));
}

void SettingsPanel::paint(juce::Graphics&) {}

// ---------------------------------------------------------------------------------------------
void Meters::set(const std::array<float, 16>& peaks)
{
    bool changed = false;
    for (size_t i = 0; i < level_.size(); ++i) {
        const float next = std::max(peaks[i], level_[i] * 0.85f); // falls about 20 dB/s at 30 Hz
        changed = changed || std::abs(next - level_[i]) > 1e-4f;
        level_[i] = next < 1e-4f ? 0.f : next;
    }
    if (changed) repaint();
}

void Meters::paint(juce::Graphics& g)
{
    const int n = int(level_.size());
    const float gap = 2.f, w = (getWidth() - gap * float(n - 1)) / float(n);
    for (int i = 0; i < n; ++i) {
        const float x = float(i) * (w + gap);
        const auto bar = juce::Rectangle<float>(x, 0, w, float(getHeight()));
        g.setColour(juce::Colour(0xff2a2a2a));
        g.fillRect(bar);
        const float db = juce::Decibels::gainToDecibels(level_[size_t(i)], -60.f);
        const float frac = juce::jlimit(0.f, 1.f, (db + 60.f) / 60.f);
        const auto fill = bar.withTop(bar.getBottom() - bar.getHeight() * frac);
        g.setColour(level_[size_t(i)] >= 1.f ? theme::error : db > -6.f ? theme::warning : theme::ok);
        g.fillRect(fill);
    }
}

} // namespace concha::ui
