#pragma once

// superdough in C++ (packages/superdough, AGPL-3.0, Strudel contributors): one voice per sound
// event, built like superdough() builds its WebAudio graph, then the orbits
// (superdoughoutput.mjs Orbit):
//
//   source -> gain -> lpf -> hpf -> bpf -> vowel -> coarse -> crush -> shape -> distort
//          -> tremolo -> pan -> phaser -> FXrelease -> postgain -+-> orbit (or dry gain)
//                                                                +-> orbit delay send (delay)
//                                                                +-> orbit reverb send (room)
//   orbit: sum + delay + reverb -> djf -> output gain (duckorbit) -> stereo output
//
// Sources: synths (sine, square, sawtooth, triangle, one, user / partials, supersaw, pulse, sbd),
// noises, samples. Worklet nodes (lfo, coarse, ladder, supersaw, pulse...) run in 128-sample
// render quanta aligned on the engine's absolute clock, as AudioWorklets do on the context's.
//
// Orbits: orbit k (k >= 1) plays on stereo output k - 1, orbit 0 on output 0, as superdough with
// multiChannelOrbits; .channels() overrides it when the orbit is created. Max polyphony 128: the
// oldest voices fade out in 0.25 s. Real-time safe: no allocation after prepare() (reverb impulse
// responses are built on a helper thread).

#include "superdough/Analyser.h"
#include "superdough/AudioParam.h"
#include "superdough/Convolver.h"
#include "superdough/FxChain.h"
#include "superdough/Kabel.h"
#include "superdough/PhaseVocoder.h"
#include "superdough/Nodes.h"
#include "superdough/SampleStore.h"
#include "superdough/SoundParams.h"

#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace concha::sd {

class Engine {
public:
    static constexpr int kMaxVoices = 192;    // 128 playing + voices fading out
    static constexpr int kMaxPolyphony = 128; // superdough DEFAULT_MAX_POLYPHONY
    static constexpr int kMaxFilters = 6;
    static constexpr int kMaxOrbits = 32;
    static constexpr int kMaxBlock = 8192;

    Engine();
    ~Engine();

    // synchronousReverb: build reverb impulse responses inline (offline renders, tests).
    void prepare(double sampleRate, bool synchronousReverb = false);
    void setSampleStore(const SampleStore* store) { store_ = store; }
    // StrudelDirt's instruments (src/sc/ScHost); none: those sounds are skipped. Set before playing.
    void setSynthServer(SynthServer* server) { synthServer_ = server; }
    // StrudelDirt's effect modules (src/sc/ScFx); none: those effects are ignored.
    void setFxServer(FxServer* server) { fxServer_ = server; }
    // StrudelDirt's orbit effects (src/sc/ScOrbitFx): leslie, shimmer.
    void setOrbitFxServer(OrbitFxServer* server) { orbitFx_ = server; }
    // Live controls (modulators of kind 4): the current value, 0..1, of a MIDI CC (kind 0, a = cc,
    // b = channel or 0) or a macro (kind 1, a = 1..16). Called on the audio thread: lock-free reads only.
    void setLiveControls(std::function<double(int kind, int a, int b)> read) { liveControl_ = std::move(read); }
    // `.analyze(id)`: the analysed voices' sound, for the visuals (any thread reads it)
    const AnalyserTaps& analysers() const { return *analysers_; }

    // A StrudelDirt instrument's time span and envelope (DirtEvent.calcTimeSpan, strudel_envelope,
    // dirt_gate) for a hap of `duration` seconds: what the voice applies.
    struct ScEnvelope {
        double sustain = 0, total = 0; // the gate's hold time; dirt_gate's totalDuration
        double attack = 0.001, decay = 0, level = 1, release = 0.01, curve = -2, amp = 1;
    };
    static ScEnvelope scEnvelope(const SoundParams& p, double duration);

    // Audio thread, before render(): a voice starting `offset` samples into the coming block.
    void trigger(int offset, const SoundParams& p, double durationSeconds, double cps, double cycle);
    // buses[b][0..1]: stereo outputs (null channel pointers are skipped). Adds into them.
    void render(float* const* const* buses, int numBuses, int numSamples);
    void reset();

    int activeVoices() const { return active_.load(std::memory_order_relaxed); }
    uint64_t missingSamples() const { return missingSamples_.load(std::memory_order_relaxed); }
    // Resources that ran out since the last call (K() graphs, FX stages, vocoders...), as console
    // warnings; each kind at most every 10 s. Not the audio thread.
    std::vector<std::string> takeWarnings();

    struct Voice;
    struct Orbit;
    std::unique_ptr<ImpulseResponse> buildImpulse(const double* params, uint32_t irId, unsigned& seed);

private:
    bool start(Voice& v, int offset, const SoundParams& p, double duration, double cps, double cycle);
    Orbit* getOrbit(int number, int channel);
    void renderOrbits(float* const* const* buses, int numBuses, int numSamples);
    void builderLoop();

    double sampleRate_ = 48000.0;
    std::function<double(int, int, int)> liveControl_;
    bool synchronousReverb_ = false;
    int64_t absSample_ = 0; // engine clock: samples rendered since prepare()
    std::unique_ptr<PeriodicWaves> waves_;
    std::unique_ptr<NoiseBuffers> noises_;
    std::vector<float> sbdCurve_; // makeSaturationCurve(2, sampleRate)
    const SampleStore* store_ = nullptr;
    SynthServer* synthServer_ = nullptr;
    FxServer* fxServer_ = nullptr;
    OrbitFxServer* orbitFx_ = nullptr;
    std::unique_ptr<AnalyserTaps> analysers_ = std::make_unique<AnalyserTaps>();
    // a StrudelDirt effects chain's way back to its orbit (index = the server's slot)
    struct FxDrain {
        bool active = false, voiceDone = false;
        int64_t start = 0, end = 0; // its voice's input, absolute samples
        Orbit* orbit = nullptr;
        double dry = 1, delay = 0, room = 0, busGain = 1;
        int busOut = -1;
    };
    static constexpr int kMaxFxChains = 32;
    std::unique_ptr<std::array<FxDrain, kMaxFxChains>> fxDrains_;
    void releaseSynth(Voice& v);
    std::unique_ptr<std::array<Voice, kMaxVoices>> voices_;
    std::unique_ptr<std::array<Orbit, kMaxOrbits>> orbits_;
    // K() graphs: states lent to the voices that play one
    static constexpr int kMaxKabel = 64;
    std::unique_ptr<std::array<kabel::KabelState, kMaxKabel>> kabel_;
    std::array<bool, kMaxKabel> kabelBusy_{};
    struct FxStage;
    void releaseKabel(FxStage& v);
    void releaseVocoder(FxStage& v);
    void releaseStages(Voice& v);
    // FX chain stages before the main one: a pool lent to the voices, with their reverbs
    static constexpr int kMaxStagePool = 64, kMaxStageReverbs = 16, kMaxStageDelays = 16;
    std::vector<std::array<std::vector<float>, 2>> stageDelays_; // FeedbackDelay lines of FX stages
    std::array<bool, kMaxStageDelays> stageDelayBusy_{};
    std::unique_ptr<std::array<FxStage, kMaxStagePool>> stages_;
    std::array<bool, kMaxStagePool> stageBusy_{};
    struct StageReverb {
        std::atomic<int> state{0}; // 0 free, 1 requested, 2 ready, 3 released (the builder frees it)
        std::atomic<Convolver*> ready{nullptr};
        double params[6] = {2, 0.1, 15000, 1000, 0, 0};
        uint32_t ir = 0;
    };
    std::unique_ptr<std::array<StageReverb, kMaxStageReverbs>> stageReverbs_;
    unsigned stageSeed_ = 1000;
    std::array<double, size_t(ModTarget::Count)> zeroMods_{};
    // stretch: phase vocoders lent to the stages that use one
    static constexpr int kMaxVocoders = 32;
    std::unique_ptr<std::array<PhaseVocoder, kMaxVocoders>> vocoders_;
    std::array<bool, kMaxVocoders> vocoderBusy_{};
    int acquireStage();
    int requestStageReverb(const SoundParams& p);
    void setupStage(FxStage& v, const SoundParams& p, bool main, double end, double endWithRelease, double cps, double cycle,
                    uint32_t seed);
    void processStage(FxStage& v, float& l, float& r, int& ch, int i, int quantumIndex, bool quantumStart, int64_t index,
                      int64_t absIndex, double t, const double* ms, bool main, double sourceStop);
    // audio buses (.bus(n), s("bus"), bmod): getStereoNode()s shared by every voice
    static constexpr int kMaxBuses = 32;
    struct AudioBus {
        double id = 0;
        bool used = false;
        std::array<std::array<float, kMaxBlock>, 2> buf{};
    };
    std::unique_ptr<std::array<AudioBus, kMaxBuses>> audioBuses_;
    int busSlot(double id); // -1 when all slots are taken
    uint64_t voiceCounter_ = 0;
    std::array<int, 64> cutGroups_{}; // cut group (mod 64) -> voice index + 1
    std::atomic<int> active_{0};
    std::atomic<uint64_t> missingSamples_{0};
    // ran out of: 1 K() state, 2 FX stage, 4 vocoder, 8 stage reverb, 16 stage delay, 32 bus, 64 K() delay line,
    // 128 instrument slot
    std::atomic<uint32_t> shortages_{0};
    std::array<std::chrono::steady_clock::time_point, 8> shortageReported_{};
    void shortage(uint32_t bit) { shortages_.fetch_or(bit, std::memory_order_relaxed); }

    // reverb impulse response builder
    std::thread builder_;
    std::mutex builderMutex_;
    std::condition_variable builderCv_;
    bool builderStop_ = false;
    std::atomic<int> builderWork_{0};
};

// superdough helpers, exposed for the tests.
struct ADSR {
    double attack, decay, sustain, release;
};
// helpers.mjs getADSRValues(params, curve, defaultValues)
ADSR getADSRValues(float a, float d, float s, float r, const ADSR* defaults);
// helpers.mjs getParamADSR(param, attack, decay, sustain, release, min, max, begin, end, curve)
void getParamADSR(AudioParam& param, double attack, double decay, double sustain, double release, double min, double max,
                  double begin, double end, bool exponential);
// helpers.mjs distortionAlgorithms (scurve, soft, hard, cubic, diode, asym, fold, sinefold, chebyshev)
double distortion(int algorithm, double x, double k);

} // namespace concha::sd
