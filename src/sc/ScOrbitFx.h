#pragma once

// The embedded scsynth World for StrudelDirt's orbit effects (sd::OrbitFxServer): per orbit, its
// dirt_leslie2 / dirt_shimmer2 synth reads the orbit's dry sum (an input pair) and writes its
// effect to the orbit's output pair, as GlobalDirtEffect plays them on DirtOrbit's buses.

#include "sc/ConchaSc.h"
#include "superdough/SynthServer.h"

#include <array>
#include <vector>

namespace concha::sc {

class ScOrbitFx final : public sd::OrbitFxServer {
public:
    static constexpr int kOrbits = 16, kRing = 1 << 14;
    explicit ScOrbitFx(double sampleRate); // blocking (SynthDefs). Not the audio thread.
    ~ScOrbitFx() override;
    bool ok() const { return world_ != nullptr && ready_; }

    void set(int orbit, Effect effect, const char* const* names, const float* values, int count, int64_t at) override;
    void write(int orbit, int64_t absolute, float left, float right) override
    {
        const size_t i = size_t(absolute & (kRing - 1));
        in_[size_t(orbit * 2)][i] = left;
        in_[size_t(orbit * 2 + 1)][i] = right;
    }
    void processTo(int64_t end) override;
    float output(int orbit, int channel, int64_t absolute) const override
    {
        return out_[size_t(orbit * 2 + channel)][size_t(absolute & (kRing - 1))];
    }
    void reset() override;

private:
    int64_t oscTime(int64_t sample) const;

    ConchaScWorld* world_ = nullptr;
    double sampleRate_;
    bool ready_ = false;
    int64_t processed_ = 0;
    std::array<std::vector<float>, kOrbits * 2> in_, out_;
    std::array<std::array<int32_t, 2>, kOrbits> node_{}; // 0: not playing
    int32_t nextNode_ = 1000;
    std::vector<float> outBlock_;
    std::vector<float*> outs_;
    std::vector<const float*> ins_;
    std::vector<char> msg_;
};

} // namespace concha::sc
