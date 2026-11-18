#pragma once

#include "library/LibraryHost.h"
#include "plugin/SyncProbe.h"
#include "sched/AudioScheduler.h"
#include "sched/PatternWorker.h"
#include "sc/ScFx.h"
#include "sc/ScOrbitFx.h"
#include "sc/ScHost.h"
#include "superdough/Engine.h"

#include <mutex>
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>

namespace concha {

inline constexpr int kNumStereoBuses = 16; // output k = orbit k + 1 (orbit 0 and 1: Main), as superdough's multichannel orbits
inline constexpr int kNumMacros = 16;

// What the editor shows; written by the audio thread, read by the UI timer.
struct TransportView {
    std::atomic<double> ppq{0.0}, bpm{120.0}, cycle{0.0};
    std::atomic<bool> playing{false}, freeRunning{false}, looping{false};
};

class ConchaProcessor final : public juce::AudioProcessor {
public:
    ConchaProcessor();
    ~ConchaProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    using AudioProcessor::processBlock;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Concha"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& state() { return state_; }
    const TransportView& transportView() const { return view_; }

    // The Strudel source as typed in the editor; saved with the DAW project.
    juce::String code() const;
    void setCode(const juce::String& code);

    // Evaluates code (Ctrl+Enter); the music keeps playing the previous code meanwhile.
    void evaluate(const juce::String& code);
    void hush(); // Ctrl+.

    PatternWorker::Status patternStatus() const { return worker_.status(); }
    int activeVoices() const { return superdough_.activeVoices(); }
    // After the library folders changed: evaluate the current code again (autoload).
    void reloadLibrary();
    std::vector<LogMessage> takeLog()
    {
        auto log = worker_.takeLog();
        for (auto& w : superdough_.takeWarnings()) log.push_back(LogMessage{"warning", std::move(w)});
        return log;
    }
    const AudioScheduler::Stats& schedulerStats() const { return scheduler_.stats(); }
    PatternWorker::ActiveCodeInfo activeCodeInfo() const { return worker_.activeCodeInfo(); }
    void setSlider(const juce::String& id, double value) { worker_.setSlider(id.toStdString(), value); }
    // For the editor: haps playing at `cycle`, and the output peaks since the last call.
    std::vector<PatternWorker::VisualHap> activeHaps(double cycle) { return worker_.activeHaps(cycle); }
    // Strudel's visuals (phase 8): the window asks for frames while it is open
    void setVisuals(bool wanted, int width, int height, double pixelRatio) { worker_.setVisuals(wanted, width, height, pixelRatio); }
    std::vector<PatternWorker::Frame> takeFrames() { return worker_.takeFrames(); }
    // the main output for shaders: Shadertoy's audio texture rows (512 values, 0..1). Message thread.
    void shaderAudio(std::vector<float>& spectrum, std::vector<float>& wave);
    float beatsPerCycle() const { return beatsPerCycle_->load(); }
    std::array<float, kNumStereoBuses> takePeaks()
    {
        std::array<float, kNumStereoBuses> out{};
        for (size_t i = 0; i < out.size(); ++i) out[i] = peaks_[i].exchange(0.f, std::memory_order_relaxed);
        return out;
    }

private:
    static BusesProperties makeBuses();
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    TransportSnapshot readTransport(int numSamples, HostPosition& host);

    juce::AudioProcessorValueTreeState state_;
    std::atomic<float>* beatsPerCycle_ = nullptr;
    std::atomic<float>* freeRun_ = nullptr;
    std::atomic<float>* probe_ = nullptr;

    SpscRing<SchedEvent> toAudio_{8192};
    SpscRing<MidiInNote> midiNotes_{256}; // DAW MIDI in note-ons -> pattern thread (midikeys)
    // DAW MIDI in CCs (midin): [channel 0 = any, 1..16][cc], 0..1; saved with the project
    std::array<std::atomic<float>, 17 * 128> midiCC_{};
    std::array<std::atomic<float>*, 16> macros_{};
    void readMidiInput(const juce::MidiBuffer& midi, int numSamples);
    juce::MidiBuffer midiInput_;
    std::array<std::vector<float>, 16> analyserSmooth_;
    std::vector<float> shaderSmooth_; // spectrum smoothing per analyser id (pattern thread)
    SeqLock<TransportState> transport_;
    SeqLock<SwapPoint> swap_;
    lib::LibraryHost host_{lib::SampleLibrary::instance()};
    PatternWorker::Options workerOptions(lib::LibraryHost& host);
    PatternWorker worker_{toAudio_, transport_, swap_, workerOptions(host_)};
    AudioScheduler scheduler_{toAudio_, transport_, swap_};
    sd::Engine superdough_;
    std::unique_ptr<sc::ScHost> scHost_; // after superdough_: destroyed first
    std::mutex scHostMutex_;             // scHost_ replaced (prepareToPlay) / sent new SynthDefs (pattern thread)
    std::unique_ptr<sc::ScFx> scFx_;
    std::unique_ptr<sc::ScOrbitFx> scOrbitFx_;

    SyncProbe probeGen_;
    double sampleRate_ = 48000.0;
    double freeRunPpq_ = 0.0;
    TransportView view_;
    std::array<std::atomic<float>, kNumStereoBuses> peaks_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ConchaProcessor)
};

} // namespace concha
