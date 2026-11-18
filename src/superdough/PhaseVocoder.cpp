#include "superdough/PhaseVocoder.h"

#include <algorithm>
#include <cmath>

namespace concha::sd {

namespace {
constexpr double kTwoPi = 6.28318530717958647692;
inline double fround(double x) { return std::floor(x + 0.5); }
inline double fceil(double x) { return std::floor(x + 1); }
} // namespace

PhaseVocoder::PhaseVocoder()
{
    for (auto& b : input_) b.assign(kBlock + kHop, 0.f);
    for (auto& b : output_) b.assign(kBlock, 0.f);
    for (auto& b : toRetrieve_) b.assign(kBlock, 0.f);
    hann_.resize(kBlock);
    for (int i = 0; i < kBlock; ++i) hann_[size_t(i)] = float(0.5 * (1 - std::cos((kTwoPi * i) / kBlock)));
    magnitudes_.assign(kBlock / 2 + 1, 0.f);
    peaks_.assign(magnitudes_.size(), 0);
    send_.assign(kBlock, 0.f);
    re_.assign(kBlock, 0);
    im_.assign(kBlock, 0);
    sre_.assign(kBlock, 0);
    sim_.assign(kBlock, 0);
    bitrev_.resize(kBlock);
    int bits = 0;
    while ((1 << bits) < kBlock) ++bits;
    for (int i = 0; i < kBlock; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
        bitrev_[size_t(i)] = r;
    }
    // fft.js constructor
    freq_.assign(size_t(2 * kBlock), 0.0);
    table_.resize(size_t(2 * kBlock));
    for (int i = 0; i < 2 * kBlock; i += 2) {
        const double angle = (3.14159265358979323846 * i) / kBlock;
        table_[size_t(i)] = std::cos(angle);
        table_[size_t(i + 1)] = -std::sin(angle);
    }
    int power = 0;
    for (int t = 1; kBlock > t; t <<= 1) ++power;
    width_ = power % 2 == 0 ? power - 1 : power;
    jsBitrev_.assign(size_t(1) << width_, 0u);
    for (uint32_t j = 0; j < jsBitrev_.size(); ++j)
        for (int shift = 0; shift < width_; shift += 2) {
            const int revShift = width_ - shift - 2;
            const uint32_t bitsJ = (j >> shift) & 3u;
            jsBitrev_[j] |= revShift >= 0 ? bitsJ << revShift : (bitsJ << 31); // JS: x << -1 == x << 31
        }
    cosT_.resize(kBlock / 2);
    sinT_.resize(kBlock / 2);
    for (int i = 0; i < kBlock / 2; ++i) {
        cosT_[size_t(i)] = std::cos(kTwoPi * i / kBlock);
        sinT_[size_t(i)] = std::sin(kTwoPi * i / kBlock);
    }
}

void PhaseVocoder::reset()
{
    for (auto& b : input_) std::fill(b.begin(), b.end(), 0.f);
    for (auto& b : output_) std::fill(b.begin(), b.end(), 0.f);
    for (auto& q : inQ_) q.fill(0.f);
    for (auto& q : outQ_) q.fill(0.f);
}

void PhaseVocoder::fft(std::vector<double>& re, std::vector<double>& im, bool inverse) const
{
    const int n = kBlock;
    for (int i = 0; i < n; ++i) {
        const int j = bitrev_[size_t(i)];
        if (j > i) {
            std::swap(re[size_t(i)], re[size_t(j)]);
            std::swap(im[size_t(i)], im[size_t(j)]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1, step = n / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < half; ++k) {
                const double wr = cosT_[size_t(k * step)], wi = inverse ? sinT_[size_t(k * step)] : -sinT_[size_t(k * step)];
                const size_t a = size_t(i + k), b = size_t(i + k + half);
                const double xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
    }
}

void PhaseVocoder::realTransform(const std::vector<float>& data)
{
    std::vector<double>& out = freq_;
    const int size = 2 * kBlock;
    int step = 1 << width_;
    int len = (size / step) << 1;
    if (len == 4) {
        for (int outOff = 0, t = 0; outOff < size; outOff += len, ++t) { // _singleRealTransform2
            const uint32_t off = jsBitrev_[size_t(t)] >> 1, st = uint32_t(step) >> 1;
            const double evenR = data[off], oddR = data[off + st];
            out[size_t(outOff)] = evenR + oddR;
            out[size_t(outOff + 1)] = 0;
            out[size_t(outOff + 2)] = evenR - oddR;
            out[size_t(outOff + 3)] = 0;
        }
    } else {
        for (int outOff = 0, t = 0; outOff < size; outOff += len, ++t) { // _singleRealTransform4
            const uint32_t off = jsBitrev_[size_t(t)] >> 1, st = uint32_t(step) >> 1;
            const double Ar = data[off], Br = data[off + st], Cr = data[off + 2 * st], Dr = data[off + 3 * st];
            const double T0r = Ar + Cr, T1r = Ar - Cr, T2r = Br + Dr, T3r = Br - Dr;
            out[size_t(outOff)] = T0r + T2r;
            out[size_t(outOff + 1)] = 0;
            out[size_t(outOff + 2)] = T1r;
            out[size_t(outOff + 3)] = -T3r;
            out[size_t(outOff + 4)] = T0r - T2r;
            out[size_t(outOff + 5)] = 0;
            out[size_t(outOff + 6)] = T1r;
            out[size_t(outOff + 7)] = T3r;
        }
    }
    const double* table = table_.data();
    for (step >>= 2; step >= 2; step >>= 2) {
        len = (size / step) << 1;
        const int halfLen = len >> 1, quarterLen = halfLen >> 1, hquarterLen = quarterLen >> 1;
        for (int outOff = 0; outOff < size; outOff += len) {
            for (int i = 0, k = 0; i <= hquarterLen; i += 2, k += step) {
                const int A = outOff + i, B = A + quarterLen, C = B + quarterLen, D = C + quarterLen;
                const double Ar = out[size_t(A)], Ai = out[size_t(A + 1)], Br = out[size_t(B)], Bi = out[size_t(B + 1)];
                const double Cr = out[size_t(C)], Ci = out[size_t(C + 1)], Dr = out[size_t(D)], Di = out[size_t(D + 1)];
                const double MAr = Ar, MAi = Ai;
                const double tBr = table[k], tBi = table[k + 1];
                const double MBr = Br * tBr - Bi * tBi, MBi = Br * tBi + Bi * tBr;
                const double tCr = table[2 * k], tCi = table[2 * k + 1];
                const double MCr = Cr * tCr - Ci * tCi, MCi = Cr * tCi + Ci * tCr;
                const double tDr = table[3 * k], tDi = table[3 * k + 1];
                const double MDr = Dr * tDr - Di * tDi, MDi = Dr * tDi + Di * tDr;
                const double T0r = MAr + MCr, T0i = MAi + MCi, T1r = MAr - MCr, T1i = MAi - MCi;
                const double T2r = MBr + MDr, T2i = MBi + MDi, T3r = MBr - MDr, T3i = MBi - MDi;
                out[size_t(A)] = T0r + T2r;
                out[size_t(A + 1)] = T0i + T2i;
                out[size_t(B)] = T1r + T3i;
                out[size_t(B + 1)] = T1i - T3r;
                if (i == 0) {
                    out[size_t(C)] = T0r - T2r;
                    out[size_t(C + 1)] = T0i - T2i;
                    continue;
                }
                if (i == hquarterLen) continue;
                const double ST0r = T1r, ST0i = -T1i, ST1r = T0r, ST1i = -T0i;
                const double ST2r = -T3i, ST2i = -T3r, ST3r = -T2i, ST3i = -T2r;
                const int SA = outOff + quarterLen - i, SB = outOff + halfLen - i;
                out[size_t(SA)] = ST0r + ST2r;
                out[size_t(SA + 1)] = ST0i + ST2i;
                out[size_t(SB)] = ST1r + ST3i;
                out[size_t(SB + 1)] = ST1i - ST3r;
            }
        }
    }
}

void PhaseVocoder::endQuantum(int channels, double pitchFactor, double timeCursor)
{
    if (pitchFactor < 0) pitchFactor *= 0.25;
    pitchFactor = std::max(0.0, pitchFactor + 1);
    const int nbOverlaps = kBlock / kHop;
    const int nMag = int(magnitudes_.size());
    for (int c = 0; c < channels; ++c) {
        auto& in = input_[size_t(c)];
        // readInputs, shiftInputBuffers, prepareInputBuffersToSend
        std::copy(inQ_[size_t(c)].begin(), inQ_[size_t(c)].end(), in.begin() + kBlock);
        std::copy(in.begin() + kHop, in.end(), in.begin());
        for (int i = 0; i < kBlock; ++i) send_[size_t(i)] = in[size_t(i)];
        // processOLA
        for (int i = 0; i < kBlock; ++i) send_[size_t(i)] = float(send_[size_t(i)] * (hann_[size_t(i)] * 1.62));
        realTransform(send_);
        for (int i = 0; i < kBlock; ++i) {
            re_[size_t(i)] = freq_[size_t(2 * i)];
            im_[size_t(i)] = freq_[size_t(2 * i + 1)];
        }
        for (int i = 0; i < nMag; ++i) magnitudes_[size_t(i)] = float(re_[size_t(i)] * re_[size_t(i)] + im_[size_t(i)] * im_[size_t(i)]);
        int nbPeaks = 0;
        for (int i = 2; i < nMag - 2;) { // findPeaks
            const float mag = magnitudes_[size_t(i)];
            if (magnitudes_[size_t(i - 1)] >= mag || magnitudes_[size_t(i - 2)] >= mag) { ++i; continue; }
            if (magnitudes_[size_t(i + 1)] >= mag || magnitudes_[size_t(i + 2)] >= mag) { ++i; continue; }
            peaks_[size_t(nbPeaks++)] = i;
            i += 2;
        }
        std::fill(sre_.begin(), sre_.end(), 0.0);
        std::fill(sim_.begin(), sim_.end(), 0.0);
        for (int k = 0; k < nbPeaks; ++k) { // shiftPeaks
            const int peak = peaks_[size_t(k)];
            const int shifted = int(fround(peak * pitchFactor));
            if (shifted > nMag) break;
            int startIndex = 0, endIndex = kBlock;
            if (k > 0) startIndex = peak - int(fround((peak - peaks_[size_t(k - 1)]) / 2.0));
            if (k < nbPeaks - 1) endIndex = peak + int(fceil((peaks_[size_t(k + 1)] - peak) / 2.0));
            const double omegaDelta = kTwoPi * (1.0 / kBlock) * (shifted - peak);
            const double pr = std::cos(omegaDelta * timeCursor), pi = std::sin(omegaDelta * timeCursor);
            for (int j = startIndex - peak; j < endIndex - peak; ++j) {
                const int bin = peak + j, binShifted = shifted + j;
                if (binShifted >= nMag) break;
                if (bin < 0 || bin >= kBlock || binShifted < 0) continue; // outside the typed arrays: NaN ignored
                const double vr = re_[size_t(bin)], vi = im_[size_t(bin)];
                sre_[size_t(binShifted)] += vr * pr - vi * pi;
                sim_[size_t(binShifted)] += vr * pi + vi * pr;
            }
        }
        for (int i = 1; i < kBlock / 2; ++i) { // completeSpectrum
            sre_[size_t(kBlock - i)] = sre_[size_t(i)];
            sim_[size_t(kBlock - i)] = -sim_[size_t(i)];
        }
        fft(sre_, sim_, true);
        auto& ret = toRetrieve_[size_t(c)];
        for (int i = 0; i < kBlock; ++i) ret[size_t(i)] = float(sre_[size_t(i)] / kBlock);
        for (int i = 0; i < kBlock; ++i) ret[size_t(i)] = float(ret[size_t(i)] * (hann_[size_t(i)] * 1.62));
        // handleOutputBuffersToRetrieve, writeOutputs, shiftOutputBuffers
        auto& out = output_[size_t(c)];
        for (int i = 0; i < kBlock; ++i) out[size_t(i)] = float(out[size_t(i)] + ret[size_t(i)] / float(nbOverlaps));
        std::copy(out.begin(), out.begin() + kHop, outQ_[size_t(c)].begin());
        std::copy(out.begin() + kHop, out.end(), out.begin());
        std::fill(out.end() - kHop, out.end(), 0.f);
    }
}

} // namespace concha::sd
