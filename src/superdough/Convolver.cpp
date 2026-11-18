#include "superdough/Convolver.h"

#include "superdough/AudioParam.h"
#include "superdough/Nodes.h"

#include <algorithm>
#include <cmath>

namespace concha::sd {

FFT::FFT(int size) : n_(size), bitrev_(size_t(size)), twiddles_(size_t(size / 2))
{
    int bits = 0;
    while ((1 << bits) < size) ++bits;
    for (int i = 0; i < size; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        bitrev_[size_t(i)] = r;
    }
    for (int k = 0; k < size / 2; ++k) {
        const double a = -2.0 * kPi * k / size;
        twiddles_[size_t(k)] = {float(std::cos(a)), float(std::sin(a))};
    }
}

void FFT::run(std::complex<float>* a, bool inverse) const
{
    const int n = n_;
    for (int i = 0; i < n; ++i)
        if (i < bitrev_[size_t(i)]) std::swap(a[i], a[bitrev_[size_t(i)]]);
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len / 2, step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; ++k) {
                std::complex<float> w = twiddles_[size_t(k * step)];
                if (inverse) w = std::conj(w);
                const auto u = a[i + k], v = a[i + k + half] * w;
                a[i + k] = u + v;
                a[i + k + half] = u - v;
            }
        }
    }
}

float convolverNormalization(const ImpulseResponse& ir)
{
    constexpr float kGainCalibration = 0.00125f, kGainCalibrationSampleRate = 44100.f, kMinPower = 0.000125f;
    double power = 0.0;
    for (int c = 0; c < ir.channels; ++c) {
        const float* d = ir.channel(c);
        for (int i = 0; i < ir.length; ++i) power += double(d[i]) * d[i];
    }
    float p = float(std::sqrt(power / (double(ir.channels) * std::max(1, ir.length))));
    if (!std::isfinite(p) || p < kMinPower) p = kMinPower;
    float scale = 1.f / p * kGainCalibration;
    if (ir.sampleRate > 0) scale *= kGainCalibrationSampleRate / float(ir.sampleRate);
    if (ir.channels == 4) scale *= 0.5f;
    return scale;
}

Convolver::Convolver(const ImpulseResponse& ir, bool normalize)
{
    const float scale = normalize ? convolverNormalization(ir) : 1.f;
    irLength_ = ir.length;
    const int head = std::min(128, ir.length);
    for (int c = 0; c < 2; ++c) {
        direct_[c].assign(128, 0.f);
        const float* d = ir.channel(c);
        for (int i = 0; i < head; ++i) direct_[c][size_t(i)] = d[i] * scale;
        directIn_[c].assign(128, 0.f);
    }
    // stages: 128-sample partitions up to 2048, 1024 up to 16384, then 8192
    int offset = 128;
    auto plan = [&](int partition, int until) {
        if (offset >= ir.length) return;
        const int end = until > 0 ? std::min(until, ir.length) : ir.length;
        const int count = (end - offset + partition - 1) / partition;
        if (count <= 0) return;
        addStage(partition, offset, count, ir, scale);
        offset += count * partition;
    };
    plan(128, 2048);
    plan(1024, 16384);
    plan(8192, 0);
    ringSize_ = 1;
    while (ringSize_ < offset + 2 * 8192 + 256) ringSize_ <<= 1;
    for (auto& r : ring_) r.assign(size_t(ringSize_), 0.f);
}

void Convolver::addStage(int partition, int offset, int count, const ImpulseResponse& ir, float scale)
{
    Stage s;
    s.partition = partition;
    s.offset = offset;
    s.count = count;
    s.fft = std::make_unique<FFT>(2 * partition);
    const int n = 2 * partition;
    for (int c = 0; c < 2; ++c) {
        const float* d = ir.channel(c);
        s.spectra[c].resize(size_t(count));
        s.history[c].assign(size_t(count), std::vector<std::complex<float>>(size_t(n), {0.f, 0.f}));
        for (int k = 0; k < count; ++k) {
            auto& sp = s.spectra[c][size_t(k)];
            sp.assign(size_t(n), {0.f, 0.f});
            for (int i = 0; i < partition; ++i) {
                const int t = offset + k * partition + i;
                if (t < ir.length) sp[size_t(i)] = {d[t] * scale, 0.f};
            }
            s.fft->forward(sp.data());
        }
        s.input[c].assign(size_t(n), 0.f);
    }
    s.scratch.assign(size_t(n), {0.f, 0.f});
    stages_.push_back(std::move(s));
}

void Convolver::reset()
{
    for (int c = 0; c < 2; ++c) {
        std::fill(directIn_[c].begin(), directIn_[c].end(), 0.f);
        std::fill(ring_[c].begin(), ring_[c].end(), 0.f);
    }
    for (auto& s : stages_) {
        for (int c = 0; c < 2; ++c) {
            for (auto& h : s.history[c]) std::fill(h.begin(), h.end(), std::complex<float>{0.f, 0.f});
            std::fill(s.input[c].begin(), s.input[c].end(), 0.f);
        }
        s.filled = 0;
        s.head = 0;
    }
    directPos_ = 0;
}

// A stage's input block is complete (the last `partition` samples of `input`): its
// contribution to the output from (block start + offset) on is added to the ring.
void Convolver::runStage(Stage& s)
{
    const int n = 2 * s.partition, P = s.partition;
    s.head = (s.head + s.count - 1) % s.count;
    for (int c = 0; c < 2; ++c) {
        auto& x = s.history[c][size_t(s.head)];
        for (int i = 0; i < n; ++i) x[size_t(i)] = {s.input[c][size_t(i)], 0.f};
        s.fft->forward(x.data());
        auto& acc = s.scratch;
        std::fill(acc.begin(), acc.end(), std::complex<float>{0.f, 0.f});
        for (int k = 0; k < s.count; ++k) {
            const auto& X = s.history[c][size_t((s.head + k) % s.count)];
            const auto& H = s.spectra[c][size_t(k)];
            for (int i = 0; i < n; ++i) acc[size_t(i)] += X[size_t(i)] * H[size_t(i)];
        }
        s.fft->inverse(acc.data());
        // overlap-save: the last P samples are the convolution of the block just completed with this
        // stage's taps; they belong at (block start + offset), i.e. offset - P samples from now
        const float inv = 1.f / float(n);
        const int dest = (ringPos_ + s.offset - P) & (ringSize_ - 1);
        for (int i = 0; i < P; ++i) ring_[c][size_t((dest + i) & (ringSize_ - 1))] += acc[size_t(P + i)].real() * inv;
    }
}

void Convolver::process(const float* inL, const float* inR, float* outL, float* outR, int n)
{
    const float* in[2] = {inL, inR};
    float* out[2] = {outL, outR};
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 2; ++c) {
            // direct taps
            directIn_[c][size_t(directPos_)] = in[c][i];
            float y = 0.f;
            const float* h = direct_[c].data();
            const float* x = directIn_[c].data();
            int p = directPos_;
            for (int k = 0; k < 128; ++k) {
                y += h[k] * x[p];
                p = (p - 1) & 127;
            }
            y += ring_[c][size_t(ringPos_)];
            ring_[c][size_t(ringPos_)] = 0.f;
            out[c][i] = y;
            for (auto& s : stages_) {
                auto& b = s.input[c];
                b[size_t(s.partition + s.filled)] = in[c][i];
            }
        }
        directPos_ = (directPos_ + 1) & 127;
        ringPos_ = (ringPos_ + 1) & (ringSize_ - 1);
        for (auto& s : stages_) {
            if (++s.filled == s.partition) {
                // ringPos_ is now the sample after the block: the block started P samples ago
                runStage(s);
                for (int c = 0; c < 2; ++c) // slide: keep the last block as the first half
                    std::copy(s.input[c].begin() + s.partition, s.input[c].end(), s.input[c].begin());
                s.filled = 0;
            }
        }
    }
}

std::unique_ptr<ImpulseResponse> generateReverb(double sr, double decayTime, double fadeInTime, double lpStart,
                                                double lpEnd, unsigned seed)
{
    auto ir = std::make_unique<ImpulseResponse>();
    ir->channels = 2;
    ir->sampleRate = sr;
    const double totalTime = decayTime * 1.5;
    const int decayFrames = std::max(1, int(std::lround(decayTime * sr)));
    const int frames = std::max(1, int(std::lround(totalTime * sr)));
    const int fadeFrames = int(std::lround(std::max(0.0, fadeInTime) * sr));
    const double decayBase = std::pow(1.0 / 1000.0, 1.0 / decayFrames);
    ir->length = frames;
    ir->data.resize(size_t(frames) * 2);
    uint32_t s = seed | 1u;
    for (int c = 0; c < 2; ++c) {
        float* d = ir->data.data() + size_t(c) * size_t(frames);
        double g = 1.0;
        for (int j = 0; j < frames; ++j) {
            d[j] = float((randomUnit(s) * 2.0 - 1.0) * g);
            g *= decayBase;
        }
        for (int j = 0; j < fadeFrames && j < frames; ++j) d[j] *= float(j) / fadeFrames;
    }
    if (lpStart != 0) {
        // OfflineAudioContext + BiquadFilter (lowpass, Q 0.0001) with a linear frequency ramp
        lpStart = std::min(lpStart, sr / 2);
        lpEnd = std::min(lpEnd, sr / 2);
        for (int c = 0; c < 2; ++c) {
            float* d = ir->data.data() + size_t(c) * size_t(frames);
            Biquad f;
            f.type = FilterType::Lowpass;
            AudioParam freq{float(lpStart)};
            freq.setValueAtTime(float(lpStart), 0.0);
            freq.linearRampToValueAtTime(float(lpEnd), decayTime);
            for (int j = 0; j < frames; ++j) {
                if ((j & 7) == 0) f.setParams(sr, freq.valueAt(j / sr), 0.0001, 0.0);
                else freq.valueAt(j / sr);
                d[j] = f.process(0, d[j]);
            }
        }
    }
    return ir;
}

} // namespace concha::sd
