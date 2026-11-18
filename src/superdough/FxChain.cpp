#include "superdough/FxChain.h"

#include <cstring>

namespace concha::sd {

FxChainStore& FxChainStore::instance()
{
    static FxChainStore s;
    return s;
}

uint32_t FxChainStore::publish(const FxChain& chain)
{
    // FNV-1a over the stages in use (bitwise: NaN fields compare equal)
    const auto* bytes = reinterpret_cast<const unsigned char*>(chain.stages.data());
    const size_t size = sizeof(SoundParams) * size_t(chain.count);
    uint64_t h = 1469598103934665603ull ^ uint64_t(chain.count);
    for (size_t i = 0; i < size; ++i) h = (h ^ bytes[i]) * 1099511628211ull;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t k = 1; k < kSlots; ++k) {
        Slot& s = owned_[k];
        if (s.chain && s.hash == h && s.chain->count == chain.count &&
            std::memcmp(s.chain->stages.data(), chain.stages.data(), size) == 0) {
            s.used = now;
            return k;
        }
    }
    // free retired chains nobody can read any more
    for (size_t i = 0; i < retired_.size();)
        if (now - retired_[i].second > std::chrono::seconds(60)) {
            retired_[i] = std::move(retired_.back());
            retired_.pop_back();
        } else {
            ++i;
        }
    for (uint32_t tries = 0; tries < kSlots - 1; ++tries) {
        const uint32_t k = next_;
        next_ = next_ + 1 >= kSlots ? 1 : next_ + 1;
        Slot& s = owned_[k];
        if (s.chain && now - s.used < std::chrono::seconds(60)) continue;
        auto fresh = std::make_unique<FxChain>(chain);
        slots_[k].store(fresh.get(), std::memory_order_release);
        if (s.chain) retired_.emplace_back(std::move(s.chain), now);
        s.chain = std::move(fresh);
        s.used = now;
        s.hash = h;
        return k;
    }
    return 0;
}

} // namespace concha::sd
