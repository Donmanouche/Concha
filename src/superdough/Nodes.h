#pragma once

// The WebAudio nodes superdough builds its voices with, following the WebAudio spec and, where
// the spec leaves room, Chromium's implementation (strudel.cc's reference browser):
//  - OscillatorNode: band-limited wavetables (PeriodicWave), 3 tables per octave, interpolated
//    between neighbouring tables, normalised on the full-band waveform (as Chromium).
//  - BiquadFilterNode: the spec's formulas (lowpass / highpass take Q in dB).
//  - StereoPannerNode: the spec's equal-power law, mono and stereo inputs.
//  - Noise buffers: superdough's noise.mjs algorithms (white, pink, brown, crackle).

#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <cstdint>
#include <memory>
#include <vector>

namespace concha::sd {

constexpr double kPi = 3.14159265358979323846;

enum class Wave : uint8_t { Sine, Square, Sawtooth, Triangle };

// Shared band-limited tables for the four basic waves at one sample rate.
class PeriodicWaves {
public:
    explicit PeriodicWaves(double sampleRate);

    int size() const { return size_; }
    int ranges() const { return ranges_; }
    double sampleRate() const { return sampleRate_; }
    // Chromium's PeriodicWave::WaveDataForFundamentalFrequency.
    void tablesFor(Wave w, double fundamental, const float*& lower, const float*& higher, float& interp) const;

private:
    double sampleRate_;
    int size_ = 4096;
    int ranges_ = 36;
    double lowestFundamental_ = 0.0;
    std::array<std::vector<std::vector<float>>, 4> tables_; // [wave][range][size_ + 1] (guard point)
};

// A PeriodicWave from Fourier coefficients (createPeriodicWave(real, imag), normalised), with the
// same band-limited tables as the basic waves.
class CustomWave {
public:
    CustomWave(double sampleRate, const std::vector<double>& real, const std::vector<double>& imag);
    void tablesFor(double fundamental, const float*& lower, const float*& higher, float& interp) const;
    int size() const { return size_; }

private:
    int size_ = 4096, ranges_ = 36;
    double lowestFundamental_ = 0.0;
    std::vector<std::vector<float>> tables_;
};

// Custom waves shared by the process: built off the audio thread, read with one atomic load.
class WaveStore {
public:
    static WaveStore& instance();
    static constexpr uint32_t kMax = 4096;
    const CustomWave* get(uint32_t id) const { return id > 0 && id < kMax ? slots_[id].load(std::memory_order_acquire) : nullptr; }
    // Returns the id of the wave for this key, building it if new (blocking, a few ms).
    uint32_t getOrBuild(const std::string& key, double sampleRate, const std::vector<double>& real, const std::vector<double>& imag);

private:
    std::array<std::atomic<const CustomWave*>, kMax> slots_{};
    std::mutex mutex_;
    std::vector<std::pair<std::string, uint32_t>> keys_;
    std::vector<std::unique_ptr<CustomWave>> owned_;
};

// Oscillator with frequency (Hz) and detune (cents) computed by the caller per sample.
struct Oscillator {
    Wave wave = Wave::Sine;
    const CustomWave* custom = nullptr; // set: plays this PeriodicWave instead of `wave`
    double phase = 0.0; // 0..1
    // table choice for the last frequency (recomputed only when it changes)
    double lastFrequency = -1.0;
    const float* lo = nullptr;
    const float* hi = nullptr;
    float interp = 0.f;
    float next(const PeriodicWaves& waves, double frequency)
    {
        const double sr = waves.sampleRate();
        const double nyq = sr * 0.5;
        if (!std::isfinite(frequency)) frequency = 0.0; // a NaN would index the tables anywhere
        if (!std::isfinite(phase)) phase = 0.0;
        if (frequency > nyq) frequency = nyq;
        if (frequency < -nyq) frequency = -nyq;
        float out;
        if (wave == Wave::Sine && !custom) {
            out = float(std::sin(2.0 * kPi * phase));
        } else {
            if (frequency != lastFrequency || !lo) {
                if (custom) custom->tablesFor(std::fabs(frequency), lo, hi, interp);
                else waves.tablesFor(wave, std::fabs(frequency), lo, hi, interp);
                lastFrequency = frequency;
            }
            const float f = interp;
            const double pos = phase * (custom ? custom->size() : waves.size());
            const int i = int(pos);
            const float frac = float(pos - i);
            const float a = lo[i] + (lo[i + 1] - lo[i]) * frac;
            const float b = hi[i] + (hi[i + 1] - hi[i]) * frac;
            out = (1.f - f) * a + f * b;
        }
        phase += frequency / sr;
        phase -= std::floor(phase);
        return out;
    }
};

enum class FilterType : uint8_t { Lowpass, Highpass, Bandpass, Notch, Allpass, Peaking, Lowshelf, Highshelf };

// BiquadFilterNode, up to 2 channels. Coefficients are recomputed when the parameters change.
class Biquad {
public:
    FilterType type = FilterType::Lowpass;

    void reset()
    {
        for (auto& s : state_) s = {};
        lastF_ = lastQ_ = lastG_ = -1.0;
    }
    // frequency in Hz (detune already applied), q as given to the node, gain in dB.
    void setParams(double sampleRate, double frequency, double q, double gainDb);
    float process(int ch, float x)
    {
        auto& s = state_[size_t(ch)];
        const double y = b0_ * x + b1_ * s.x1 + b2_ * s.x2 - a1_ * s.y1 - a2_ * s.y2;
        s.x2 = s.x1;
        s.x1 = x;
        s.y2 = s.y1;
        s.y1 = std::fabs(y) < 1e-30 ? 0.0 : y; // flush denormals
        return float(y);
    }

private:
    struct State {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    };
    std::array<State, 2> state_{};
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    double lastF_ = -1, lastQ_ = -1, lastG_ = -1;
    FilterType lastType_ = FilterType::Lowpass;
};

// StereoPannerNode: pan in [-1, 1].
inline void stereoPan(float pan, int inputChannels, float inL, float inR, float& outL, float& outR)
{
    pan = pan < -1.f ? -1.f : (pan > 1.f ? 1.f : pan);
    if (inputChannels == 1) {
        const double x = (pan + 1.0) / 2.0;
        outL = float(inL * std::cos(x * kPi / 2));
        outR = float(inL * std::sin(x * kPi / 2));
        return;
    }
    const double x = pan <= 0 ? pan + 1.0 : pan;
    const float gL = float(std::cos(x * kPi / 2)), gR = float(std::sin(x * kPi / 2));
    if (pan <= 0) {
        outL = inL + inR * gL;
        outR = inR * gR;
    } else {
        outL = inL * gL;
        outR = inR + inL * gR;
    }
}

enum class NoiseType : uint8_t { White, Pink, Brown, Crackle };

// 2-second looped noise buffers (noise.mjs getNoiseBuffer); crackle is regenerated per voice.
class NoiseBuffers {
public:
    explicit NoiseBuffers(double sampleRate);
    const std::vector<float>& get(NoiseType t) const { return buffers_[size_t(t)]; }
    static void generate(NoiseType t, double density, std::vector<float>& out, uint32_t& seed);

private:
    std::array<std::vector<float>, 4> buffers_;
};

// Small fast PRNG for noise (Math.random stands in for superdough).
inline float randomUnit(uint32_t& s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return float(s) / 4294967296.f;
}

} // namespace concha::sd
