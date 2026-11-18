#include "superdough/Analyser.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <mutex>

namespace concha::sd {

AnalyserTaps::AnalyserTaps()
{
    for (auto& t : taps_) {
        t.ring.assign(kRing, 0.f);
        t.block.assign(8192, 0.f);
    }
}

float* AnalyserTaps::block(int id, int numSamples, int fftSize)
{
    if (id < 0 || id >= kMaxIds || numSamples > int(taps_[size_t(id)].block.size())) return nullptr;
    Tap& t = taps_[size_t(id)];
    if (!t.used) {
        std::fill_n(t.block.begin(), numSamples, 0.f);
        t.used = true;
    }
    t.fftSize.store(std::clamp(fftSize, 32, kRing), std::memory_order_relaxed);
    return t.block.data();
}

void AnalyserTaps::endBlock(int numSamples)
{
    for (auto& t : taps_) {
        if (t.fftSize.load(std::memory_order_relaxed) == 0) continue; // never analysed
        const int64_t w = t.written.load(std::memory_order_relaxed);
        for (int i = 0; i < numSamples; ++i) t.ring[size_t((w + i) & (kRing - 1))] = t.used ? t.block[size_t(i)] : 0.f;
        t.written.store(w + numSamples, std::memory_order_release);
        t.used = false;
    }
}

int AnalyserTaps::read(int id, float* dst, int n) const
{
    if (id < 0 || id >= kMaxIds) return 0;
    const Tap& t = taps_[size_t(id)];
    const int size = t.fftSize.load(std::memory_order_relaxed);
    if (!size || !dst) return size;
    n = std::min(n, kRing);
    const int64_t w = t.written.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) dst[i] = t.ring[size_t((w - n + i) & (kRing - 1))];
    return size;
}

int analyserSlot(const std::string& id)
{
    static std::mutex mutex;
    static std::vector<std::string> ids; // slot - 1 -> id
    static size_t next = 0;
    std::lock_guard<std::mutex> lock(mutex);
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == id) return int(i) + 1;
    const size_t slots = AnalyserTaps::kMaxIds - 1;
    if (ids.size() < slots) {
        ids.push_back(id);
        return int(ids.size());
    }
    const size_t k = next++ % slots;
    ids[k] = id;
    return int(k) + 1;
}

void analyserFrequencyData(const float* time, int n, double smoothing, std::vector<float>& smooth, float* outDb)
{
    // n is a power of two (fftSize)
    std::vector<std::complex<double>> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) { // Blackman window (alpha 0.16), as the spec
        const double a = 0.16, a0 = (1 - a) / 2, a1 = 0.5, a2 = a / 2;
        const double w = a0 - a1 * std::cos(2 * M_PI * i / n) + a2 * std::cos(4 * M_PI * i / n);
        x[size_t(i)] = time[i] * w;
    }
    for (int i = 1, j = 0; i < n; ++i) { // iterative radix-2 FFT
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[size_t(i)], x[size_t(j)]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const std::complex<double> wl = std::polar(1.0, -2 * M_PI / len);
        for (int i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (int k = 0; k < len / 2; ++k) {
                const auto u = x[size_t(i + k)], v = x[size_t(i + k + len / 2)] * w;
                x[size_t(i + k)] = u + v;
                x[size_t(i + k + len / 2)] = u - v;
                w *= wl;
            }
        }
    }
    const int bins = n / 2;
    if (int(smooth.size()) != bins) smooth.assign(size_t(bins), 0.f);
    for (int k = 0; k < bins; ++k) {
        const double mag = std::abs(x[size_t(k)]) / n;
        const double s = smoothing * smooth[size_t(k)] + (1 - smoothing) * mag;
        smooth[size_t(k)] = float(s);
        outDb[k] = s > 0 ? float(20 * std::log10(s)) : -1000.f;
    }
}

} // namespace concha::sd
