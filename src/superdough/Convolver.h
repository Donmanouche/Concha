#pragma once

// ConvolverNode (WebAudio) for superdough's reverb: stereo impulse response, no added latency.
//
// Non-uniform partitioned convolution: taps [0, 128) computed directly, then frequency-domain
// stages (partitions of 128, 1024 and 8192 samples) whose results land in the future of an output
// ring, so the result is exactly the convolution, sample-aligned (as Chromium's ReverbConvolver).
// Impulse responses are normalised like Chromium's ConvolverNode (normalize = true).
//
// Built (allocations) off the audio thread; process() is real-time safe.

#include <complex>
#include <memory>
#include <vector>

namespace concha::sd {

class FFT {
public:
    explicit FFT(int size);
    int size() const { return n_; }
    void forward(std::complex<float>* data) const { run(data, false); }
    void inverse(std::complex<float>* data) const { run(data, true); } // unscaled

private:
    void run(std::complex<float>* data, bool inverse) const;
    int n_;
    std::vector<int> bitrev_;
    std::vector<std::complex<float>> twiddles_;
};

struct ImpulseResponse {
    int channels = 2;
    int length = 0;
    double sampleRate = 48000.0;
    std::vector<float> data; // planar
    const float* channel(int c) const { return data.data() + size_t(c < channels ? c : channels - 1) * size_t(length); }
};

// Chromium ConvolverNode normalisation (RMS power, -58 dB calibration, 44.1 kHz reference).
float convolverNormalization(const ImpulseResponse& ir);

class Convolver {
public:
    explicit Convolver(const ImpulseResponse& ir, bool normalize = true);
    // in/out: 2 channels, n samples; `out` is overwritten. Mono input: pass the same pointer twice.
    void process(const float* inL, const float* inR, float* outL, float* outR, int n);
    void reset();

private:
    struct Stage {
        int partition = 0, offset = 0, count = 0; // taps [offset, offset + count * partition)
        std::unique_ptr<FFT> fft;
        std::vector<std::vector<std::complex<float>>> spectra[2]; // IR partitions per channel
        std::vector<std::vector<std::complex<float>>> history[2]; // input spectra (FDL)
        int head = 0;
        std::vector<float> input[2]; // last 2 * partition input samples (overlap-save)
        int filled = 0;
        std::vector<std::complex<float>> scratch;
    };
    void addStage(int partition, int offset, int count, const ImpulseResponse& ir, float scale);
    void runStage(Stage& s);

    int irLength_ = 0;
    std::vector<float> direct_[2];   // taps [0, 128)
    std::vector<float> directIn_[2]; // input history for the direct part
    int directPos_ = 0;
    std::vector<Stage> stages_;
    std::vector<float> ring_[2]; // future output
    int ringSize_ = 0, ringPos_ = 0;
};

// reverbGen.mjs generateReverb: noise * exponential decay (to -60 dB at decayTime), length
// 1.5 * decayTime, fade-in, then a lowpass whose frequency goes from lpStart to lpEnd over
// decayTime (applyGradualLowpass; skipped when lpStart is 0).
std::unique_ptr<ImpulseResponse> generateReverb(double sampleRate, double decayTime, double fadeInTime, double lpFreqStart,
                                                double lpFreqEnd, unsigned seed);

} // namespace concha::sd
