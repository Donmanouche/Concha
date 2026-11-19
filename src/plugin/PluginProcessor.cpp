#include "plugin/PluginProcessor.h"
#include "plugin/PluginEditor.h"

namespace concha {

namespace {
const char* const kDefaultCode = R"(// Concha: Ctrl+Entrée évalue, Ctrl+. coupe. Le son suit le transport du DAW.
// Samples : ajoute tes dossiers avec le bouton « Samples », puis s("bd") ou samples('Nom').
$: note("<c3 eb3 g3 bb2>*2").s("sawtooth").lpf(800).lpenv(2).decay(.2).sustain(0)
$: note("<[c4,eb4,g4] [bb3,d4,f4]>").s("triangle").attack(.05).release(.5).orbit(2)
// $: note("c4 e4 g4 b4").ccn(74).ccv(sine.segment(8)).midi()
)";

// Sink for the scheduler: MIDI into the host's buffer, sounds into superdough.
struct BlockSink final : AudioScheduler::Sink {
    juce::MidiBuffer& out;
    sd::Engine& engine;
    double sampleRate, beatsPerCycle;
    BlockSink(juce::MidiBuffer& m, sd::Engine& e, double sr, double n) : out(m), engine(e), sampleRate(sr), beatsPerCycle(n) {}
    void midi(int offset, const uint8_t* bytes, int length) override { out.addEvent(bytes, length, offset); }
    void sound(int offset, const SchedEvent& e, double spb) override
    {
        const double secondsPerBeat = spb / sampleRate;
        const double cps = 1.0 / (secondsPerBeat * beatsPerCycle);
        engine.trigger(offset, e.sound, e.durationBeats * secondsPerBeat, cps, e.cycle);
    }
};
} // namespace

juce::AudioProcessor::BusesProperties ConchaProcessor::makeBuses()
{
    auto props = BusesProperties().withOutput("Main", juce::AudioChannelSet::stereo(), true);
    for (int i = 1; i < kNumStereoBuses; ++i) // output i plays orbit i + 1
        props = props.withOutput("Orbit " + juce::String(i + 1), juce::AudioChannelSet::stereo(), true);
    return props;
}

ConchaProcessor::ConchaProcessor() : AudioProcessor(makeBuses()), state_(*this, nullptr, "Concha", createLayout())
{
    beatsPerCycle_ = state_.getRawParameterValue("beatsPerCycle");
    freeRun_ = state_.getRawParameterValue("freeRun");
    probe_ = state_.getRawParameterValue("syncProbe");
    for (int i = 0; i < 16; ++i) macros_[size_t(i)] = state_.getRawParameterValue("macro" + juce::String(i + 1));
    // scope / spectrum: the analysed voices (pattern thread)
    host_.analysers = [this](const std::string& name, int kind, std::vector<float>& out) -> int {
        const int id = sd::analyserSlot(name);
        const int size = superdough_.analysers().read(id, nullptr, 0);
        if (!size || kind == 0) return size;
        std::vector<float> time(size_t(size), 0.f);
        superdough_.analysers().read(id, time.data(), size);
        out.resize(size_t(size / 2));
        if (kind == 1) std::copy_n(time.begin(), size / 2, out.begin()); // the window's first half, as WebAudio
        else sd::analyserFrequencyData(time.data(), size, 0.5, analyserSmooth_[size_t(id) % analyserSmooth_.size()], out.data());
        return size;
    };
    // midin() and macro() read these at query time (pattern thread)
    host_.controls = [this](int kind, int a, int b) -> double {
        if (kind == 0 && a >= 0 && a < 128 && b >= 0 && b <= 16) return midiCC_[size_t(b * 128 + a)].load(std::memory_order_relaxed);
        if (kind == 1 && a >= 1 && a <= 16) return macros_[size_t(a - 1)]->load(std::memory_order_relaxed);
        return 0.0;
    };
    state_.state.setProperty("code", kDefaultCode, nullptr);
    worker_.start(); // the JS engines start on the pattern thread, the DAW is not blocked
    worker_.requestEvaluate(kDefaultCode);
}

ConchaProcessor::~ConchaProcessor() { worker_.stop(); }

PatternWorker::Options ConchaProcessor::workerOptions(lib::LibraryHost& host)
{
    PatternWorker::Options o;
    o.engine.host = &host;
    o.library = &host.library;
    o.midiNotes = &midiNotes_;
    o.onNewSynthDefs = [this] { // the user's new SynthDefs, to this instance's World
        std::lock_guard<std::mutex> lock(scHostMutex_);
        if (scHost_) scHost_->loadNewDefs();
    };
    return o;
}

// the sample / SynthDef folders changed: their sounds in the playing code, without evaluating it
void ConchaProcessor::reloadLibrary() { worker_.requestLibraryRefresh(); }

juce::AudioProcessorValueTreeState::ParameterLayout ConchaProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID{"beatsPerCycle", 1}, "Beats per cycle", 1, 32, 4));
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"freeRun", 1}, "Free run", false));
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"syncProbe", 1}, "Sync probe", false));
    for (int i = 1; i <= 16; ++i) // macro(n) in patterns
        layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{"macro" + juce::String(i), 1}, "Macro " + juce::String(i),
                                                               0.0f, 1.0f, 0.0f));
    for (int i = 1; i <= kNumMacros; ++i)
        layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{"macro" + juce::String(i), 1},
                                                               "Macro " + juce::String(i),
                                                               juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    return layout;
}

bool ConchaProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
    for (const auto& set : layouts.outputBuses)
        if (!set.isDisabled() && set != juce::AudioChannelSet::stereo()) return false;
    return layouts.inputBuses.isEmpty();
}

void ConchaProcessor::prepareToPlay(double sampleRate, int)
{
    sampleRate_ = sampleRate;
    probeGen_.prepare(sampleRate, kNumStereoBuses);
    scheduler_.prepare(sampleRate);
    superdough_.prepare(sampleRate);
    superdough_.setSampleStore(&lib::SampleLibrary::instance().store());
    // StrudelDirt's instruments: a scsynth World for this instance, at the host's rate
    superdough_.setSynthServer(nullptr);
    superdough_.setFxServer(nullptr);
    superdough_.setOrbitFxServer(nullptr);
    {
        std::lock_guard<std::mutex> lock(scHostMutex_);
        scHost_ = std::make_unique<sc::ScHost>(sampleRate);
    }
    if (scHost_->ok()) superdough_.setSynthServer(scHost_.get());
    scFx_ = std::make_unique<sc::ScFx>(sampleRate); // StrudelDirt's effect modules
    if (scFx_->ok()) superdough_.setFxServer(scFx_.get());
    scOrbitFx_ = std::make_unique<sc::ScOrbitFx>(sampleRate); // StrudelDirt's orbit effects
    if (scOrbitFx_->ok()) superdough_.setOrbitFxServer(scOrbitFx_.get());
    freeRunPpq_ = 0.0;
    midiInput_.ensureSize(8192);
}

TransportSnapshot ConchaProcessor::readTransport(int numSamples, HostPosition& host)
{
    TransportSnapshot t;
    bool hostPlaying = false;
    if (auto* ph = getPlayHead()) {
        if (auto pos = ph->getPosition()) {
            if (auto bpm = pos->getBpm()) t.bpm = *bpm;
            if (auto ppq = pos->getPpqPosition()) t.ppq = *ppq;
            if (auto sig = pos->getTimeSignature())
                t.beatsPerBar = std::max(1, sig->numerator * 4 / std::max(1, sig->denominator));
            hostPlaying = pos->getIsPlaying();
            host.looping = pos->getIsLooping();
            if (auto loop = pos->getLoopPoints()) {
                host.loopStart = loop->ppqStart;
                host.loopEnd = loop->ppqEnd;
            } else {
                host.looping = false;
            }
        }
    }
    const bool freeRun = freeRun_->load() > 0.5f && !hostPlaying;
    if (freeRun) {
        t.ppq = freeRunPpq_;
        freeRunPpq_ += numSamples * t.bpm / (60.0 * sampleRate_);
        host.looping = false;
    } else {
        // Where the next block starts: if the DAW stops while Free run is on, the counter takes over
        // from there, without a jump.
        freeRunPpq_ = t.ppq + (hostPlaying ? numSamples * t.bpm / (60.0 * sampleRate_) : 0.0);
    }
    t.playing = hostPlaying || freeRun;
    host.playing = t.playing;
    host.ppq = t.ppq;
    host.bpm = t.bpm;

    view_.ppq = t.ppq;
    view_.bpm = t.bpm;
    view_.cycle = t.ppq / std::max(1.0f, beatsPerCycle_->load());
    view_.playing = t.playing;
    view_.freeRunning = freeRun;
    view_.looping = host.looping;
    return t;
}

void ConchaProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    buffer.clear();
    midiInput_.clear(); // read once the block's transport is known (preallocated: no allocation here)
    midiInput_.addEvents(midi, 0, -1, 0);
    midi.clear();

    // Per-bus channel pointers (disabled buses stay null).
    std::array<juce::AudioBuffer<float>, kNumStereoBuses> views;
    std::array<juce::AudioBuffer<float>*, kNumStereoBuses> buses{};
    std::array<std::array<float*, 2>, kNumStereoBuses> channels{};
    std::array<float* const*, kNumStereoBuses> busChannels{};
    const int numBuses = std::min(getBusCount(false), kNumStereoBuses);
    for (int b = 0; b < numBuses; ++b) {
        const auto i = static_cast<size_t>(b);
        busChannels[i] = channels[i].data();
        if (getBus(false, b) == nullptr || !getBus(false, b)->isEnabled()) continue;
        views[i] = getBusBuffer(buffer, false, b);
        if (views[i].getNumChannels() == 0) continue;
        buses[i] = &views[i];
        channels[i][0] = views[i].getWritePointer(0);
        channels[i][1] = views[i].getNumChannels() > 1 ? views[i].getWritePointer(1) : nullptr;
    }

    HostPosition host;
    const auto t = readTransport(numSamples, host);

    const double N = std::max(1.0f, beatsPerCycle_->load());
    BlockSink sink(midi, superdough_, sampleRate_, N);
    scheduler_.process(host, numSamples, N, sink);
    readMidiInput(midiInput_, numSamples);
    superdough_.render(busChannels.data(), numBuses, numSamples);
    for (int b = 0; b < numBuses; ++b) { // meters
        const auto i = static_cast<size_t>(b);
        if (!buses[i]) continue;
        float peak = 0.f;
        for (int c = 0; c < buses[i]->getNumChannels(); ++c) peak = std::max(peak, buses[i]->getMagnitude(c, 0, numSamples));
        float prev = peaks_[i].load(std::memory_order_relaxed);
        while (peak > prev && !peaks_[i].compare_exchange_weak(prev, peak, std::memory_order_relaxed)) {}
    }

    if (probe_->load() > 0.5f) probeGen_.process(t, numSamples, buses.data(), numBuses, midi);
}

// The DAW's MIDI input: CCs into midiCC_ (midin), note-ons to the pattern thread (midikeys) at their
// position plus 10 ms, as Strudel's midikeys (latencySeconds 0.01).
void ConchaProcessor::readMidiInput(const juce::MidiBuffer& midi, int numSamples)
{
    if (midi.isEmpty()) return;
    const TransportState ts = transport_.load(); // this block's, as the scheduler published it
    for (const auto m : midi) {
        const auto msg = m.getMessage();
        const int chan = msg.getChannel(); // 1..16, 0 for system messages
        if (msg.isController() && chan >= 1) {
            const float v = float(msg.getControllerValue()) / 127.0f;
            const int cc = msg.getControllerNumber();
            midiCC_[size_t(cc)].store(v, std::memory_order_relaxed);
            midiCC_[size_t(chan * 128 + cc)].store(v, std::memory_order_relaxed);
        } else if (msg.isNoteOn() && ts.playing) {
            MidiInNote n;
            n.epoch = ts.epoch;
            n.lap = ts.lap;
            const double offset = std::clamp(m.samplePosition, 0, std::max(0, numSamples - 1)) / sampleRate_ + 0.01;
            n.ppq = ts.ppq + offset * ts.bpm / 60.0;
            n.note = uint8_t(msg.getNoteNumber());
            n.velocity = msg.getVelocity();
            n.channel = uint8_t(chan);
            midiNotes_.push(n); // full (256 notes waiting): dropped
        }
    }
}

// As Shadertoy's audio input (a WebAudio analyser, fftSize 1024, smoothing 0.8): row 0
// getByteFrequencyData (-100..-30 dB), row 1 getByteTimeDomainData.
void ConchaProcessor::shaderAudio(std::vector<float>& spectrum, std::vector<float>& wave)
{
    std::vector<float> time(1024, 0.f);
    spectrum.assign(512, 0.f);
    wave.assign(512, 0.5f);
    if (!superdough_.analysers().read(0, time.data(), 1024)) return;
    std::vector<float> db(512);
    sd::analyserFrequencyData(time.data(), 1024, 0.8, shaderSmooth_, db.data());
    for (int i = 0; i < 512; ++i) {
        spectrum[size_t(i)] = juce::jlimit(0.0f, 1.0f, (db[size_t(i)] + 100.0f) / 70.0f);
        wave[size_t(i)] = juce::jlimit(0.0f, 1.0f, time[size_t(512 + i)] * 0.5f + 0.5f);
    }
}

int ConchaProcessor::uiAnalyser(const std::string& name, bool spectrum, std::vector<float>& out)
{
    const int id = sd::analyserSlot(name);
    const int size = superdough_.analysers().read(id, nullptr, 0);
    if (!size) return 0;
    std::vector<float> time(static_cast<size_t>(size));
    superdough_.analysers().read(id, time.data(), size);
    if (!spectrum) {
        out = std::move(time);
        return size;
    }
    out.resize(size_t(size / 2));
    sd::analyserFrequencyData(time.data(), size, 0.5, uiSmooth_[name], out.data());
    return size;
}

void ConchaProcessor::evaluate(const juce::String& code)
{
    setCode(code);
    worker_.requestEvaluate(code.toStdString());
}

void ConchaProcessor::hush() { worker_.requestEvaluate("silence"); }

juce::String ConchaProcessor::code() const { return state_.state.getProperty("code").toString(); }

void ConchaProcessor::setCode(const juce::String& code) { state_.state.setProperty("code", code, nullptr); }

void ConchaProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // midin's last values, as Strudel keeps them in localStorage: "chan:cc:value ..."
    juce::String cc;
    for (int i = 0; i < 17 * 128; ++i)
        if (const float v = midiCC_[size_t(i)].load(); v != 0.f) cc << (i / 128) << ':' << (i % 128) << ':' << v << ' ';
    state_.state.setProperty("midiCC", cc.trim(), nullptr);
    if (auto xml = state_.copyState().createXml()) copyXmlToBinary(*xml, destData);
}

void ConchaProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes)) {
        if (xml->hasTagName(state_.state.getType())) {
            state_.replaceState(juce::ValueTree::fromXml(*xml));
            for (auto& v : midiCC_) v.store(0.f);
            for (const auto& item : juce::StringArray::fromTokens(state_.state.getProperty("midiCC").toString(), " ", "")) {
                const auto parts = juce::StringArray::fromTokens(item, ":", "");
                if (parts.size() != 3) continue;
                const int chan = parts[0].getIntValue(), cc = parts[1].getIntValue();
                if (chan >= 0 && chan <= 16 && cc >= 0 && cc < 128) midiCC_[size_t(chan * 128 + cc)].store(parts[2].getFloatValue());
            }
            worker_.requestEvaluate(code().toStdString()); // the project's code plays again
        }
    }
}

juce::AudioProcessorEditor* ConchaProcessor::createEditor() { return new ConchaEditor(*this); }

} // namespace concha

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new concha::ConchaProcessor(); }
