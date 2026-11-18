#pragma once

// Phase 0 test generator. It proves three things before any Strudel code exists:
//  - sync: one click per host beat, placed on the exact sample (downbeats are higher);
//  - multi-out: each auxiliary bus k (1..15) plays a short tone of its own pitch on every
//    downbeat, so each DAW track can be identified by ear;
//  - MIDI out: a note per beat (C5 on the bar, C4 otherwise, half a beat long), CC 74 stepping
//    with the beat, and a Program Change (bar % 8) at every bar.
// Beat positions come from the host's ppq position, so loops and jumps stay on the grid.

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cstdint>

namespace concha {

struct TransportSnapshot {
    double ppq = 0.0;        // quarter notes at the first sample of the block
    double bpm = 120.0;
    int beatsPerBar = 4;     // time signature numerator, in quarter notes for 4/4
    bool playing = false;
};

class SyncProbe {
public:
    void prepare(double sampleRate, int numBuses);
    void reset();

    // Renders into the buses (adds) and appends MIDI. Buses beyond the vector size are skipped.
    void process(const TransportSnapshot& t, int numSamples,
                 juce::AudioBuffer<float>* const* buses, int numBuses, juce::MidiBuffer& midiOut);

private:
    struct Voice {
        int bus = -1;
        double phase = 0.0, inc = 0.0;
        float amp = 0.0f, decay = 0.0f;
        int remaining = 0;
    };
    struct PendingOff {
        int note = -1;
        int64_t samplesLeft = 0;
    };

    void startVoice(int bus, double freq, float amp, double lengthSeconds);
    void renderVoices(int start, int end, juce::AudioBuffer<float>* const* buses, int numBuses);

    double sampleRate_ = 48000.0;
    int numBuses_ = 1;
    std::array<Voice, 32> voices_{};
    std::array<PendingOff, 8> offs_{};
    bool wasPlaying_ = false;
};

// Sample offsets (in [0, numSamples)) of every beat boundary that falls inside the block.
// Exposed for the unit tests.
template <typename F>
void forEachBeatInBlock(double ppqStart, double bpm, double sampleRate, int numSamples, F&& f)
{
    if (bpm <= 0.0 || sampleRate <= 0.0) return;
    const double samplesPerBeat = sampleRate * 60.0 / bpm;
    const double ppqEnd = ppqStart + numSamples / samplesPerBeat;
    double beat = std::ceil(ppqStart - 1e-9);
    for (; beat < ppqEnd; beat += 1.0) {
        const auto offset = static_cast<int>(std::llround((beat - ppqStart) * samplesPerBeat));
        if (offset >= 0 && offset < numSamples) f(static_cast<int64_t>(beat), offset);
    }
}

} // namespace concha
