#include "plugin/PluginEditor.h"

#include "plugin/ui/HostWindow.h"
#include "plugin/ui/Logo.h"
#include "plugin/ui/Theme.h"

namespace concha {

using namespace ui;

// The interface, at its logical size, scaled by the zoom inside the host's window.
class ConchaEditor::Content final : public juce::Component {
public:
    explicit Content(ConchaEditor& e) : editor_(e) {}
    void paint(juce::Graphics& g) override { editor_.paintChrome(g, getLocalBounds()); }
    void resized() override { editor_.layout(getLocalBounds()); }

private:
    ConchaEditor& editor_;
};

ConchaEditor::ConchaEditor(ConchaProcessor& p) : AudioProcessorEditor(p), proc_(p)
{
    setLookAndFeel(&lookAndFeel_);
    content_ = std::make_unique<Content>(*this);
    zoomSetting_ = loadZoomSetting();
    zoom_ = zoomSetting_ > 0 ? zoomSetting_ : systemScale({}, 1.0);
    // OpenGL paints the editor only while a shader is drawn behind the code (attached then): JUCE's
    // OpenGL thread takes the message thread's lock to paint components and keeps it while the
    // display server hands it a buffer, which froze typing. The text is painted in software.
    gl_.setComponentPaintingEnabled(true);
    gl_.setRenderer(this);

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
    content_->addAndMakeVisible(code_);
    content_->addChildComponent(strip_);

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
        content_->addAndMakeVisible(c);

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
    content_->addAndMakeVisible(tabs_);
    addAndMakeVisible(*content_);
    applySettings();

    setResizable(true, true);
    settings_->setZoom(zoomSetting_, zoom_);
    settings_->onZoom = [this](double z) { setZoomSetting(z); };
    code_.onWindowKey = [this](const juce::KeyPress& k) { return zoomKey(k); };
    applyZoom();
    startTimerHz(60);
    code_.grabKeyboardFocus();
}

ConchaEditor::~ConchaEditor()
{
    stopTimer();
    proc_.setVisuals(false, 1, 1, 1.0); // no window: no frames
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
    content_->resized(); // the layout, at the same size
}

void ConchaEditor::showFolders()
{
    if (library_) {
        library_.reset();
        return;
    }
    library_ = std::make_unique<LibraryPanel>([this] { proc_.reloadLibrary(); });
    library_->onClose = [this] { juce::MessageManager::callAsync([this] { library_.reset(); }); };
    content_->addAndMakeVisible(*library_);
    content_->resized();
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
    // under the scaled content (its rounding edge); the shader shows through the code
    if (shader_.active()) g.excludeClipRegion(getLocalArea(&code_, code_.getLocalBounds()));
    g.fillAll(theme::background);
}

void ConchaEditor::paintChrome(juce::Graphics& g, juce::Rectangle<int> area)
{
    if (shader_.active()) g.excludeClipRegion(code_.getBounds()); // the shader shows through there
    if (strip_.isVisible()) g.excludeClipRegion(strip_.getBounds()); // the band paints itself (opaque)
    g.fillAll(theme::background);
    auto top = area.removeFromTop(44);
    g.setColour(theme::panel);
    g.fillRect(top);
    g.setColour(theme::panelLine);
    g.drawHorizontalLine(top.getBottom() - 1, 0.f, (float)area.getWidth());
    paintLogo(g, {14.f, 0.f, 150.f, 44.f}, theme::text);
    auto bottom = area.removeFromBottom(24);
    g.setColour(theme::panel);
    g.fillRect(bottom);
    g.setColour(theme::panelLine);
    g.drawHorizontalLine(bottom.getY(), 0.f, (float)area.getWidth());
}

void ConchaEditor::resized()
{
    if (!content_) return;
    content_->setTransform(juce::AffineTransform::scale(float(zoom_)));
    content_->setBounds(0, 0, juce::roundToInt(getWidth() / zoom_), juce::roundToInt(getHeight() / zoom_));
    auto& ui = proc_.state().state;
    ui.setProperty("width", content_->getWidth(), nullptr); // the logical size, without the zoom
    ui.setProperty("height", content_->getHeight(), nullptr);
}

void ConchaEditor::layout(juce::Rectangle<int> area)
{
    auto r = area;
    auto top = r.removeFromTop(44).reduced(8, 7);
    top.removeFromLeft(150); // the logo
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
        tabs_.setBounds(r.removeFromRight(juce::jlimit(210, 392, r.getWidth() * 25 / 100)));
    } else {
        tabs_.setVisible(false);
    }
    strip_.setVisible(stripWanted_); // the oscilloscope band, when the code asks for it
    if (stripWanted_) strip_.setBounds(r.removeFromBottom(juce::jlimit(85, 160, r.getHeight() * 17 / 100))); // the panel closed: the whole width
    code_.setBounds(r.reduced(10, 6)); // strudel.cc's editor has some air around the text
    if (library_) library_->setBounds(r);
}

// ---- zoom ----

void ConchaEditor::applyZoom()
{
    auto& ui = proc_.state().state;
    const int w = juce::jlimit(640, 3000, (int)ui.getProperty("width", 1100)), h = juce::jlimit(400, 2000, (int)ui.getProperty("height", 680));
    setResizeLimits(juce::roundToInt(640 * zoom_), juce::roundToInt(400 * zoom_), juce::roundToInt(3000 * zoom_), juce::roundToInt(2000 * zoom_));
    const int pw = juce::roundToInt(w * zoom_), ph = juce::roundToInt(h * zoom_);
    if (getWidth() == pw && getHeight() == ph) {
        resized();
        content_->resized();
    }
    else setSize(pw, ph);
    repaint();
}

void ConchaEditor::followHostWindow()
{
    if (proc_.wrapperType == juce::AudioProcessor::wrapperType_Standalone) return; // its own window resizes it
    HostWindowSizes s;
    if (!hostWindowSizes(getPeer(), s) || s.ownW <= 0 || s.ownH <= 0 || getWidth() <= 0 || getHeight() <= 0) return;
    const bool first = lastParentW_ < 0;
    const bool changed = s.parentW != lastParentW_ || s.parentH != lastParentH_;
    lastParentW_ = s.parentW, lastParentH_ = s.parentH;
    if (!changed) return; // only a change of the host's window: never fight a resize we asked for
    if (std::abs(s.parentW - s.ownW) <= 2 && std::abs(s.parentH - s.ownH) <= 2) return;
    if (first && (s.parentW < s.ownW || s.parentH < s.ownH)) return; // opening: only grow
    const auto* top = getTopLevelComponent();
    const double rx = double(s.ownW) / top->getWidth(), ry = double(s.ownH) / top->getHeight();
    int w = juce::roundToInt(s.parentW / rx), h = juce::roundToInt(s.parentH / ry);
    if (auto* c = getConstrainer()) {
        w = juce::jlimit(c->getMinimumWidth(), c->getMaximumWidth(), w);
        h = juce::jlimit(c->getMinimumHeight(), c->getMaximumHeight(), h);
    }
    if (w != getWidth() || h != getHeight()) setSize(w, h);
}

void ConchaEditor::setZoomSetting(double setting, bool save)
{
    zoomSetting_ = setting <= 0 ? 0 : juce::jlimit(kZoomMin, kZoomMax, setting);
    if (save) saveZoomSetting(zoomSetting_); // only a choice of the user is written
    double next = zoomSetting_;
    if (next <= 0) { // auto: the system's scale, unless JUCE already scales this window
        const auto* peer = getPeer();
        next = peer && peer->getPlatformScaleFactor() > 1.01 ? 1.0 : systemScale(getScreenBounds(), hostScale_);
    }
    settings_->setZoom(zoomSetting_, next);
    if (std::abs(next - zoom_) < 1e-3) return;
    // keep the logical size: the window grows or shrinks with the zoom
    zoom_ = next;
    applyZoom();
}

void ConchaEditor::setScaleFactor(float newScale)
{
    hostScale_ = newScale; // the host scales nothing itself here: the zoom does (auto)
    if (zoomSetting_ <= 0) setZoomSetting(0, false);
}

void ConchaEditor::parentHierarchyChanged()
{
    if (zoomSetting_ <= 0 && getPeer()) setZoomSetting(0, false); // the window is on a screen now: its scale
}

bool ConchaEditor::zoomKey(const juce::KeyPress& key)
{
    if (!key.getModifiers().isCommandDown()) return false;
    const auto c = key.getTextCharacter();
    const int code = key.getKeyCode();
    static const double steps[] = {1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5};
    if (c == '=' || c == '+' || code == '=' || code == '+') {
        for (double s : steps)
            if (s > zoom_ + 1e-3) return setZoomSetting(s), true;
        return true;
    }
    if (c == '-' || code == '-') {
        for (int i = int(std::size(steps)) - 1; i >= 0; --i)
            if (steps[i] < zoom_ - 1e-3) return setZoomSetting(steps[i]), true;
        return true;
    }
    if (c == '0' || code == '0') return setZoomSetting(0), true; // back to auto
    return false;
}

bool ConchaEditor::keyPressed(const juce::KeyPress& key) { return zoomKey(key); }

void ConchaEditor::updateShader(const std::vector<PatternWorker::VisualHap>& haps, double cycle, double cps)
{
    ui::ShaderLayer::Uniforms u;
    u.cycle = cycle;
    u.cps = cps;
    u.time = cps > 0 ? cycle / cps : 0;
    for (const auto& h : haps) // .uniform(name): the latest onset at or before now
        if (!h.uniform.empty() && h.begin <= cycle) {
            auto& o = uniformOnsets_[h.uniform];
            if (h.begin >= o.first || o.first > cycle) o = {h.begin, h.uniformValue};
        }
    for (const auto& [name, o] : uniformOnsets_) u.haps[name] = {float(cps > 0 ? (cycle - o.first) / cps : 0), float(o.second)};
    proc_.shaderAudio(u.spectrum, u.wave);
    shader_.setUniforms(std::move(u));
    gl_.triggerRepaint();
}

// the band's data at the DAW's position: the channels' haps, the analysers
void ConchaEditor::updateStrip()
{
    const auto& v = proc_.transportView();
    ui::ScopeStrip::Model m = stripModel_;
    m.cycle = v.cycle.load();
    m.sampleRate = proc_.sampleRate();
    if (m.channels) {
        uint32_t generation = 0;
        for (const auto& h : proc_.channelHaps(m.cycle - 1.3, m.cycle + 1.3, generation))
            m.haps.push_back({h.begin, h.end, h.block, h.note, h.gain, h.s});
    }
    if (m.scope) proc_.uiAnalyser(stripScope_, false, m.scopeData);
    if (m.fft) proc_.uiAnalyser(stripFft_, true, m.fftDb);
    strip_.update(m); // repaints the band only
}

void ConchaEditor::renderOpenGL()
{
    juce::Rectangle<int> area;
    {
        std::lock_guard<std::mutex> lock(shaderAreaMutex_);
        area = shaderArea_;
    }
    const double scale = gl_.getRenderingScale();
    const auto target = getLocalBounds().toDouble() * scale; // getLocalBounds: set on the message thread, read only
    const auto vp = (area.toDouble() * scale).toNearestInt();
    const int targetHeight = int(std::lround(target.getHeight()));
    shader_.render(gl_, vp, targetHeight);
    if (onGLFrame) {
        using namespace juce::gl;
        const int w = int(std::lround(target.getWidth())), h = targetHeight;
        std::vector<uint8_t> px(size_t(w * h * 4));
        glViewport(0, 0, w, h);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        juce::Image img(juce::Image::ARGB, w, h, false);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t* p = px.data() + ((h - 1 - y) * w + x) * 4;
                img.setPixelAt(x, y, juce::Colour(p[0], p[1], p[2]));
            }
        onGLFrame(img);
    }
}

void ConchaEditor::timerCallback()
{
    const auto& v = proc_.transportView();
    const auto s = proc_.patternStatus();
    if (++hostTicks_ % 15 == 0) followHostWindow();

    // after an evaluation: the evaluated code's mini-notation ranges, the known sounds
    if (s.activeGeneration != shownGeneration_) {
        const auto info = proc_.activeCodeInfo();
        shownGeneration_ = info.generation;
        code_.setMiniLocations(info.miniLocations);
        std::vector<CodeView::SliderInfo> sliders;
        for (const auto& x : info.sliders)
            sliders.push_back({x.from, x.to, juce::String::fromUTF8(x.value.c_str()), x.min, x.max, x.step});
        code_.setSliders(sliders);
        // the oscilloscope band: what the code asks for
        stripModel_ = {};
        stripModel_.channels = info.visuals.channels;
        stripModel_.scope = !info.visuals.scope.empty();
        stripModel_.fft = !info.visuals.fft.empty();
        for (const auto& b : info.blocks) stripModel_.blockNames.push_back(b.name);
        stripScope_ = info.visuals.scope.empty() ? std::string() : info.visuals.scope.front();
        stripFft_ = info.visuals.fft.empty() ? std::string() : info.visuals.fft.front();
        if (strip_.wanted(stripModel_) != stripWanted_) {
            stripWanted_ = !stripWanted_;
            content_->resized();
        }
        shader_.setSource(info.shader);
        if (!info.shader.empty() && !gl_.isAttached()) gl_.attachTo(*this);
        if (info.shader.empty() && gl_.isAttached()) gl_.detach();
        code_.setShaderActive(!info.shader.empty());
        uniformOnsets_.clear();
    }
    // the known sounds: after an evaluation, or when the sample folders changed
    if (s.soundsVersion != shownSounds_) {
        shownSounds_ = s.soundsVersion;
        juce::StringArray names;
        for (const auto& n : proc_.activeCodeInfo().soundNames) names.add(juce::String::fromUTF8(n.c_str()));
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
    code_.followScroll(); // sliders over their call, whichever way the code scrolled
    // Strudel's visuals: the code view's size, the frames drawn since the last tick
    proc_.setVisuals(true, code_.getWidth(), code_.getHeight(), 1.0); // the editor exists while its window is open
    {
        std::lock_guard<std::mutex> lock(shaderAreaMutex_);
        shaderArea_ = getLocalArea(&code_, code_.getLocalBounds());
    }
    if (shader_.active()) {
        const double N = std::max(1.0, double(proc_.beatsPerCycle()));
        std::vector<PatternWorker::VisualHap> haps;
        if (v.playing.load()) haps = proc_.activeHaps(v.cycle.load());
        updateShader(haps, v.cycle.load(), v.bpm.load() / 60.0 / N);
    }
    for (const auto& e : shader_.takeErrors()) console_->add(juce::String::fromUTF8(e.c_str()), theme::error);
    {
        std::lock_guard<std::mutex> lock(shaderAreaMutex_);
        stripArea_ = strip_.isVisible() ? getLocalArea(&strip_, strip_.getLocalBounds()) : juce::Rectangle<int>();
    }
    if (stripWanted_ && (ticks_ % 3 == 0)) updateStrip(); // 20 frames a second: enough for the band, a third of the painting
    if (auto frames = proc_.takeFrames(); !frames.empty()) code_.addFrames(frames);

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
