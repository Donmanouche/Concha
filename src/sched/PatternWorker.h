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

#include <atomic>
#include <condition_variable>
#include <memory>
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

    // Test hook: true once the events up to (lap, ppq) have been queued.
    bool queuedUpTo(int32_t lap, double ppq) const;

private:
    void queryLoop();
    void evalLoop();
    void runQueries(const TransportState& t);
    void pushEvents(const std::vector<PatternEvent>& events, const TransportState& t, int32_t lap, double chunkStartPpq);
    void log(std::string type, std::string message);

    SpscRing<SchedEvent>& toAudio_;
    SeqLock<TransportState>& transport_;
    SeqLock<SwapPoint>& swap_;
    Options options_;

    std::unique_ptr<PatternEngine> engines_[2];
    int active_ = 0; // guarded by mutex_

    mutable std::mutex mutex_;
    std::condition_variable evalCv_;
    std::optional<std::string> pendingCode_;
    bool swapPending_ = false;
    std::string standbyCode_;
    Status status_;
    std::vector<LogMessage> log_;

    std::atomic<bool> running_{false};
    std::thread queryThread_, evalThread_;

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
