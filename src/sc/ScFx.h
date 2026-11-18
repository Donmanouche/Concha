#pragma once

// The embedded scsynth World that runs StrudelDirt's effect modules at the end of superdough voices
// (sd::FxServer). A chain: the voice's input pair -> a private bus (concha_fx_in2), the modules in
// StrudelDirt's order (ReplaceOut on that bus), the bus -> the slot's output pair (concha_fx_out2),
// all in one group freed with the chain.

#include "sc/ConchaSc.h"
#include "superdough/SynthServer.h"

#include <array>
#include <functional>
#include <vector>

namespace concha::sc {

class ScFx final : public sd::FxServer {
public:
    static constexpr int kSlots = 32, kRing = 1 << 14;
    explicit ScFx(double sampleRate); // loads the modules' SynthDefs (blocking). Not the audio thread.
    ~ScFx() override;
    bool ok() const { return world_ != nullptr && ready_; }

    int start(const sd::FxModule* modules, int count, int64_t at, double sustain) override;
    // For tools (concha_render --trace-sc): each module started (its onset is the voice's start,
    // kLatency early), with its controls. Never set in the plug-in (audio thread).
    std::function<void(int64_t at, const struct ScDef& def, const sd::SynthArg* args, int numArgs, int sustainArg, double sustain)> trace;
    void write(int slot, int64_t absolute, float left, float right) override
    {
        const size_t i = size_t(absolute & (kRing - 1));
        in_[size_t(slot * 2)][i] = left;
        in_[size_t(slot * 2 + 1)][i] = right;
    }
    void processTo(int64_t end) override;
    float output(int slot, int channel, int64_t absolute) const override
    {
        return out_[size_t(slot * 2 + channel)][size_t(absolute & (kRing - 1))];
    }
    void stop(int slot) override;

private:
    int64_t oscTime(int64_t sample) const;
    void queue(int64_t at);

    ConchaScWorld* world_ = nullptr;
    double sampleRate_;
    bool ready_ = false;
    int64_t processed_ = -1; // absolute sample the World has processed up to (-1: not started)
    std::array<std::vector<float>, kSlots * 2> in_, out_;
    std::array<bool, kSlots> busy_{};
    std::array<int32_t, kSlots> group_{};
    int32_t nextNode_ = 1000;
    std::vector<float> outBlock_;
    std::vector<float*> outs_;
    std::vector<const float*> ins_;
    std::vector<char> msg_;
};

} // namespace concha::sc
