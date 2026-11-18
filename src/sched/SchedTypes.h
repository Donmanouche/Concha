#pragma once

// Data shared by the pattern thread (PatternWorker) and the audio thread (AudioScheduler).
#include "superdough/SoundParams.h"

#include <cstdint>

namespace concha {

// One event for the audio thread. Times are host positions: `ppq` (quarter notes) on loop lap `lap`
// of transport epoch `epoch` (see AudioScheduler).
struct SchedEvent {
    enum class Type : uint8_t { Midi, Sound };
    Type type = Type::Sound;
    uint8_t midiLength = 0;
    uint32_t epoch = 0;
    uint32_t codeGeneration = 0;
    int32_t lap = 0;
    double ppq = 0.0;
    // Sound: hap.duration (clip included) in beats, the hap's cycle, superdough's parameters.
    float durationBeats = 0.f;
    double cycle = 0.0;
    sd::SoundParams sound;
    // MIDI bytes; sysex longer than this is dropped by the pattern thread (and logged).
    static constexpr int kMaxMidi = 64;
    uint8_t midi[kMaxMidi] = {};
};

// Audio thread -> pattern thread, every block.
struct TransportState {
    double ppq = 0.0;           // position at the start of the last block
    double bpm = 120.0;
    double beatsPerCycle = 4.0; // N: one Strudel cycle = N quarter notes
    double loopStart = 0.0, loopEnd = 0.0;
    double sampleRate = 48000.0;
    double blockBeats = 0.0;    // length of the last block, in beats
    int32_t lap = 0;
    uint32_t epoch = 0;         // incremented on every jump / start / stop. While stopped, events
                                // for epoch + 1 prepare the start from `ppq`.
    bool playing = false;
    bool looping = false;
};

// Pattern thread -> audio thread when new code takes over: events of older code generations at or
// after this point are dropped (note-offs excepted, so no note hangs).
struct SwapPoint {
    uint32_t codeGeneration = 0;
    uint32_t epoch = 0;
    int32_t lap = 0;
    double ppq = 0.0;
};

} // namespace concha
