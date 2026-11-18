#pragma once

// superdough's analysers (`.analyze(id)`, `.fft(n)`): the voices' sound after postgain, summed per id
// to mono (an AnalyserNode down-mixes), kept in a ring the visuals read (scope, spectrum).
// AnalyserNode's getFloatFrequencyData: Blackman window, FFT, |X| / N, smoothing over time, dB.

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace concha::sd {

class AnalyserTaps {
public:
    static constexpr int kMaxIds = 16, kRing = 1 << 15; // fftSize up to 32768 (fft 10); slot 0: the main output

    AnalyserTaps();
    // audio thread
    float* block(int id, int numSamples, int fftSize); // to add the block's samples into (zeroed)
    void endBlock(int numSamples);
    // any thread: the last n samples of id (n <= kRing); its fftSize, 0 before any sound
    int read(int id, float* dst, int n) const;

private:
    struct Tap {
        std::vector<float> ring, block;
        std::atomic<int64_t> written{0};
        std::atomic<int> fftSize{0};
        bool used = false;
    };
    std::array<Tap, kMaxIds> taps_;
};

// superdough's analysers are keyed by any value (`.analyze(1)`, widgets use their id string): the
// slot (1..kMaxIds-1) of an id, the same for every engine and thread; old ids are reused in turn.
int analyserSlot(const std::string& id);

// getFloatFrequencyData of `time` (fftSize samples, oldest first) into fftSize / 2 dB values;
// `smooth` keeps the previous magnitudes (smoothingTimeConstant).
void analyserFrequencyData(const float* time, int fftSize, double smoothing, std::vector<float>& smooth, float* outDb);

} // namespace concha::sd
