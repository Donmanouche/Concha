#pragma once

// K(): kabelsalat graphs, as superdough's generic-processor worklet runs them.
//
// The pattern thread evaluates the kabelsalat expression and compiles it with kabelsalat's own
// compiler (js/src/kabel.mjs). That gives the per-sample update code (a small subset of
// JavaScript: register assignments, arithmetic, Math.*, ternaries and `nodes[i].update(...)`
// calls), the list of ugens and the register count. Here that code is compiled to bytecode with
// JavaScript's semantics (booleans from comparisons, strict equality, `&&` / `||` returning an
// operand, `%` as fmod), and kabelsalat's ugens (lib/src/ugens.js, synth.js) are ported.
//
// Programs are built on the pattern thread (KabelStore, never freed) and run on the audio thread
// with per-voice state preallocated (KabelState): real-time safe. Delay lines (10 s each, as
// kabelsalat's) come from a process-wide pool reserved by the pattern thread.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace concha::sd::kabel {

// A JavaScript value, as far as the generated code can produce them.
struct Value {
    enum Kind : uint8_t { Number, Bool, Pair, String, Undefined };
    double a = 0, b = 0; // number (or bool 0/1, or the pair's two numbers)
    Kind kind = Number;
};

enum class Ugen : uint8_t {
    ADSR, Clock, ClockDiv, ClockOut, Delay, Distort, Hold, Output, LcgNoise, Noise, Dust, Brown, Pink, Impulse,
    Pulse, Zaw, Saw, Sine, Sidechain, Tri, Scope, Lag, Slew, Slide, Filter, BPF, Fold, AudioIn, MidiGate, MidiFreq,
    MidiVel, Signal, CC, MidiCC, Sequence, Pick, Remap, Clip, Trig, Biquad, Unknown
};

struct UgenDesc {
    Ugen type = Ugen::Unknown;
    double ccInit = 0; // CC: inputs[1] ?? 0; MidiCC: inputs[2] ?? -1
    bool gate = false;  // a cc('strudel-gate-…'): 1 until the note's end
};

struct Instr {
    uint8_t op = 0;
    uint8_t argc = 0;
    int32_t arg = 0;
};

struct Program {
    std::vector<Instr> code;
    std::vector<Value> consts;
    std::vector<UgenDesc> ugens;
    int registers = 0;
    int delays = 0;     // number of Delay ugens
    int maxStack = 0;
};

class KabelStore {
public:
    static KabelStore& instance();
    static constexpr uint32_t kMax = 4096;
    const Program* get(uint32_t id) const { return id > 0 && id < kMax ? slots_[id].load(std::memory_order_acquire) : nullptr; }
    // Compiles the JSON from js/src/kabel.mjs (cached by text). 0 and `error` set on failure.
    // Also reserves delay lines for it at `sampleRate`.
    uint32_t getOrBuild(const std::string& json, double sampleRate, std::string& error);

private:
    std::array<std::atomic<const Program*>, kMax> slots_{};
    std::mutex mutex_;
    std::vector<std::pair<std::string, uint32_t>> keys_;
    std::vector<std::unique_ptr<Program>> owned_;
};

// kabelsalat's synth.Delay buffers: 10 s, shared by every engine of the process.
class DelayPool {
public:
    static DelayPool& instance();
    static constexpr int kMax = 32;
    void reserve(int count, double sampleRate); // pattern thread (allocates)
    // Audio thread: a zeroed buffer of at least `frames`, or -1 when none is free.
    int acquire(int64_t frames);
    void release(int index, int64_t written); // clears what was written
    bool takeMissed() { return missed_.exchange(false, std::memory_order_relaxed); }
    void missed() { missed_.store(true, std::memory_order_relaxed); }
    float* data(int index) const { return buffers_[size_t(index)].data.load(std::memory_order_acquire); }
    int64_t length(int index) const { return buffers_[size_t(index)].length.load(std::memory_order_acquire); }

private:
    struct Slot {
        std::atomic<float*> data{nullptr};
        std::atomic<int64_t> length{0};
        std::atomic<bool> busy{false};
    };
    std::array<Slot, kMax> buffers_{};
    std::atomic<bool> missed_{false};
    std::mutex mutex_;
    std::vector<std::unique_ptr<float[]>> owned_;
    int count_ = 0;
};

// State of one ugen (the fields each port needs).
struct UgenState {
    double phase = 0, value = 0, s = 0, last = 0;
    double b[7] = {0, 0, 0, 0, 0, 0, 0}; // pink noise / biquad x1 x2 y1 y2
    int state = 0, step = 0, count = 0;
    bool sgn = false, sgn2 = false, flag = false;
    double startTime = 0, startVal = 0;
    // Delay
    int delay = -1;
    int64_t writeIdx = 0, readIdx = 0, written = 0, length = 0;
    uint32_t lcg = 0, lcgStart = 0;
};

class KabelState {
public:
    static constexpr int kMaxRegisters = 1024, kMaxUgens = 256, kMaxStack = 64;
    // Voice start (audio thread). `seed` drives Math.random.
    void start(const Program* program, double sampleRate, uint32_t seed);
    void stop(); // returns delay lines to the pool
    // One sample: input = the chain's first channel; writes the two outputs.
    void sample(double input, float& left, float& right);
    void setGate(double v) { gateValue_ = v; }
    bool active() const { return program_ != nullptr; }

private:
    double random();
    double update(int index, const Value* args, int argc);

    const Program* program_ = nullptr;
    double sampleRate_ = 48000, time_ = 0, gateValue_ = 1;
    uint32_t rng_ = 1;
    bool failed_ = false; // the worklet threw (kabelsalat code calling this.send, NaN in a filter)
    std::array<Value, kMaxRegisters> r_{};
    std::array<Value, 16> o_{}, s_{};
    std::array<UgenState, kMaxUgens> u_{};
    std::array<Value, kMaxStack> stack_{};
};

} // namespace concha::sd::kabel
