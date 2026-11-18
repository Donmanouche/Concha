#pragma once

// Audio-thread side of the scheduler: follows the DAW's playhead, places the pattern thread's
// events on their exact sample, and handles loops, jumps and code changes.
//
//  - Continuity: each block predicts where the next one starts (wrapping at the loop end when the
//    DAW loops). A block that starts elsewhere is a jump: the transport epoch changes, queued
//    events are dropped, hanging MIDI notes are released, and the pattern thread starts again
//    from the playhead. Tempo changes are not jumps: events are placed in beats, so they follow.
//  - Loops: a block that crosses the loop end is split; the second part is the next lap.
//  - Code changes (SwapPoint): events of the old code from the swap point on are dropped, except
//    note-offs.
//  - Same sample: note-offs go out before note-ons.
//
// No allocation, no lock: real-time safe. JUCE-free (tests drive it directly).

#include "sched/SchedTypes.h"
#include "sched/SpscRing.h"

#include <array>
#include <vector>
#include <atomic>
#include <bitset>
#include <cstdint>

namespace concha {

struct HostPosition {
    bool playing = false;
    double ppq = 0.0;
    double bpm = 120.0;
    bool looping = false;
    double loopStart = 0.0, loopEnd = 0.0;
};

class AudioScheduler {
public:
    struct Sink {
        virtual ~Sink() = default;
        virtual void midi(int sampleOffset, const uint8_t* bytes, int length) = 0;
        virtual void sound(int sampleOffset, const SchedEvent& event, double samplesPerBeat) = 0;
    };

    struct Stats {
        std::atomic<uint64_t> emitted{0}, late{0}, overflow{0}, jumps{0}, swaps{0};
    };

    AudioScheduler(SpscRing<SchedEvent>& fromPattern, SeqLock<TransportState>& transport, SeqLock<SwapPoint>& swap);

    void prepare(double sampleRate);
    void process(const HostPosition& host, int numSamples, double beatsPerCycle, Sink& sink);

    const Stats& stats() const { return stats_; }
    uint32_t epoch() const { return epoch_; }
    int32_t lap() const { return lap_; }

    // Events further in the past than this are dropped instead of played at sample 0.
    static constexpr double kLateToleranceBeats = 0.03;

private:
    void jump(Sink& sink);
    void releaseNotes(Sink& sink, int offset);
    void trackMidi(const uint8_t* b, int n);

    SpscRing<SchedEvent>& in_;
    SeqLock<TransportState>& transport_;
    SeqLock<SwapPoint>& swap_;
    double sampleRate_ = 48000.0;

    uint32_t epoch_ = 0;
    int32_t lap_ = 0;
    bool wasPlaying_ = false;
    double expectedPpq_ = 0.0; // where the next block should start...
    int32_t expectedLap_ = 0;
    double altPpq_ = 0.0;      // ...or there (loop end reached exactly: wrapped, or not)
    int32_t altLap_ = 0;
    SwapPoint swapPoint_{};

    static constexpr int kPending = 4096;
    std::vector<SchedEvent> pending_ = std::vector<SchedEvent>(kPending); // (heap: an event is ~2.4 KB)
    int pendingCount_ = 0;

    struct Due {
        int offset;
        int priority; // 0 note-off, 1 other
        int index;
    };
    std::array<Due, kPending> due_;

    std::array<std::bitset<128>, 16> notesOn_{};
    Stats stats_;
};

} // namespace concha
