#pragma once

// Decoded samples, shared by every Concha instance of the process. Loading threads publish a
// buffer under an id; the audio thread reads it with one atomic load. Buffers stay alive until the
// store is destroyed (like superdough's bufferCache), so the audio thread never sees one freed.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace concha::sd {

struct SampleData {
    int channels = 1;
    int64_t frames = 0;
    double sampleRate = 48000.0;
    std::vector<float> planar; // channel 0 frames, then channel 1...
    const float* channel(int c) const { return planar.data() + int64_t(c < channels ? c : channels - 1) * frames; }
    double duration() const { return frames / sampleRate; }
};

class SampleStore {
public:
    static constexpr uint32_t kMaxSamples = 65536;

    // Audio thread: null while not loaded (or failed).
    const SampleData* get(uint32_t id) const
    {
        return id > 0 && id < kMaxSamples ? slots_[id].load(std::memory_order_acquire) : nullptr;
    }

    // Loading side.
    void publish(uint32_t id, std::unique_ptr<SampleData> data)
    {
        if (id == 0 || id >= kMaxSamples) return;
        std::lock_guard<std::mutex> lock(mutex_);
        owned_.push_back(std::move(data));
        slots_[id].store(owned_.back().get(), std::memory_order_release);
    }

    // Points a slot at a buffer owned elsewhere (transient buffers).
    void publishPointer(uint32_t id, const SampleData* data)
    {
        if (id > 0 && id < kMaxSamples) slots_[id].store(data, std::memory_order_release);
    }

private:
    std::array<std::atomic<const SampleData*>, kMaxSamples> slots_{};
    std::mutex mutex_;
    std::vector<std::unique_ptr<SampleData>> owned_;
};

} // namespace concha::sd
