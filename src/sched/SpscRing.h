#pragma once

// Single-producer / single-consumer lock-free ring (pattern thread -> audio thread).
#include <atomic>
#include <cstddef>
#include <memory>

namespace concha {

template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacityPowerOfTwo) : mask_(capacityPowerOfTwo - 1), data_(new T[capacityPowerOfTwo]) {}

    // Producer. Returns false when full.
    bool push(const T& v)
    {
        const size_t w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) > mask_) return false;
        data_[w & mask_] = v;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    // Consumer.
    bool pop(T& v)
    {
        const size_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        v = data_[r & mask_];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }

    size_t sizeApprox() const { return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire); }
    size_t capacity() const { return mask_ + 1; }

private:
    const size_t mask_;
    std::unique_ptr<T[]> data_;
    alignas(64) std::atomic<size_t> write_{0};
    alignas(64) std::atomic<size_t> read_{0};
};

// Seqlock: one writer publishes a small trivially-copyable struct, readers get a consistent copy
// without blocking the writer (the audio thread may be either side).
template <typename T>
class SeqLock {
public:
    void store(const T& v)
    {
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        copy(value_, v);
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);
    }
    T load() const
    {
        T out;
        for (;;) {
            const uint32_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1) continue;
            copy(out, value_);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s1) return out;
        }
    }

private:
    static void copy(T& dst, const T& src)
    {
        // byte copy through volatile-free memcpy semantics; T must be trivially copyable
        auto* d = reinterpret_cast<unsigned char*>(&dst);
        const auto* s = reinterpret_cast<const unsigned char*>(&src);
        for (size_t i = 0; i < sizeof(T); ++i) d[i] = s[i];
    }
    std::atomic<uint32_t> seq_{0};
    T value_{};
};

} // namespace concha
