#include "plugin/PluginEditor.h"

namespace concha {

namespace {
const juce::Colour kBackground(0xff15161a), kAccent(0xfff0c674), kText(0xffd8d8d8), kError(0xffff6b6b),
    kWarning(0xffe5c07b), kDim(0xff8a8f98);
}

bool ConchaEditor::CodeBox::keyPressed(const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (mods.isCommandDown() && (key.getKeyCode() == juce::KeyPress::returnKey)) {
        if (onEvaluate) onEvaluate();
        return true;
    }
    if (mods.isCommandDown() && key.getTextCharacter() == '.') {
        if (onHush) onHush();
        return true;
    }
    return juce::TextEditor::keyPressed(key);
}

ConchaEditor::ConchaEditor(ConchaProcessor& p) : AudioProcessorEditor(p), proc_(p)
{
    gl_.setComponentPaintingEnabled(true);
    gl_.attachTo(*this);

    const auto mono = juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 15.0f, juce::Font::plain);
    transport_.setFont(juce::Font(mono));
    transport_.setColour(juce::Label::textColourId, kText);
    status_.setFont(juce::Font(mono.withHeight(13.0f)));
    status_.setColour(juce::Label::textColourId, kDim);

    beatsPerCycle_.setSliderStyle(juce::Slider::IncDecButtons);
    beatsPerCycle_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 44, 24);
    beatsPerCycle_.setTooltip("Temps par cycle : 1 cycle Strudel = N noires");

    code_.setMultiLine(true, false);
    code_.setReturnKeyStartsNewLine(true);
    code_.setTabKeyUsedAsCharacter(true);
    code_.setFont(juce::Font(mono));
    code_.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff1d1f24));
    code_.setColour(juce::TextEditor::textColourId, kText);
    code_.setText(proc_.code(), false);
    code_.onTextChange = [this] { proc_.setCode(code_.getText()); };
    code_.onEvaluate = [this] { proc_.evaluate(code_.getText()); };
    code_.onHush = [this] { proc_.hush(); };

    console_.setMultiLine(true, true);
    console_.setReadOnly(true);
    console_.setCaretVisible(false);
    console_.setFont(juce::Font(mono.withHeight(12.5f)));
    console_.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff111215));

    evalButton_.onClick = [this] { proc_.evaluate(code_.getText()); };
    hushButton_.onClick = [this] { proc_.hush(); };
    samplesButton_.onClick = [this] {
        if (library_) {
            library_.reset();
            return;
        }
        library_ = std::make_unique<LibraryPanel>([this] { proc_.reloadLibrary(); });
        library_->onClose = [this] { juce::MessageManager::callAsync([this] { library_.reset(); }); };
        addAndMakeVisible(*library_);
        resized();
    };

    for (auto* c : std::initializer_list<juce::Component*>{&transport_, &status_, &beatsPerCycle_, &freeRun_, &probe_,
                                                            &evalButton_, &hushButton_, &samplesButton_, &code_, &console_})
        addAndMakeVisible(c);

    beatsAttach_ = std::make_unique<SliderAttachment>(proc_.state(), "beatsPerCycle", beatsPerCycle_);
    freeRunAttach_ = std::make_unique<ButtonAttachment>(proc_.state(), "freeRun", freeRun_);
    probeAttach_ = std::make_unique<ButtonAttachment>(proc_.state(), "syncProbe", probe_);

    setResizable(true, true);
    setResizeLimits(560, 360, 2600, 1800);
    setSize(900, 600);
    startTimerHz(30);
    code_.grabKeyboardFocus();
}

ConchaEditor::~ConchaEditor()
{
    stopTimer();
    gl_.detach();
}

void ConchaEditor::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    g.setColour(kAccent);
    g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    g.drawText("Concha", 12, 8, 120, 28, juce::Justification::centredLeft);
}

void ConchaEditor::resized()
{
    auto r = getLocalBounds().reduced(12);
    auto top = r.removeFromTop(30);
    top.removeFromLeft(110);
    probe_.setBounds(top.removeFromRight(110));
    freeRun_.setBounds(top.removeFromRight(96));
    beatsPerCycle_.setBounds(top.removeFromRight(110));
    hushButton_.setBounds(top.removeFromRight(120).reduced(4, 2));
    evalButton_.setBounds(top.removeFromRight(170).reduced(4, 2));
    samplesButton_.setBounds(top.removeFromRight(90).reduced(4, 2));
    r.removeFromTop(6);
    transport_.setBounds(r.removeFromTop(22));
    status_.setBounds(r.removeFromTop(20));
    r.removeFromTop(4);
    console_.setBounds(r.removeFromBottom(juce::jmax(80, r.getHeight() / 4)));
    r.removeFromBottom(6);
    code_.setBounds(r);
    if (library_) library_->setBounds(code_.getBounds().getUnion(console_.getBounds()));
}

void ConchaEditor::appendConsole(const juce::String& line, juce::Colour colour)
{
    if (++consoleLines_ > 400) { // keep the console short
        console_.clear();
        consoleLines_ = 1;
    }
    console_.moveCaretToEnd();
    console_.setColour(juce::TextEditor::textColourId, colour);
    console_.insertTextAtCaret(line + "\n");
}

void ConchaEditor::timerCallback()
{
    const auto& v = proc_.transportView();
    transport_.setText(juce::String::formatted("%s%s  %6.2f BPM   ppq %9.3f   cycle %8.3f",
                                               v.playing ? (v.freeRunning ? "FREE" : "PLAY") : "STOP",
                                               v.looping ? " LOOP" : "", v.bpm.load(), v.ppq.load(), v.cycle.load()),
                       juce::dontSendNotification);

    const auto s = proc_.patternStatus();
    const auto& st = proc_.schedulerStats();
    juce::String line;
    if (!s.engineError.empty()) line = "moteur : " + juce::String(s.engineError);
    else if (!s.ready) line = "demarrage du moteur Strudel...";
    else {
        line << (s.evaluating ? "evaluation..." : s.lastOk ? "ok" : "erreur") << juce::String::formatted("  (%.0f ms)", s.lastEvalMs);
        if (code_.getText().toStdString() != s.activeCode) line << "  * modifie";
        line << juce::String::formatted("   voix %d", proc_.activeVoices());
        line << juce::String::formatted("   evenements %llu  retards %llu  sauts %llu  requete max %.1f ms",
                                        (unsigned long long)st.emitted.load(), (unsigned long long)st.late.load(),
                                        (unsigned long long)st.jumps.load(), s.worstQueryMs);
        if (s.droppedEvents) line << juce::String::formatted("  perdus %llu", (unsigned long long)s.droppedEvents);
    }
    status_.setText(line, juce::dontSendNotification);
    status_.setColour(juce::Label::textColourId, s.ready && !s.lastOk && s.evaluations ? kError : kDim);

    for (const auto& m : proc_.takeLog()) {
        const juce::String msg = juce::String::fromUTF8(m.message.c_str());
        if (msg.contains("@strudel/core loaded")) continue;
        appendConsole(msg, m.type == "error" ? kError : m.type == "warning" ? kWarning : kDim);
    }
}

} // namespace concha
