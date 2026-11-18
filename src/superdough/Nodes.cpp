#include "superdough/Nodes.h"

#include <algorithm>
#include <complex>

namespace concha::sd {

namespace {

// In-place radix-2 FFT (inverse when inverse == true, unscaled).
void fft(std::vector<std::complex<double>>& a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * kPi / double(len) * (inverse ? 1 : -1);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Fourier sine coefficients b_n of the spec's basic waves (a_n = 0).
double coefficient(Wave w, int n)
{
    switch (w) {
        case Wave::Sine: return n == 1 ? 1.0 : 0.0;
        case Wave::Square: return (n & 1) ? 4.0 / (n * kPi) : 0.0;
        case Wave::Sawtooth: return ((n + 1) & 1 ? -1.0 : 1.0) * 2.0 / (n * kPi); // (-1)^(n+1) 2/(n pi)
        case Wave::Triangle: return 8.0 * std::sin(n * kPi / 2.0) / ((kPi * n) * (kPi * n));
    }
    return 0.0;
}

} // namespace

PeriodicWaves::PeriodicWaves(double sampleRate) : sampleRate_(sampleRate)
{
    // Chromium: 2048 points up to 24 kHz, 4096 up to 88.2 kHz, 16384 above.
    size_ = sampleRate <= 24000 ? 2048 : sampleRate <= 88200 ? 4096 : 16384;
    ranges_ = int(std::lround(3.0 * std::log2(double(size_))));
    lowestFundamental_ = sampleRate / size_;
    const int maxPartials = size_ / 2;

    for (Wave w : {Wave::Square, Wave::Sawtooth, Wave::Triangle}) {
        auto& wt = tables_[size_t(w)];
        wt.resize(size_t(ranges_));
        double normalisation = 1.0;
        for (int r = 0; r < ranges_; ++r) {
            // Chromium NumberOfPartialsForRange: cull 400 cents per range.
            const double culling = std::pow(2.0, -(r * 400.0) / 1200.0);
            const int partials = std::max(1, int(culling * maxPartials));
            std::vector<std::complex<double>> spec(size_t(size_), {0.0, 0.0});
            for (int n = 1; n < partials && n < maxPartials; ++n) {
                // x(t) = sum b_n sin(2 pi n t): imaginary part, inverse transform.
                const double b = coefficient(w, n);
                spec[size_t(n)] = {0.0, -b / 2.0};
                spec[size_t(size_ - n)] = {0.0, b / 2.0};
            }
            fft(spec, true);
            auto& t = wt[size_t(r)];
            t.resize(size_t(size_) + 1);
            for (int i = 0; i < size_; ++i) t[size_t(i)] = float(spec[size_t(i)].real());
            if (r == 0) { // normalise every range with the full-band peak
                float peak = 0.f;
                for (int i = 0; i < size_; ++i) peak = std::max(peak, std::fabs(t[size_t(i)]));
                normalisation = peak > 0 ? 1.0 / peak : 1.0;
            }
            for (int i = 0; i < size_; ++i) t[size_t(i)] = float(t[size_t(i)] * normalisation);
            t[size_t(size_)] = t[0];
        }
    }
}

void PeriodicWaves::tablesFor(Wave w, double fundamental, const float*& lower, const float*& higher, float& interp) const
{
    const auto& wt = tables_[size_t(w)];
    const double ratio = fundamental > 0 ? fundamental / lowestFundamental_ : 0.5;
    const double cents = std::log2(ratio) * 1200.0;
    double pitchRange = 1.0 + cents / 400.0;
    pitchRange = std::clamp(pitchRange, 0.0, double(ranges_ - 1));
    const int r1 = int(pitchRange);
    const int r2 = std::min(r1 + 1, ranges_ - 1);
    lower = wt[size_t(r1)].data();  // more partials
    higher = wt[size_t(r2)].data(); // fewer partials
    interp = float(pitchRange - r1);
}

CustomWave::CustomWave(double sampleRate, const std::vector<double>& real, const std::vector<double>& imag)
{
    size_ = sampleRate <= 24000 ? 2048 : sampleRate <= 88200 ? 4096 : 16384;
    ranges_ = int(std::lround(3.0 * std::log2(double(size_))));
    lowestFundamental_ = sampleRate / size_;
    const int maxPartials = size_ / 2;
    const int coeffs = int(std::min(real.size(), imag.size()));
    tables_.resize(size_t(ranges_));
    double normalisation = 1.0;
    for (int r = 0; r < ranges_; ++r) {
        const double culling = std::pow(2.0, -(r * 400.0) / 1200.0);
        const int partials = std::max(1, int(culling * maxPartials));
        std::vector<std::complex<double>> spec(size_t(size_), {0.0, 0.0});
        for (int n = 1; n < partials && n < maxPartials && n < coeffs; ++n) {
            // x(t) = sum a_n cos + b_n sin
            spec[size_t(n)] = {real[size_t(n)] / 2.0, -imag[size_t(n)] / 2.0};
            spec[size_t(size_ - n)] = {real[size_t(n)] / 2.0, imag[size_t(n)] / 2.0};
        }
        fft(spec, true);
        auto& t = tables_[size_t(r)];
        t.resize(size_t(size_) + 1);
        for (int i = 0; i < size_; ++i) t[size_t(i)] = float(spec[size_t(i)].real());
        if (r == 0) {
            float peak = 0.f;
            for (int i = 0; i < size_; ++i) peak = std::max(peak, std::fabs(t[size_t(i)]));
            normalisation = peak > 0 ? 1.0 / peak : 1.0;
        }
        for (int i = 0; i < size_; ++i) t[size_t(i)] = float(t[size_t(i)] * normalisation);
        t[size_t(size_)] = t[0];
    }
}

void CustomWave::tablesFor(double fundamental, const float*& lower, const float*& higher, float& interp) const
{
    const double ratio = fundamental > 0 ? fundamental / lowestFundamental_ : 0.5;
    double pitchRange = std::clamp(1.0 + std::log2(ratio) * 1200.0 / 400.0, 0.0, double(ranges_ - 1));
    const int r1 = int(pitchRange), r2 = std::min(r1 + 1, ranges_ - 1);
    lower = tables_[size_t(r1)].data();
    higher = tables_[size_t(r2)].data();
    interp = float(pitchRange - r1);
}

WaveStore& WaveStore::instance()
{
    static WaveStore store;
    return store;
}

uint32_t WaveStore::getOrBuild(const std::string& key, double sampleRate, const std::vector<double>& real,
                               const std::vector<double>& imag)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [k, id] : keys_)
        if (k == key) return id;
    const uint32_t id = uint32_t(keys_.size() + 1);
    if (id >= kMax) return 0;
    owned_.push_back(std::make_unique<CustomWave>(sampleRate, real, imag));
    slots_[id].store(owned_.back().get(), std::memory_order_release);
    keys_.emplace_back(key, id);
    return id;
}

void Biquad::setParams(double sr, double frequency, double q, double gainDb)
{
    if (frequency == lastF_ && q == lastQ_ && gainDb == lastG_ && type == lastType_) return;
    lastF_ = frequency;
    lastQ_ = q;
    lastG_ = gainDb;
    lastType_ = type;
    const double nyquist = sr / 2.0;
    const double f0 = std::clamp(frequency, 0.0, nyquist);
    const double normalized = f0 / nyquist; // Chromium works with frequency / nyquist
    const double A = std::pow(10.0, gainDb / 40.0);
    auto set = [&](double b0, double b1, double b2, double a0, double a1, double a2) {
        b0_ = b0 / a0;
        b1_ = b1 / a0;
        b2_ = b2 / a0;
        a1_ = a1 / a0;
        a2_ = a2 / a0;
    };
    const double w0 = kPi * normalized;
    const double cw = std::cos(w0), sw = std::sin(w0);
    switch (type) {
        case FilterType::Lowpass: {
            if (normalized >= 1.0) { set(1, 0, 0, 1, 0, 0); break; }
            if (normalized <= 0.0) { set(0, 0, 0, 1, 0, 0); break; }
            const double alpha = sw / (2.0 * std::pow(10.0, q / 20.0));
            set((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
            break;
        }
        case FilterType::Highpass: {
            if (normalized >= 1.0) { set(0, 0, 0, 1, 0, 0); break; }
            if (normalized <= 0.0) { set(1, 0, 0, 1, 0, 0); break; }
            const double alpha = sw / (2.0 * std::pow(10.0, q / 20.0));
            set((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
            break;
        }
        case FilterType::Bandpass: {
            if (normalized <= 0.0 || normalized >= 1.0 || q <= 0.0) {
                set(normalized > 0.0 && normalized < 1.0 ? 1 : 0, 0, 0, 1, 0, 0);
                break;
            }
            const double alpha = sw / (2.0 * q);
            set(alpha, 0, -alpha, 1 + alpha, -2 * cw, 1 - alpha);
            break;
        }
        case FilterType::Notch: {
            if (normalized <= 0.0 || normalized >= 1.0) { set(1, 0, 0, 1, 0, 0); break; }
            if (q <= 0.0) { set(0, 0, 0, 1, 0, 0); break; }
            const double alpha = sw / (2.0 * q);
            set(1, -2 * cw, 1, 1 + alpha, -2 * cw, 1 - alpha);
            break;
        }
        case FilterType::Allpass: {
            if (normalized <= 0.0 || normalized >= 1.0) { set(1, 0, 0, 1, 0, 0); break; }
            const double alpha = sw / (2.0 * std::max(q, 1e-9));
            set(1 - alpha, -2 * cw, 1 + alpha, 1 + alpha, -2 * cw, 1 - alpha);
            break;
        }
        case FilterType::Peaking: {
            const double alpha = sw / (2.0 * std::max(q, 1e-9));
            set(1 + alpha * A, -2 * cw, 1 - alpha * A, 1 + alpha / A, -2 * cw, 1 - alpha / A);
            break;
        }
        case FilterType::Lowshelf: {
            const double alpha = sw / 2.0 * std::sqrt(2.0), k = 2 * std::sqrt(A) * alpha;
            set(A * ((A + 1) - (A - 1) * cw + k), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - k),
                (A + 1) + (A - 1) * cw + k, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - k);
            break;
        }
        case FilterType::Highshelf: {
            const double alpha = sw / 2.0 * std::sqrt(2.0), k = 2 * std::sqrt(A) * alpha;
            set(A * ((A + 1) + (A - 1) * cw + k), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - k),
                (A + 1) - (A - 1) * cw + k, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - k);
            break;
        }
    }
}

void NoiseBuffers::generate(NoiseType type, double density, std::vector<float>& out, uint32_t& seed)
{
    double lastOut = 0, b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    for (auto& o : out) {
        const double white = randomUnit(seed) * 2.0 - 1.0;
        switch (type) {
            case NoiseType::White: o = float(white); break;
            case NoiseType::Brown:
                o = float((lastOut + 0.02 * white) / 1.02);
                lastOut = o;
                break;
            case NoiseType::Pink: {
                b0 = 0.99886 * b0 + white * 0.0555179;
                b1 = 0.99332 * b1 + white * 0.0750759;
                b2 = 0.969 * b2 + white * 0.153852;
                b3 = 0.8665 * b3 + white * 0.3104856;
                b4 = 0.55 * b4 + white * 0.5329522;
                b5 = -0.7616 * b5 - white * 0.016898;
                o = float((b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362) * 0.11);
                b6 = white * 0.115926;
                break;
            }
            case NoiseType::Crackle:
                o = randomUnit(seed) < density * 0.01 ? float(randomUnit(seed) * 2.0 - 1.0) : 0.f;
                break;
        }
    }
}

NoiseBuffers::NoiseBuffers(double sampleRate)
{
    uint32_t seed = 0x9e3779b9u;
    for (NoiseType t : {NoiseType::White, NoiseType::Pink, NoiseType::Brown, NoiseType::Crackle}) {
        auto& b = buffers_[size_t(t)];
        b.resize(size_t(2 * sampleRate));
        generate(t, 0.03, b, seed);
    }
}

} // namespace concha::sd
