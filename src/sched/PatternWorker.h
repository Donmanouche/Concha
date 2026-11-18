#pragma once

// The pattern thread: queries Strudel ahead of the playhead and feeds the audio thread.
//
// Two PatternEngines alternate: the "active" one answers the queries, the "standby" one evaluates
// new code on its own thread. When an evaluation succeeds the two swap, so a slow evaluation
// (1.7 s for some tunes in QuickJS) never leaves the music without events.
//
// Timeline: the audio thread publishes the host position (TransportState). The worker keeps a
// cursor (lap, ppq) and queries [cursor, playhead + lookahead), cut at the DAW's loop end, where
// it continues at the loop start on the next lap. cycle = ppq / beatsPerCycle.

#include "pattern/PatternEngine.h"
#include "sched/SchedTypes.h"
#include "sched/SoundConvert.h"
#include "sched/SpscRing.h"

#include <functional>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace concha {

class PatternWorker {
public:
    struct Options {
        double lookaheadSeconds = 0.25;
        double maxChunkBeats = 1.0;           // longest single query
        double swapMarginSeconds = 0.1;       // new code starts this far after the playhead (room for a slow query)
        std::chrono::milliseconds period{4};  // worker wake-up
        PatternEngine::Options engine;           // engine.host: natives for samples()
        lib::SampleLibrary* library = nullptr;   // sample ids for the sampler
        std::chrono::milliseconds sampleWarmup{250}; // wait for new samples before new code plays
        // After the user's SynthDef folders gave new SynthDefs (read before each evaluation), on
        // the evaluation thread: sends them to the scsynth World (ScHost::loadNewDefs).
        std::function<void()> onNewSynthDefs;
        // Note-ons of the DAW's MIDI input (audio thread -> this worker), played through midikeys().
        SpscRing<MidiInNote>* midiNotes = nullptr;
    };

    struct Status {
        bool ready = false;           // both engines started
        std::string engineError;      // engine start failure
        bool evaluating = false;
        uint64_t evaluations = 0;     // finished evaluations (ok or not)
        bool lastOk = false;
        std::string lastError;
        double lastEvalMs = 0.0;
        std::string activeCode;       // code of the active engine
        uint64_t queries = 0;
        double worstQueryMs = 0.0;
        uint64_t droppedEvents = 0;   // ring full or sysex too long
        uint32_t activeGeneration = 0; // changes with each new code (activeCodeInfo())
    };

    // For the editor, after each new code: its mini-notation ranges, sliders, the known sounds.
    struct ActiveCodeInfo {
        uint32_t generation = 0;
        std::vector<std::pair<int, int>> miniLocations;
        std::vector<EvalResult::Slider> sliders;
        std::vector<std::string> soundNames;
        std::vector<EvalResult::Widget> widgets;
        std::string shader;
    };
    ActiveCodeInfo activeCodeInfo() const;

    // Visuals: while wanted (the window is open), the playing code's animation frames run on the
    // query thread at about 60 per second, for a window of width x height. takeFrames() returns the
    // frames since the last call (oldest first), with the code generation they belong to.
    void setVisuals(bool wanted, int width, int height, double pixelRatio);
    struct Frame {
        uint32_t generation = 0;
        std::vector<CanvasFrame> canvases;
    };
    std::vector<Frame> takeFrames();

    // A queried hap, for highlighting its code ranges while it plays (cycles, whole).
    struct VisualHap {
        double begin = 0, end = 0;
        uint32_t generation = 0;
        std::string color; // hap.value.color, if any
        std::string uniform; // .uniform("name") (shaders), with its value (gain * velocity)
        double uniformValue = 1;
        std::vector<std::pair<int, int>> locations;
    };

    PatternWorker(SpscRing<SchedEvent>& toAudio, SeqLock<TransportState>& transport, SeqLock<SwapPoint>& swap);
    PatternWorker(SpscRing<SchedEvent>& toAudio, SeqLock<TransportState>& transport, SeqLock<SwapPoint>& swap, Options options);
    ~PatternWorker();

    void start(); // starts the threads; the engines are created on them (about 150 ms each)
    void stop();

    // Any thread. The latest request wins if several arrive during an evaluation.
    void requestEvaluate(std::string code);

    Status status() const;
    std::vector<LogMessage> takeLog();

    // A slider widget moved: its value, for the playing code (applied on the query thread).
    void setSlider(std::string id, double value);

    // The haps playing at `cycle` (begin <= cycle < end); old ones are dropped. Any thread.
    std::vector<VisualHap> activeHaps(double cycle);

    // Test hook: true once the events up to (lap, ppq) have been queued.
    bool queuedUpTo(int32_t lap, double ppq) const;

private:
    void queryLoop();
    void evalLoop();
    void runQueries(const TransportState& t);
    void playMidiNotes(const TransportState& t);
    void flushExternals(const TransportState& t);
    struct External { // an MQTT / serial message waiting for its epoch to play
        uint32_t epoch = 0;
        int32_t lap = 0;
        double ppq = 0;
        PatternEvent event;
    };
    std::vector<External> externals_;
    void pushEvents(const std::vector<PatternEvent>& events, const TransportState& t, int32_t lap, double chunkStartPpq);
    void log(std::string type, std::string message);

    SpscRing<SchedEvent>& toAudio_;
    SeqLock<TransportState>& transport_;
    SeqLock<SwapPoint>& swap_;
    Options options_;

    std::unique_ptr<PatternEngine> engines_[2];
    int registeredDefs_[2] = {0, 0}; // scDefCount() each engine has registered
    int active_ = 0; // guarded by mutex_

    mutable std::mutex mutex_;
    std::condition_variable evalCv_;
    std::optional<std::string> pendingCode_;
    bool swapPending_ = false;
    std::string standbyCode_;
    ActiveCodeInfo activeInfo_; // guarded by mutex_
    std::vector<std::pair<int, int>> standbyMini_;
    std::vector<EvalResult::Slider> standbySliders_;
    std::vector<EvalResult::Widget> standbyWidgets_;
    std::string standbyShader_;
    void visualLoop();
    std::vector<std::pair<std::string, double>> visualSliders_; // guarded by mutex_: slider moves for the visuals' engine
    struct VisualsRequest {
        bool wanted = false;
        int width = 800, height = 600;
        double pixelRatio = 1;
    } visuals_; // guarded by mutex_
    std::mutex framesMutex_;
    std::deque<Frame> frames_;
    std::vector<std::pair<std::string, double>> sliderMoves_; // guarded by mutex_
    std::vector<std::string> standbySounds_;
    std::mutex visualMutex_;
    std::deque<VisualHap> visual_;
    Status status_;
    std::vector<LogMessage> log_;

    std::atomic<bool> running_{false};
    std::thread queryThread_, evalThread_, visualThread_;

    // Query thread only.
    uint32_t epoch_ = UINT32_MAX;
    uint32_t codeGeneration_ = 0;
    int32_t cursorLap_ = 0;
    double cursorPpq_ = 0.0;
    double startedFromPpq_ = 0.0; // where the cursor of this epoch started
    std::atomic<int32_t> queuedLap_{0};
    std::atomic<double> queuedPpq_{-1e300};
    std::vector<PatternEvent> scratch_;
    SoundConverter converter_;
    std::vector<std::string> convertLog_;
};

} // namespace concha
