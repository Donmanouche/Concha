#include "plugin/PluginEditor.h"

#include "plugin/ui/Theme.h"

namespace concha {

using namespace ui;

ConchaEditor::ConchaEditor(ConchaProcessor& p) : AudioProcessorEditor(p), proc_(p)
{
    setLookAndFeel(&lookAndFeel_);
    gl_.setComponentPaintingEnabled(true);
    gl_.attachTo(*this);

    // ---- code ----
    syncedCode_ = proc_.code();
    document_.replaceAllContent(syncedCode_);
    document_.clearUndoHistory();
    document_.addListener(this);
    code_.onEvaluate = [this] { evaluate(); };
    code_.onHush = [this] { proc_.hush(); };
    code_.onSlider = [this](const juce::String& id, double value) { proc_.setSlider(id, value); };
    code_.onWordAtCaret = [this](const DocEntry* e, const juce::String& word) {
        if (e && reference_) reference_->show(e, word);
    };
    addAndMakeVisible(code_);

    // ---- top bar ----
    update_.setTooltip("Evaluate the code (Ctrl+Enter or Alt+Enter)");
    stop_.setTooltip("Stop the pattern (Ctrl+. or Alt+.)");
    panelButton_.setTooltip("Show or hide the side panel");
    update_.onClick = [this] { evaluate(); };
    stop_.onClick = [this] { proc_.hush(); };
    panelButton_.onClick = [this] { togglePanel(); };
    const auto mono = juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain);
    transport_.setFont(juce::Font(mono));
    transport_.setColour(juce::Label::textColourId, theme::text);
    status_.setFont(juce::Font(mono.withHeight(12.5f)));
    status_.setColour(juce::Label::textColourId, theme::dim);
    meters_.setTooltip("Outputs: Main (orbits 0 and 1), then Orbit 2 to 16");
    for (auto* c : std::initializer_list<juce::Component*>{&update_, &stop_, &panelButton_, &transport_, &status_, &meters_})
        addAndMakeVisible(c);

    // ---- side panel ----
    console_ = new ConsolePanel();
    reference_ = new ReferencePanel();
    sounds_ = new SoundsPanel();
    settings_ = new SettingsPanel(proc_.state());
    tabs_.setTabBarDepth(32);
    tabs_.setOutline(0);
    tabs_.addTab("console", theme::panel, console_, true);
    tabs_.addTab("reference", theme::panel, reference_, true);
    tabs_.addTab("sounds", theme::panel, sounds_, true);
    tabs_.addTab("settings", theme::panel, settings_, true);
    auto& ui = proc_.state().state;
    tabs_.setCurrentTabIndex(juce::jlimit(0, 3, (int)ui.getProperty("panelTab", 0)), false);
    panelOpen_ = (bool)ui.getProperty("panelOpen", true);
    reference_->onInsert = [this](const juce::String& name) { insert(name); };
    sounds_->onInsert = [this](const juce::String& name) { insert(name); };
    sounds_->onFolders = [this] { showFolders(); };
    settings_->onChanged = [this] { applySettings(); };
    addAndMakeVisible(tabs_);
    applySettings();

    setResizable(true, true);
    setResizeLimits(640, 400, 3000, 2000);
    setSize(juce::jlimit(640, 3000, (int)ui.getProperty("width", 1100)), juce::jlimit(400, 2000, (int)ui.getProperty("height", 680)));
    startTimerHz(60);
    code_.grabKeyboardFocus();
}

ConchaEditor::~ConchaEditor()
{
    stopTimer();
    document_.removeListener(this);
    auto& ui = proc_.state().state;
    ui.setProperty("panelTab", tabs_.getCurrentTabIndex(), nullptr);
    ui.setProperty("panelOpen", panelOpen_, nullptr);
    gl_.detach();
    setLookAndFeel(nullptr);
}

void ConchaEditor::applySettings()
{
    auto& ui = proc_.state().state;
    code_.setFontSize((float)(double)ui.getProperty("fontSize", 18));
    code_.setHighlightEnabled((bool)ui.getProperty("highlight", true));
    code_.setFlashEnabled((bool)ui.getProperty("flash", true));
    code_.setCompletionEnabled((bool)ui.getProperty("completion", true));
    code_.setLineNumbersShown((bool)ui.getProperty("lineNumbers", false));
}

void ConchaEditor::evaluate()
{
    code_.flash();
    proc_.evaluate(document_.getAllContent());
}

void ConchaEditor::insert(const juce::String& text)
{
    code_.insertTextAtCaret(text);
    code_.grabKeyboardFocus();
}

void ConchaEditor::togglePanel()
{
    panelOpen_ = !panelOpen_;
    resized();
}

void ConchaEditor::showFolders()
{
    if (library_) {
        library_.reset();
        return;
    }
    library_ = std::make_unique<LibraryPanel>([this] { proc_.reloadLibrary(); });
    library_->onClose = [this] { juce::MessageManager::callAsync([this] { library_.reset(); }); };
    addAndMakeVisible(*library_);
    resized();
}

void ConchaEditor::codeDocumentTextInserted(const juce::String&, int)
{
    syncedCode_ = document_.getAllContent();
    proc_.setCode(syncedCode_);
}

void ConchaEditor::codeDocumentTextDeleted(int, int)
{
    syncedCode_ = document_.getAllContent();
    proc_.setCode(syncedCode_);
}

void ConchaEditor::paint(juce::Graphics& g)
{
    g.fillAll(theme::background);
    auto top = getLocalBounds().removeFromTop(44);
    g.setColour(theme::panel);
    g.fillRect(top);
    g.setColour(theme::panelLine);
    g.drawHorizontalLine(top.getBottom() - 1, 0.f, (float)getWidth());
    g.setColour(theme::accent);
    g.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
    g.drawText("concha", 14, 0, 100, 44, juce::Justification::centredLeft);
    auto bottom = getLocalBounds().removeFromBottom(24);
    g.setColour(theme::panel);
    g.fillRect(bottom);
    g.setColour(theme::panelLine);
    g.drawHorizontalLine(bottom.getY(), 0.f, (float)getWidth());
}

void ConchaEditor::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop(44).reduced(8, 7);
    top.removeFromLeft(100);
    update_.setBounds(top.removeFromLeft(76));
    top.removeFromLeft(6);
    stop_.setBounds(top.removeFromLeft(64));
    top.removeFromLeft(14);
    panelButton_.setBounds(top.removeFromRight(64));
    top.removeFromRight(12);
    meters_.setBounds(top.removeFromRight(juce::jmin(220, top.getWidth() / 3)).reduced(0, 4));
    top.removeFromRight(12);
    transport_.setBounds(top);
    status_.setBounds(r.removeFromBottom(24).reduced(10, 0));
    if (panelOpen_) {
        tabs_.setVisible(true);
        tabs_.setBounds(r.removeFromRight(juce::jlimit(300, 560, r.getWidth() * 36 / 100)));
    } else {
        tabs_.setVisible(false);
    }
    code_.setBounds(r.reduced(10, 6)); // strudel.cc's editor has some air around the text
    if (library_) library_->setBounds(r);
    auto& ui = proc_.state().state;
    ui.setProperty("width", getWidth(), nullptr);
    ui.setProperty("height", getHeight(), nullptr);
}

void ConchaEditor::timerCallback()
{
    const auto& v = proc_.transportView();
    const auto s = proc_.patternStatus();

    // after an evaluation: the evaluated code's mini-notation ranges, the known sounds
    if (s.activeGeneration != shownGeneration_) {
        const auto info = proc_.activeCodeInfo();
        shownGeneration_ = info.generation;
        code_.setMiniLocations(info.miniLocations);
        std::vector<CodeView::SliderInfo> sliders;
        for (const auto& x : info.sliders)
            sliders.push_back({x.from, x.to, juce::String::fromUTF8(x.value.c_str()), x.min, x.max, x.step});
        code_.setSliders(sliders);
        juce::StringArray names;
        for (const auto& n : info.soundNames) names.add(juce::String::fromUTF8(n.c_str()));
        sounds_->setSounds(names);
    }
    // the haps playing now (highlight.mjs: outline in hap.value.color or the foreground)
    std::vector<CodeView::Mark> marks;
    if (v.playing.load())
        for (const auto& h : proc_.activeHaps(v.cycle.load())) {
            if (h.generation != s.activeGeneration) continue;
            const juce::Colour c = h.color.empty() ? theme::foreground : cssColour(juce::String::fromUTF8(h.color.c_str()), theme::foreground);
            for (const auto& [a, b] : h.locations) marks.push_back({a, b, c});
        }
    code_.setPlaying(marks);

    std::array<float, 16> peaks{};
    const auto taken = proc_.takePeaks();
    std::copy(taken.begin(), taken.end(), peaks.begin());
    meters_.set(peaks);

    if (++ticks_ % 4 != 0) return; // the texts at 15 Hz
    if (const juce::String code = proc_.code(); code != syncedCode_) { // the DAW loaded a project or a preset
        syncedCode_ = code;
        document_.replaceAllContent(code);
        document_.clearUndoHistory();
    }
    transport_.setText(juce::String::formatted("%s%s  %6.2f bpm   cycle %8.3f", v.playing ? (v.freeRunning ? "free run" : "playing") : "stopped",
                                               v.looping ? " (loop)" : "", v.bpm.load(), v.cycle.load()),
                       juce::dontSendNotification);
    const auto& st = proc_.schedulerStats();
    juce::String line;
    if (!s.engineError.empty()) line = "engine: " + juce::String(s.engineError);
    else if (!s.ready) line = "starting the Strudel engine...";
    else {
        line << (s.evaluating ? "evaluating..." : s.lastOk ? "ok" : s.evaluations ? "error" : "ready");
        if (s.evaluations) line << juce::String::formatted(" (%.0f ms)", s.lastEvalMs);
        if (s.evaluations && document_.getAllContent().toStdString() != s.activeCode) line << "   * modified, Ctrl+Enter to update";
        line << juce::String::formatted("   voices %d   events %llu   late %llu", proc_.activeVoices(),
                                        (unsigned long long)st.emitted.load(), (unsigned long long)st.late.load());
        if (s.droppedEvents) line << juce::String::formatted("   dropped %llu", (unsigned long long)s.droppedEvents);
    }
    status_.setText(line, juce::dontSendNotification);
    status_.setColour(juce::Label::textColourId, s.ready && !s.lastOk && s.evaluations ? theme::error : theme::dim);

    for (const auto& m : proc_.takeLog()) {
        const juce::String msg = juce::String::fromUTF8(m.message.c_str());
        if (msg.contains("@strudel/core loaded")) continue;
        console_->add(msg, m.type == "error" ? theme::error : m.type == "warning" ? theme::warning : theme::dim);
        if (m.type == "error" && !panelOpen_) togglePanel();
        if (m.type == "error") tabs_.setCurrentTabIndex(0);
    }
}

} // namespace concha
