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
    o.onNewSynthDefs = [this] { // the user's new SynthDefs, to this instance's World
        std::lock_guard<std::mutex> lock(scHostMutex_);
        if (scHost_) scHost_->loadNewDefs();
    };
    return o;
}

void ConchaProcessor::reloadLibrary() { worker_.requestEvaluate(code().toStdString()); }

juce::AudioProcessorValueTreeState::ParameterLayout ConchaProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID{"beatsPerCycle", 1}, "Beats per cycle", 1, 32, 4));
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"freeRun", 1}, "Free run", false));
    layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"syncProbe", 1}, "Sync probe", false));
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
    midi.clear(); // incoming MIDI: phase 7

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
    if (auto xml = state_.copyState().createXml()) copyXmlToBinary(*xml, destData);
}

void ConchaProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes)) {
        if (xml->hasTagName(state_.state.getType())) {
            state_.replaceState(juce::ValueTree::fromXml(*xml));
            worker_.requestEvaluate(code().toStdString()); // the project's code plays again
        }
    }
}

juce::AudioProcessorEditor* ConchaProcessor::createEditor() { return new ConchaEditor(*this); }

} // namespace concha

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new concha::ConchaProcessor(); }
