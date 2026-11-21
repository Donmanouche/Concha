#pragma once

// stretch: superdough's phase-vocoder-processor (worklets.mjs, on ola-processor.js): 2048-sample
// frames every 128 samples, Hann windows (x 1.62), peaks of the spectrum moved by the pitch factor
// with their regions of influence, overlap-add.
//
// The worklet's output for a render quantum depends on that whole quantum's input; the engine
// renders sample by sample, so here a quantum's output comes out during the next one (128 samples
// later). The scheduler starts stretched sounds that much earlier, on top of superdough's own 40 ms.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace concha::sd {

// How much earlier a sound with stretch starts: superdough's 40 ms and the quantum of delay here.
inline double kStretchAdvanceSeconds(double sampleRate) { return 0.04 + 128.0 / sampleRate; }

class PhaseVocoder {
public:
    static constexpr int kBlock = 2048, kHop = 128;
    PhaseVocoder();
    void reset();
    // A sample of channel c at `pos` in the current quantum (0..127), and the output at that
    // position (the previous quantum's result).
    void push(int c, int pos, float x) { inQ_[size_t(c)][size_t(pos)] = x; }
    float output(int c, int pos) const { return outQ_[size_t(c)][size_t(pos)]; }
    // End of a quantum: runs the OLA step on what was pushed. timeCursor = the worklet's (128 per
    // quantum since the context's start).
    void endQuantum(int channels, double pitchFactor, double timeCursor);

private:
    void fft(std::vector<double>& re, std::vector<double>& im, bool inverse) const;
    void realTransform(const std::vector<float>& data); // fft.js realTransform -> freq_ (interleaved)
    std::vector<double> freq_, table_;
    std::vector<uint32_t> jsBitrev_;
    int width_ = 0;
    std::array<std::array<float, kHop>, 2> inQ_{}, outQ_{};
    std::array<std::vector<float>, 2> input_, output_, toRetrieve_;
    std::vector<float> hann_, magnitudes_, send_;
    std::vector<int> peaks_;
    std::vector<double> re_, im_, sre_, sim_;
    std::vector<int> bitrev_;
    std::vector<double> cosT_, sinT_;
};

} // namespace concha::sd
