#pragma once

// The embedded scsynth World that plays StrudelDirt's instruments for one superdough engine
// (sd::SynthServer). Each playing synth writes to its own pair of the World's outputs; the World
// is rendered by blocks of 64 into a ring the voices read.

#include "sc/ConchaSc.h"
#include "superdough/SynthServer.h"

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

namespace concha::sc {

class ScHost final : public sd::SynthServer {
public:
    static constexpr int kSlots = 32, kRing = 1 << 14;
    // Creates the World and loads the SynthDefs (blocking, ~100 ms). Not the audio thread.
    explicit ScHost(double sampleRate);
    ~ScHost() override;
    bool ok() const { return world_ != nullptr && ready_; }
    // Sends the user's SynthDefs added since (sc::loadUserSynthDefFolders). Any thread but the
    // audio thread; the World takes them on its next blocks.
    void loadNewDefs();
    // For tools (concha_render --trace-sc): each synth started, its onset and controls. Never set
    // in the plug-in (it is called on the audio thread).
    std::function<void(int64_t onset, const struct ScDef& def, const sd::SynthArg* args, int numArgs)> trace;

    int start(int def, const sd::SynthArg* args, int numArgs, int64_t onset, int64_t& readFrom) override;
    void stop(int slot) override;
    void set(int slot, int control, float value, int64_t at) override;
    void renderTo(int64_t end) override;
    float sample(int slot, int channel, int64_t absolute) const override
    {
        return ring_[size_t(slot * 2 + channel)][size_t(absolute & (kRing - 1))];
    }

private:
    int64_t oscTime(int64_t sample) const;
    void processBlock();

    ConchaScWorld* world_ = nullptr;
    double sampleRate_;
    bool ready_ = false;
    std::mutex defsMutex_;
    std::atomic<int> loadedDefs_{0}; // scDef() indices sent to the World
    int64_t rendered_ = 0; // absolute sample the World has rendered up to
    std::array<std::vector<float>, kSlots * 2> ring_;
    std::array<bool, kSlots> busy_{};
    std::array<int32_t, kSlots> node_{};
    int32_t nextNode_ = 1000;
    std::vector<float> block_;
    std::vector<float*> outs_;
    std::vector<char> msg_; // message being built (preallocated)
};

} // namespace concha::sc
