#pragma once

// FX chains (superdough's `FX` array, Pattern.FX()): the stages a voice goes through before its own
// effects. The pattern thread converts each stage to SoundParams and publishes the chain here; the
// audio thread reads it when the voice starts. Identical chains share a slot; a slot is reused
// 60 s after its last publication at the earliest (events are played well before that).

#include "superdough/SoundParams.h"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace concha::sd {

struct FxChain {
    static constexpr int kMaxStages = 4;
    int count = 0;
    std::array<SoundParams, kMaxStages> stages;
};

class FxChainStore {
public:
    static FxChainStore& instance();
    static constexpr uint32_t kSlots = 1024;
    const FxChain* get(uint32_t id) const { return id > 0 && id < kSlots ? slots_[id].load(std::memory_order_acquire) : nullptr; }
    // Pattern thread. 0 when every slot is recent.
    uint32_t publish(const FxChain& chain);

private:
    struct Slot {
        std::unique_ptr<FxChain> chain;
        std::chrono::steady_clock::time_point used{};
        uint64_t hash = 0;
    };
    std::array<std::atomic<const FxChain*>, kSlots> slots_{};
    std::array<Slot, kSlots> owned_{};
    std::vector<std::pair<std::unique_ptr<FxChain>, std::chrono::steady_clock::time_point>> retired_;
    std::mutex mutex_;
    uint32_t next_ = 1;
};

} // namespace concha::sd
