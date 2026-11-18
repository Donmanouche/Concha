#pragma once

// What superdough needs from a synthesis server to use its instruments as voice sources
// (src/sc/ScHost: the embedded scsynth with StrudelDirt's SynthDefs). Audio thread only.

#include <cstdint>

namespace concha::sd {

struct SynthArg {
    uint8_t control = 0; // index among the SynthDef's controls
    float value = 0;
};

class SynthServer {
public:
    virtual ~SynthServer() = default;
    // Starts SynthDef `def` at absolute sample `onset` (the engine's clock); returns a slot (-1: none
    // free). `readFrom` gets the absolute sample its output starts at (its block's start: the
    // voice reads the slot from there, as StrudelDirt's dirt_gate OffsetOuts it).
    virtual int start(int def, const SynthArg* args, int numArgs, int64_t onset, int64_t& readFrom) = 0;
    virtual void stop(int slot) = 0; // frees the synth, the slot can be reused
    // Renders up to absolute sample `end` (excluded); then sample() is valid up to there.
    virtual void renderTo(int64_t end) = 0;
    virtual float sample(int slot, int channel, int64_t absolute) const = 0;
};

// StrudelDirt's effect modules (squiz, waveloss, fshift, triode, krush, octer, ring, distort2,
// pshift, grenvelo, tremolorate, the spectral ones) on a voice's output: the voice writes its audio,
// the server runs the modules by blocks of 64 and gives it back kLatency samples later (the voice
// starts that much earlier: the scheduler moves it).
struct FxModule {
    int16_t def = -1;     // the module's SynthDef (sc::scDefs() index)
    uint8_t numArgs = 0;
    int8_t sustainArg = -1; // an argument set to the voice's sustain when it starts
    SynthArg args[6];
};

class FxServer {
public:
    static constexpr int kLatency = 64;
    virtual ~FxServer() = default;
    // A chain of modules from absolute sample `at`; -1 when none is free.
    virtual int start(const FxModule* modules, int count, int64_t at, double sustain) = 0;
    virtual void write(int slot, int64_t absolute, float left, float right) = 0;
    // Runs the blocks of 64 whose input is complete, below `end`.
    virtual void processTo(int64_t end) = 0;
    // The chain's output for the input written at `absolute` (valid once processed).
    virtual float output(int slot, int channel, int64_t absolute) const = 0;
    virtual void stop(int slot) = 0;
};

// StrudelDirt's orbit effects (GlobalDirtEffect: dirt_leslie, dirt_shimmer) on an orbit's dry sum:
// the engine writes the sum, the server gives the effects' output back kLatency samples later.
class OrbitFxServer {
public:
    static constexpr int kLatency = 64;
    enum Effect { Leslie = 0, Shimmer = 1 };
    virtual ~OrbitFxServer() = default;
    // Starts the effect on the orbit if needed and sets these controls (by name), at absolute sample `at`.
    virtual void set(int orbit, Effect effect, const char* const* names, const float* values, int count, int64_t at) = 0;
    virtual void write(int orbit, int64_t absolute, float left, float right) = 0;
    virtual void processTo(int64_t end) = 0;
    virtual float output(int orbit, int channel, int64_t absolute) const = 0;
    virtual void reset() = 0; // stops every orbit's effects
};

} // namespace concha::sd
