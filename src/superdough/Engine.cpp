#include "superdough/Engine.h"

#include "superdough/Compressor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace concha::sd {

namespace {
constexpr double kEnvMin = 0.001, kReleaseMin = 0.01, kEnvMax = 1.0;
constexpr int kQuantum = 128;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr float kNaNf = std::numeric_limits<float>::quiet_NaN();

double nanFallback(float v, double fallback = 0.0) { return std::isnan(v) ? fallback : double(v); }
double clampd(double x, double a, double b) { return x < a ? a : (x > b ? b : x); }
double fracd(double x) { return x - std::floor(x); }

// helpers.mjs drywet: stays 1 until .5, then fades out
double wetfade(double d) { return d < 0.5 ? 1.0 : 1.0 - (d - 0.5) / 0.5; }

// vowel.mjs vowelFormant (index order: see sched/SoundConvert)
struct Formant {
    float freqs[5], gains[5], qs[5];
};
const Formant kVowels[] = {
    {{660, 1120, 2750, 3000, 3350}, {1, 0.5012f, 0.0708f, 0.0631f, 0.0126f}, {80, 90, 120, 130, 140}},  // a
    {{440, 1800, 2700, 3000, 3300}, {1, 0.1995f, 0.1259f, 0.1f, 0.1f}, {70, 80, 100, 120, 120}},         // e
    {{270, 1850, 2900, 3350, 3590}, {1, 0.0631f, 0.0631f, 0.0158f, 0.0158f}, {40, 90, 100, 120, 120}},   // i
    {{430, 820, 2700, 3000, 3300}, {1, 0.3162f, 0.0501f, 0.0794f, 0.01995f}, {40, 80, 100, 120, 120}},   // o
    {{370, 630, 2750, 3000, 3400}, {1, 0.1f, 0.0708f, 0.0316f, 0.01995f}, {40, 60, 100, 120, 120}},      // u
    {{650, 1515, 2400, 3000, 3350}, {1, 0.5f, 0.1008f, 0.0631f, 0.0126f}, {80, 90, 120, 130, 140}},      // ae
    {{560, 900, 2570, 3000, 3300}, {1, 0.5f, 0.0708f, 0.0631f, 0.0126f}, {80, 90, 120, 130, 140}},       // aa
    {{500, 1430, 2300, 3000, 3300}, {1, 0.2f, 0.0708f, 0.0316f, 0.01995f}, {40, 60, 100, 120, 120}},     // oe
    {{250, 1750, 2150, 3200, 3300}, {1, 0.1f, 0.0708f, 0.0316f, 0.01995f}, {40, 60, 100, 120, 120}},     // ue
    {{400, 1460, 2400, 3000, 3300}, {1, 0.2f, 0.0708f, 0.0316f, 0.02995f}, {40, 60, 100, 120, 120}},     // y
    {{600, 1250, 2100, 3100, 3500}, {1, 0.3f, 0.0608f, 0.0316f, 0.01995f}, {40, 70, 100, 120, 130}},     // uh
    {{500, 1240, 2280, 3000, 3500}, {1, 0.1f, 0.1708f, 0.0216f, 0.02995f}, {40, 60, 100, 120, 120}},     // un
    {{600, 1480, 2450, 3200, 3300}, {1, 0.15f, 0.0708f, 0.0316f, 0.02995f}, {40, 60, 100, 120, 120}},    // en
    {{700, 1050, 2500, 3000, 3300}, {1, 0.1f, 0.0708f, 0.0316f, 0.02995f}, {40, 60, 100, 120, 120}},     // an
    {{500, 1080, 2350, 3000, 3300}, {1, 0.1f, 0.0708f, 0.0316f, 0.02995f}, {40, 60, 100, 120, 120}},     // on
};

// worklets.mjs waveshapes, by index (tri, sine, ramp, saw, square)
double lfoShape(int shape, double phase, double skew)
{
    switch (shape) {
        case 0: {
            const double x = 1 - skew;
            return phase >= skew ? 1 / x - phase / x : phase / skew;
        }
        case 1: return std::sin(2 * kPi * phase) * 0.5 + 0.5;
        case 2: return phase;
        case 3: return 1 - phase;
        case 4: return phase >= skew ? 0.0 : 1.0;
        default: return phase;
    }
}

double polyBlep(double phase, double dt)
{
    dt = std::min(dt, 1 - dt);
    const double invdt = 1 / dt;
    if (phase < dt) {
        phase *= invdt;
        return 2 * phase - phase * phase - 1;
    }
    if (phase > 1 - dt) {
        phase = (phase - 1) * invdt;
        return phase * phase + 2 * phase + 1;
    }
    return 0;
}

double fastTanh(double x)
{
    const double x2 = x * x;
    return (x * (27.0 + x2)) / (27.0 + 9.0 * x2);
}

// lfo-processor (worklets.mjs), on the voice's clock. It outputs 0 until the first render quantum
// that starts after `begin`; its phase then starts at frac(time * frequency + phaseoffset).
struct Lfo {
    bool on = false, started = false, ended = false;
    double phase = 0, frequency = 0.5, depth = 1, skew = 0.5, dcoffset = 0, curve = 1, mn = -1e9, mx = 1e9;
    double time = 0, phaseoffset = 0, begin = 0, end = 0;
    int shape = 0;
    // the parameters' own values; modulators add to them at each quantum (lfo-processor reads [0])
    double bf = 0.5, bd = 1, bdc = 0, bsk = 0.5;
    void capture() { bf = frequency; bd = depth; bdc = dcoffset; bsk = skew; }
    void apply(double f, double d, double dc, double sk)
    {
        frequency = bf + f;
        depth = bd + d;
        dcoffset = bdc + dc;
        skew = bsk + sk;
    }
    void quantum(double now) // at each render quantum start (voice time)
    {
        if (!on || ended) return;
        if (now >= end) {
            ended = true;
            return;
        }
        if (!started && now > begin) {
            started = true;
            phase = fracd(time * frequency + phaseoffset);
        }
    }
    double next(double sr)
    {
        if (!on || !started || ended) return 0.0;
        double v = (lfoShape(shape, phase, skew) + dcoffset) * depth;
        v = std::pow(v, curve);
        if (std::isnan(v)) v = 0.0;
        v = clampd(v, mn, mx);
        phase += frequency / sr;
        if (phase > 1.0) phase -= 1.0;
        return v;
    }
};

int modulationShape(int8_t s) { return s < 0 ? 0 : s; }

double clampJs(double x, double a, double b) { return std::min(std::max(x, a), b); } // util.mjs clamp

// envelope-processor (worklets.mjs): its state only moves at each process() call (currentTime is
// the quantum's start), so a quantum holds one value for its first sample and one for the others.
struct EnvMod {
    double attack = 0.005, decay = 0.14, sustain = 0, release = 0.1, acurve = 0, dcurve = 0, rcurve = 0;
    double depth = 1, mn = -1e9, mx = 1e9, end = 0;
    bool started = false, ended = false, active = false;
    int state = 0;
    double val = 0, attackStart = 0, endTime = 0, val0 = 0, val1 = 0;

    static double warp(double phase, double c)
    {
        if (phase == 0 || phase == 1) return phase;
        if (c > 0) return 1 - std::pow(1 - phase, 1 + 8 * c);
        return std::pow(phase, 1 - 8 * c);
    }
    bool paramMod = false; // attack / decay / sustain / release are modulated: run every sample
    double now_ = 0;
    void step(double now) { step(now, attack, decay, sustain, release); }
    void step(double now, double attack, double decay, double sustain, double release) // one iteration of the loop
    {
        const double susTime = endTime;
        struct St { double time, start, target, curve; };
        const St states[5] = {{std::numeric_limits<double>::infinity(), 0, 0, 0},
                              {attack, attackStart, 1, acurve},
                              {attack + decay, 1, sustain, dcurve},
                              {susTime, sustain, sustain, 0},
                              {susTime + release, sustain, 0, rcurve}};
        const St& st = states[state];
        if (st.time == 0 || st.start == st.target) val = st.target;
        else val = st.start + (st.target - st.start) * warp(std::min(1.0, now / st.time), st.curve);
        double time = st.time;
        while (now >= time) {
            state = (state + 1) % 5;
            time = states[state].time;
        }
    }
    void quantum(double now) // voice time; begin = 0 (the onset)
    {
        active = false;
        if (ended) return;
        if (now >= end) {
            ended = true;
            return;
        }
        if (now <= 0) return;
        if (!started) { // triggered
            started = true;
            state = 1;
            endTime = end;
            attackStart = val;
        }
        now_ = now;
        active = true;
        if (paramMod) return; // stepped by sampleWith()
        step(now);
        val0 = val;
        step(now);
        val1 = val; // further iterations repeat this one
    }
    // one sample with modulated parameters (AudioParam ranges applied)
    double sampleWith(double depthMod, double da, double dd, double ds, double dr)
    {
        if (!active) return 0.0;
        step(now_, std::max(0.0, attack + da), std::max(0.0, decay + dd), clampd(sustain + ds, 0, 1), std::max(0.0, release + dr));
        return clampJs(val * (depth + depthMod), mn, mx);
    }
    double next(bool first, double depthMod) const
    {
        if (!active) return 0.0;
        return clampJs((first ? val0 : val1) * (depth + depthMod), mn, mx);
    }
};

struct ModState {
    uint8_t kind = 0; // 1 lfo, 2 env, 3 bmod, 4 live control
    ModTarget target = ModTarget::None;
    int targetSlot = -1; // ModFrequency...: the voice's modulator slot
    Lfo lfo;
    double baseFrequency = 0, baseDepth = 0, baseDcoffset = 0, baseSkew = 0;
    EnvMod env;
    int bus = -1; // bmod: the bus slot read
    double dc = 0, bmodDepth = 0, bmodMin = kNaN, bmodMax = kNaN;
    int stage = -1; // the FX stage (index in the voice's stages) whose nodes it modulates, -1 = main
    // inputs from other modulators: frequency, depth, dcoffset, skew, curve, attack, decay, sustain, release
    double in[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    double baseCurve = 1;
    // kind 4, a live control: slope * (smoothed control - c0), clamped to [liveMin, liveMax]
    int liveKind = 0, liveA = 0, liveB = 0;
    double liveC0 = 0, liveSlope = 0, liveTarget = 0, liveValue = 0, liveMin = kNaN, liveMax = kNaN;
};

// ---- wavetable-oscillator-processor (worklets.mjs) ----
uint32_t hash32(uint32_t u)
{
    u = u + 0x7ed55d16u + (u << 12);
    u = u ^ 0xc761c23cu ^ (u >> 19);
    u = u + 0x165667b1u + (u << 5);
    u = (u + 0xd3a2646cu) ^ (u << 9);
    u = u + 0xfd7046c5u + (u << 3);
    u = u ^ 0xb55a4f09u ^ (u >> 16);
    return u;
}
double hash01(int64_t i) { return double(hash32(uint32_t(int32_t(i))) >> 8) / 16777216.0; }
double valueNoise(double x)
{
    const double i = std::floor(x), f = x - i;
    const double a = hash01(int64_t(i)), b = hash01(int64_t(i) + 1);
    return a + (b - a) * f;
}
double brownian(double x, int oct = 4)
{
    double amp = 0.5, sum = 0, norm = 0, freq = 1;
    for (int o = 0; o < oct; ++o) {
        sum += amp * valueNoise(x * freq);
        norm += amp;
        amp *= 0.5;
        freq *= 2;
    }
    return (sum / norm) * 2 - 1;
}
int fround(double x) { return int(x + 0.5); }
void toBits(double amt, double mn, double mx, double& b, int& n)
{
    b = mx + (mn - mx) * amt;
    n = fround(std::pow(2.0, b));
}
double warpPhase(double phase, double amt, int mode)
{
    auto mirror = [](double x) { return 1 - std::fabs(2 * x - 1); };
    switch (mode) {
        case 0: return phase;
        case 1: { // ASYM
            const double a = 0.01 + 0.99 * amt;
            return phase < a ? (0.5 * phase) / a : 0.5 + (0.5 * (phase - a)) / (1 - a);
        }
        case 2: return mirror(warpPhase(phase, amt, 1));
        case 3: return std::pow(phase, 1 + 3 * amt);
        case 4: return std::pow(phase, 1 / (1 + 3 * amt));
        case 5: return amt < 0.5 ? warpPhase(phase, 1 - 2 * amt, 3) : warpPhase(phase, 2 * amt - 1, 2);
        case 6: return std::fmod(phase * std::pow(16.0, amt * amt), 1.0);
        case 7: {
            double b;
            int n;
            toBits(amt, 2, 12, b, n);
            return std::floor(phase * n) / n;
        }
        case 8: {
            const int k = 1 + std::max(1, fround(7 * amt));
            const double x = k * phase;
            return std::fabs((x - std::floor(x)) - 0.5) * 2;
        }
        case 9: {
            const double w = clampd(0.5 + 0.49 * (2 * amt - 1), 0, 1);
            return phase < w ? (phase / w) * 0.5 : 0.5 + ((phase - w) / (1 - w)) * 0.5;
        }
        case 10: return fracd(phase + 0.5 * amt * std::sin(2 * kPi * 3 * phase));
        case 11: {
            double b;
            int n;
            toBits(amt, 1, 6, b, n);
            return fracd(phase + 0.5 * amt * std::sin(2 * kPi * n * phase));
        }
        case 12: {
            const double r = 3.7 + 0.3 * amt;
            return clampd((1 - amt) * phase + amt * (r * phase * (1 - phase)), 0, 1);
        }
        case 13: {
            double b;
            int n;
            toBits(amt, 3, 12, b, n);
            auto isPrime = [](int m) {
                if (m < 2) return false;
                if (m % 2 == 0) return m == 2;
                for (int d = 3; d * d <= m; d += 2)
                    if (m % d == 0) return false;
                return true;
            };
            while (!isPrime(n)) ++n;
            return std::floor(phase * n) / n;
        }
        case 14: {
            double b;
            int n;
            toBits(amt, 3, 12, b, n);
            const int bits = fround(b);
            const int nn = 1 << bits;
            int idx = int(std::floor(phase * nn)), r = 0;
            for (int k = 0; k < bits; ++k) {
                r = (r << 1) | (idx & 1);
                idx >>= 1;
            }
            return double(r) / nn;
        }
        case 15: return fracd(phase + 0.25 * amt * brownian(64 * phase, 4));
        case 16: {
            const double g = 2 + 4 * amt, num = phase * g, den = phase + (1 - phase) * g;
            return clampd(den > 1e-12 ? num / den : 0, 0, 1);
        }
        case 17: {
            const double gap = clampd(0.8 * amt, 0, 1), a = 0.5 * (1 - gap), b = 0.5 * (1 + gap);
            if (phase < a) return (phase / a) * 0.5;
            if (phase > b) return 0.5 * (1 + (phase - b) / (1 - b));
            return 0.5;
        }
        case 18: {
            double x = phase;
            const double r = 3.6 + 0.4 * amt;
            const int iters = 1 + fround(2 * amt);
            for (int i = 0; i < iters; ++i) x = r * x * (1 - x);
            return clampd(x, 0, 1);
        }
        case 19: {
            const double k = 1 + 10 * amt, x = phase - 0.5;
            const double y = 1 / (1 + std::exp(-k * x)), y0 = 1 / (1 + std::exp(0.5 * k)), y1 = 1 / (1 + std::exp(-0.5 * k));
            return (y - y0) / (y1 - y0);
        }
        case 20: return fracd(phase + 0.5 * std::sin(2 * kPi * phase) * amt);
        default: return phase; // FLIP and unknown
    }
}

double sampleFrame(const float* frame, int len, double phase)
{
    const double pos = phase * len;
    int i = int(pos);
    if (i >= len) i = 0;
    const double frac = pos - i;
    int i1 = i + 1;
    if (i1 >= len) i1 = 0;
    return frame[i] + (frame[i1] - frame[i]) * frac;
}

} // namespace

ADSR getADSRValues(float a, float d, float s, float r, const ADSR* defaults)
{
    const bool na = std::isnan(a), nd = std::isnan(d), ns = std::isnan(s), nr = std::isnan(r);
    if (na && nd && ns && nr) return defaults ? *defaults : ADSR{kEnvMin, kEnvMin, kEnvMax, kReleaseMin};
    const double sustain = !ns ? s : ((!na && nd) || (na && nd)) ? kEnvMax : kEnvMin;
    return {std::max(na ? 0.0 : double(a), kEnvMin), std::max(nd ? 0.0 : double(d), kEnvMin), std::min(sustain, kEnvMax),
            std::max(nr ? 0.0 : double(r), kReleaseMin)};
}

void getParamADSR(AudioParam& param, double attack, double decay, double sustain, double release, double min, double max,
                  double begin, double end, bool exponential)
{
    auto fb = [](double v) { return std::isnan(v) ? 0.0 : v; };
    attack = fb(attack);
    decay = fb(decay);
    sustain = fb(sustain);
    release = fb(release);
    if (exponential) {
        min = min == 0 ? 0.001 : min;
        max = max == 0 ? 0.001 : max;
    }
    const double range = max - min;
    const double sustainVal = min + sustain * range;
    const double duration = end - begin;
    auto slope = [](double y1, double y2, double x1, double x2) { return x2 - x1 == 0 ? 0.0 : (y2 - y1) / (x2 - x1); };
    auto envValAtTime = [&](double time) {
        double val = attack > time ? time * slope(min, max, 0, attack) + min
                                   : (time - attack) * slope(max, sustainVal, 0, decay) + max;
        if (exponential && val == 0) val = 0.001;
        return val;
    };
    auto ramp = [&](double v, double t) {
        if (exponential) param.exponentialRampToValueAtTime(float(v), t);
        else param.linearRampToValueAtTime(float(v), t);
    };
    param.setValueAtTime(float(min), begin);
    if (attack > duration) {
        ramp(envValAtTime(duration), end);
    } else if (attack + decay > duration) {
        ramp(envValAtTime(attack), begin + attack);
        ramp(envValAtTime(duration), end);
    } else {
        ramp(envValAtTime(attack), begin + attack);
        ramp(envValAtTime(attack + decay), begin + attack + decay);
        param.setValueAtTime(float(sustainVal), end);
    }
    ramp(min, end + release);
}

double distortion(int algorithm, double x, double k)
{
    auto squash = [](double v) { return v / (1 + v); };
    auto soft = [](double v, double kk) { return std::tanh(v * (1 + kk)); };
    auto fold = [](double v, double kk) {
        const double y = (1 + 0.5 * kk) * v;
        const double w = std::fmod(std::fmod(y + 1, 4.0) + 4.0, 4.0);
        return 1 - std::fabs(w - 2);
    };
    auto diode = [&](double v, double kk, bool asym) {
        const double g = 1 + 2 * kk, t = squash(std::log1p(kk)), bias = 0.07 * t;
        const double pos = soft(v + bias, 2 * kk), neg = soft(asym ? bias : -v + bias, 2 * kk);
        const double sech = 1 / std::cosh(g * bias);
        const double denom = std::max(1e-8, (asym ? 1 : 2) * g * sech * sech);
        return soft((pos - neg) / denom, kk);
    };
    switch (((algorithm % 9) + 9) % 9) {
        case 0: return ((1 + k) * x) / (1 + k * std::fabs(x)); // scurve
        case 1: return soft(x, k);                            // soft
        case 2: return clampd((1 + k) * x, -1, 1);             // hard
        case 3: {                                              // cubic
            const double t = squash(std::log1p(k));
            return soft((x - (t / 3) * x * x * x) / (1 - t / 3), k);
        }
        case 4: return diode(x, k, false);             // diode
        case 5: return diode(x, k, true);              // asym
        case 6: return fold(x, k);                     // fold
        case 7: return std::sin(kPi / 2 * fold(x, k)); // sinefold
        case 8: {                                      // chebyshev
            const double kl = 10 * std::log1p(k);
            double tnm1 = 1, tnm2 = x, y = 0;
            for (int i = 1; i < 64; ++i) {
                if (i < 2) {
                    y += tnm2;
                    continue;
                }
                const double tn = 2 * x * tnm1 - tnm2;
                tnm2 = tnm1;
                tnm1 = tn;
                if (i % 2 == 0) y += std::min((1.3 * kl) / i, 2.0) * tn;
            }
            return soft(y, kl / 20);
        }
    }
    return x;
}

// ---- orbit (superdoughoutput.mjs Orbit) ----

namespace {
// SuperCollider's Env segment shape for a curvature c (EnvGen: linear when |c| < 0.001)
double envCurve(double y1, double y2, double pos, double c)
{
    pos = std::min(1.0, std::max(0.0, pos));
    if (std::fabs(c) < 0.001) return y1 + (y2 - y1) * pos;
    return y1 + (y2 - y1) * (1.0 - std::exp(c * pos)) / (1.0 - std::exp(c));
}
} // namespace

struct Engine::Orbit {
    bool used = false;
    int number = 0;
    // StrudelDirt's orbit effects (OrbitFxServer): playing, leslie's share, the controls last sent
    bool leslieOn = false, shimmerOn = false;
    float leslieAmount = 0.5f;
    float leslieSent[3] = {kNaNf, kNaNf, kNaNf}, shimmerSent[4] = {kNaNf, kNaNf, kNaNf, kNaNf};
    int bus = 0;
    std::array<std::vector<float>, 2> dry, delaySend, roomSend;

    // delay (FeedbackDelayNode: DelayNode, max 1 s, in a feedback loop)
    bool hasDelay = false;
    std::array<std::vector<float>, 2> delayLine;
    int delayWrite = 0;
    AudioParam delayTime{0.f, 0.f, 1.f}, feedback{0.5f};
    float lastDelayTime = -1.f, lastFeedback = -1.f;

    // reverb (ConvolverNode with superdough's generated impulse response)
    bool hasReverb = false;
    std::unique_ptr<Convolver> reverb;
    std::atomic<Convolver*> pending{nullptr};
    std::atomic<Convolver*> retired{nullptr};
    double roomsize = 2, roomfade = 0.1, roomlp = 15000, roomdim = 1000; // convolver.generate defaults
    float irspeed = kNaNf, irbegin = kNaNf; // as last passed (undefined stays undefined)
    uint32_t irId = 0;
    std::atomic<bool> buildRequested{false};
    double buildParams[6] = {2, 0.1, 15000, 1000, kNaN, kNaN};
    uint32_t buildIr = 0;
    unsigned buildSeed = 1;
    int64_t lastRoomInput = -1;

    // modulators of the voices on the orbit's delay time, feedback and djf (summed per sample)
    bool modsUsed = false;
    std::array<std::vector<float>, 3> mods; // delayTime, feedback, djf
    // djf (djf-processor: two-pole filters)
    bool hasDjf = false;
    AudioParam djfValue{0.5f};
    double djfS0[2] = {0, 0}, djfS1[2] = {0, 0};
    float djfBlockValue = 0.5f;

    AudioParam outGain{1.f};
};

// ---- voice ----

// One pass of superdough's effect block (the loop over FX in superdough()): K(), transient, gain,
// filters, vowel, coarse, crush, shape, distort, tremolo, compressor, pan, phaser; the stages of an
// FX chain before the main one also have their own FeedbackDelay and reverb.
struct Engine::FxStage {
    double gain = 1.0;
    std::array<Biquad, kMaxFilters> filters;
    std::array<bool, kMaxFilters> ladder{};
    std::array<AudioParam, kMaxFilters> filterFreq;
    std::array<Lfo, kMaxFilters> filterLfo;
    std::array<double, kMaxFilters> filterQ{}, filterDrive{};
    struct LadderState {
        double p0[2] = {0, 0}, p1[2] = {0, 0}, p2[2] = {0, 0}, p3[2] = {0, 0}, p32[2] = {0, 0}, p33[2] = {0, 0}, p34[2] = {0, 0};
        double cutoff = 0, k = 0, drive = 1, makeup = 1;
    };
    std::array<LadderState, kMaxFilters> ladders{};
    int numFilters = 0;
    int vowel = -1;
    std::array<Biquad, 5> vowelFilters;
    double coarse = 0.0;
    float coarseHeld[2] = {0, 0};
    double crush = 0.0;
    bool shapeOn = false;
    double shapeAmount = 0, shapeGain = 1;
    bool distortOn = false;
    int distortType = 0;
    double distortK = 0, distortGain = 1;
    bool tremoloOn = false;
    double tremoloBase = 0;
    Lfo tremolo;
    bool compressorOn = false;
    Compressor compressor;
    // transient-processor (before the gain stage)
    bool transientOn = false;
    double trAttackCoeff = 0, trSustainCoeff = 0, trAttackAmt = 0, trSustainAmt = 0, trScaling = 0, trMix = 1;
    double trGainCoeff = 0, trAvgGain = 1, trAttackEnv[2] = {0, 0}, trSustainEnv[2] = {0, 0}, trEnd = 0;
    bool pan = false;
    float panValue = 0.f;
    bool phaserOn = false;
    Biquad phaser;
    double phaserFreq = 0, phaserQ = 1;
    Lfo phaserLfo;
    // K(): generic-processor (index in the engine's kabel states, -1 = none)
    int kabel = -1;
    bool kabelRunning = false, kabelEnded = false, kabelGateEnded = false;
    double kabelGateEnd = 0, kabelEnd = 0;
    // FX stages before the main one (key !== 'main'): their own delay and reverb
    bool stageDelayOn = false, stageRoomOn = false;
    double stageDelayTime = 0, stageDelayFb = 0, stageDelayWet = 0, stageDelayDry = 1, stageRoom = 0, stageRoomDry = 1;
    int stageDelay = -1; // the engine's stage delay line
    int stageDelayWrite = 0;
    int stageReverb = -1; // the engine's stage reverb slot
    bool modulated = false; // lfo / env / bmod reach this stage (the main one)
    // modulators reaching this stage: their sum per target for the current sample
    std::array<double, size_t(ModTarget::Count)> modSum{};
    // parameter values as the nodes hold them (modulators add to these)
    std::array<double, kMaxFilters> filterBaseFreq{};
    double coarseRaw = 0, crushRaw = 0, shapeRaw = 0, shapeVolRaw = 1, distortRaw = 0, distortVolRaw = 1;
    double compBase[5] = {0, 0, 0, 0, 0}; // threshold, ratio, knee, attack, release
    // stretch (phase-vocoder-processor), before the transient
    int vocoder = -1;
    bool vocoderEnded = false;
    int vocoderChannels = 1;
    double stretch = 0;
};

struct Engine::Voice : Engine::FxStage {
    bool active = false;
    bool fading = false;
    uint64_t id = 0;
    int startDelay = 0;
    int64_t absStart = 0; // engine clock sample of the onset
    int64_t index = 0;    // samples since the onset
    double t = 0.0;       // seconds since the onset
    double endTime = 0.0;
    Orbit* orbit = nullptr;

    SoundType type = SoundType::Synth;
    SynthWave wave = SynthWave::Triangle;
    int sourceChannels = 1;
    double sourceStop = 0.0;
    double sourceGain = 0.3;

    // oscillator synths
    Oscillator osc;
    AudioParam frequency{0.f}, detune{0.f};
    // applyFM: operators 1..8 and the edges between them (0 = the carrier's frequency)
    bool fm = false;
    struct FmOp {
        bool used = false, env = false;
        Oscillator osc;
        double freq = 0;
        AudioParam envGain{1.f};
        const std::vector<float>* noise = nullptr; // noise operators (looped 2 s buffer)
        int64_t noisePos = 0;
    };
    struct FmEdge {
        int8_t src = 0, dst = 0;
        double amt = 0;
    };
    std::array<FmOp, 9> fmOps;
    std::array<FmEdge, 72> fmEdges;
    int numFmEdges = 0;
    std::array<int8_t, 8> fmOrder{}; // operators to compute, sources before their targets
    int numFmOrder = 0;
    std::array<int8_t, 9> fmGainEdge{}; // nodes['fm_N_gain']: an edge, or -1 (a gain left unconnected)
    bool vib = false;
    Oscillator vibOsc;
    double vibFreq = 0.0, vibDepthCents = 0.0;
    bool noiseMix = false;
    double noiseDry = 1.0, noiseWet = 0.0;

    // worklet oscillators (supersaw-oscillator, pulse-oscillator): silent until the first render
    // quantum that starts after the onset, silent again from their end
    bool workletStarted = false, workletEnded = false;
    int sawVoices = 5;
    double sawFreqspread = 0.18, sawPanspread = 0.4;
    std::array<double, 100> sawPhase{};
    double phi = -kPi, Y0 = 0, Y1 = 0, B = 2.3, dphif = 0, envf = 0, pulseEnv = 1;
    AudioParam pulsewidth{0.5f};
    Lfo pwLfo;
    // wavetable-oscillator-processor
    int wtFrameLen = 2048, wtNumFrames = 1, wtVoices = 1, wtWarpMode = 0;
    double wtFreqspread = 0.18, wtPanspread = 0.7;
    AudioParam wtPosition{0.f, 0.f, 1.f}, wtWarp{0.f, 0.f, 1.f};
    Lfo wtLfo, warpLfo;
    // sbd
    AudioParam sbdGain{1.f}, sbdNoiseGain{1.f}, sbdMix{1.f};
    const float* sbdCurve = nullptr;
    int sbdCurveSize = 0;
    const std::vector<float>* sbdNoise = nullptr;
    int64_t sbdNoisePos = 0;

    // noise
    NoiseType noise = NoiseType::White;
    double density = 0.03;
    uint32_t seed = 1;
    double brown = 0, p0 = 0, p1 = 0, p2 = 0, p3 = 0, p4 = 0, p5 = 0, p6 = 0;

    // sample
    const SampleData* sample = nullptr;
    bool reverse = false, loop = false;
    double playbackRate = 1.0, position = 0.0, loopStart = 0.0, loopEnd = 0.0, sampleStart = 0.0;
    bool sampleEnded = false;

    AudioParam envGain{1.f};

    bool fxRelease = false;
    AudioParam fxReleaseGain{1.f};
    double postgain = 1.0;
    AudioParam out{1.f};
    // sends
    double delaySend = 0, roomSend = 0;
    bool dryOn = false;
    double dryGain = 1;
    // lfo / env modulators (modulators.mjs), and the sum of their outputs per target for this sample
    std::array<ModState, SoundParams::kMaxModulators> mods;
    int numMods = 0;
    bool orbitMods = false; // modulators on the orbit's delay time / feedback / djf
    double sampleDetuneMod = 0; // AudioBufferSourceNode.detune is k-rate: the quantum's first value
    std::array<int8_t, FxChain::kMaxStages> stages{}; // FX chain stages from the engine's pool
    int numStages = 0;
    int fxSlot = -1;           // StrudelDirt effects: the effects server's chain
    int scSlot = -1;           // ScSynth: the synth server's slot
    int64_t scReadFrom = 0;    // its output's first sample (absolute)
    double scTotal = 0;        // dirt_gate: totalDuration
    // a SynthDef with a gate: its control, the absolute sample it closes at, then how long its output
    // has been silent (the synth freed itself: the voice ends)
    int scGate = -1;
    bool scGateClosed = false;
    int64_t scGateAt = 0;
    int scSilent = 0;
    int analyze = 0, fftSize = 8192; // .analyze(id): this voice feeds analyser id
    // strudel_envelope: Env.adsr(attack.max(0.001), decay, hold, release, 1, curve), gate held for
    // sustain, times amp
    double scAttack = 0.001, scDecay = 0, scLevel = 1, scRelease = 0.01, scCurve = -2, scHoldTime = 0, scAmp = 1;
    int busOut = -1, busIn = -1; // .bus(n) send / s("bus") source: bus slots
    double busGain = 1;
    bool readsBus = false;
    int level = 0; // render order: after the voices writing the buses it reads

    double localTimeAt(int offset, double sr) const { return t + double(offset - std::max(0, startDelay)) / sr; }

    float noiseSample()
    {
        const double white = randomUnit(seed) * 2.0 - 1.0;
        switch (noise) {
            case NoiseType::White: return float(white);
            case NoiseType::Brown: brown = (brown + 0.02 * white) / 1.02; return float(brown);
            case NoiseType::Pink: {
                p0 = 0.99886 * p0 + white * 0.0555179;
                p1 = 0.99332 * p1 + white * 0.0750759;
                p2 = 0.969 * p2 + white * 0.153852;
                p3 = 0.8665 * p3 + white * 0.3104856;
                p4 = 0.55 * p4 + white * 0.5329522;
                p5 = -0.7616 * p5 - white * 0.016898;
                const double o = (p0 + p1 + p2 + p3 + p4 + p5 + p6 + white * 0.5362) * 0.11;
                p6 = white * 0.115926;
                return float(o);
            }
            case NoiseType::Crackle:
                return randomUnit(seed) < density * 0.01 ? float(randomUnit(seed) * 2.0 - 1.0) : 0.f;
        }
        return 0.f;
    }
};

Engine::ScEnvelope Engine::scEnvelope(const SoundParams& p, double duration)
{
    ScEnvelope e;
    e.sustain = isSet(p.scSustainValue) ? double(p.scSustainValue) : duration * p.scClip; // delta * (clip ? 1)
    e.total = e.sustain + p.scReleaseValue;
    e.attack = std::max(0.001, isSet(p.scAttack) ? double(p.scAttack) : 0.0); // attack.max(0.001)
    e.decay = isSet(p.scDecay) ? std::max(0.0, double(p.scDecay)) : 0.0;
    e.level = isSet(p.scHold) ? double(p.scHold) : (isSet(p.scDecay) ? 0.0 : 1.0); // ~hold ? ~decay.isNil
    e.release = std::max(0.0, double(p.scReleaseValue));
    e.curve = p.scCurve;
    e.amp = p.scAmp;
    return e;
}

namespace {
// strudel_envelope's EnvGen at t seconds from the synth's start: attack 0 -> 1, decay -> level, held
// while the gate (Trig.ar(1, sustain)) is open, then the release from wherever it is to 0
double dirtEnvelope(double t, const Engine::Voice& v)
{
    auto adsr = [&](double x) {
        if (x < v.scAttack) return envCurve(0, 1, x / v.scAttack, v.scCurve);
        x -= v.scAttack;
        if (x < v.scDecay) return envCurve(1, v.scLevel, x / v.scDecay, v.scCurve);
        return v.scLevel;
    };
    if (t < v.scHoldTime) return adsr(t);
    const double from = adsr(v.scHoldTime);
    if (v.scGate >= 0) return from; // the synth releases itself when its gate closes
    if (v.scRelease <= 0) return 0;
    return envCurve(from, 0, (t - v.scHoldTime) / v.scRelease, v.scCurve);
}
} // namespace

Engine::Engine()
    : voices_(std::make_unique<std::array<Voice, kMaxVoices>>()), orbits_(std::make_unique<std::array<Orbit, kMaxOrbits>>()),
      kabel_(std::make_unique<std::array<kabel::KabelState, kMaxKabel>>()),
      audioBuses_(std::make_unique<std::array<AudioBus, kMaxBuses>>()),
      stages_(std::make_unique<std::array<FxStage, kMaxStagePool>>()),
      stageReverbs_(std::make_unique<std::array<StageReverb, kMaxStageReverbs>>()),
      vocoders_(std::make_unique<std::array<PhaseVocoder, kMaxVocoders>>()),
      fxDrains_(std::make_unique<std::array<FxDrain, kMaxFxChains>>())
{
    zeroMods_.fill(0.0);
}

std::vector<std::string> Engine::takeWarnings()
{
    uint32_t bits = shortages_.exchange(0, std::memory_order_relaxed);
    if (kabel::DelayPool::instance().takeMissed()) bits |= 64;
    static const char* const what[] = {
        "more than 64 K() graphs at once: the newest play without theirs",
        "more than 64 FX stages at once: the newest voices skip the rest of their chain",
        "more than 32 stretched sounds at once: the newest are not stretched",
        "more than 16 FX stage reverbs at once: the newest stages have none",
        "more than 16 FX stage delays at once: the newest stages have none",
        "more than 32 different buses: the others are ignored",
        "K(): too many delay lines at once: the newest delay() nodes are silent",
        "more than 32 StrudelDirt instruments at once: the newest are not played",
    };
    std::vector<std::string> out;
    const auto now = std::chrono::steady_clock::now();
    for (int b = 0; b < 8; ++b) {
        if (!(bits & (1u << b))) continue;
        if (shortageReported_[size_t(b)] != std::chrono::steady_clock::time_point{} &&
            now - shortageReported_[size_t(b)] < std::chrono::seconds(10))
            continue;
        shortageReported_[size_t(b)] = now;
        out.push_back(std::string("[superdough C++] ") + what[b]);
    }
    return out;
}

int Engine::busSlot(double id)
{
    auto& b = *audioBuses_;
    for (int k = 0; k < kMaxBuses; ++k)
        if (b[size_t(k)].used && b[size_t(k)].id == id) return k;
    for (int k = 0; k < kMaxBuses; ++k)
        if (!b[size_t(k)].used) {
            b[size_t(k)].used = true;
            b[size_t(k)].id = id;
            for (auto& c : b[size_t(k)].buf) c.fill(0.f);
            return k;
        }
    shortage(32);
    return -1;
}

void Engine::releaseSynth(Voice& v)
{
    if (v.scSlot >= 0 && synthServer_) synthServer_->stop(v.scSlot);
    v.scSlot = -1;
    if (v.fxSlot >= 0) { // its last input was written up to now: the drain stops kLatency later
        FxDrain& d = (*fxDrains_)[size_t(v.fxSlot)];
        d.voiceDone = true;
        d.end = std::max(d.start, v.absStart + v.index); // the next sample it would have written
        v.fxSlot = -1;
    }
}

void Engine::releaseStages(Voice& v)
{
    releaseKabel(v);
    releaseVocoder(v);
    for (int k = 0; k < v.numStages; ++k) {
        const int slot = v.stages[size_t(k)];
        FxStage& st = (*stages_)[size_t(slot)];
        releaseKabel(st);
        releaseVocoder(st);
        if (st.stageDelay >= 0) {
            stageDelayBusy_[size_t(st.stageDelay)] = false;
            st.stageDelay = -1;
        }
        if (st.stageReverb >= 0) {
            (*stageReverbs_)[size_t(st.stageReverb)].state.store(3, std::memory_order_release); // the builder frees it
            st.stageReverb = -1;
        }
        stageBusy_[size_t(slot)] = false;
    }
    v.numStages = 0;
}

int Engine::acquireStage()
{
    for (int k = 0; k < kMaxStagePool; ++k)
        if (!stageBusy_[size_t(k)]) {
            stageBusy_[size_t(k)] = true;
            return k;
        }
    return -1;
}

int Engine::requestStageReverb(const SoundParams& p)
{
    for (int k = 0; k < kMaxStageReverbs; ++k) {
        StageReverb& r = (*stageReverbs_)[size_t(k)];
        if (r.state.load(std::memory_order_acquire) != 0) continue;
        // createReverb(fx.roomsize, fx.roomfade, fx.roomlp, fx.roomdim, ir, irspeed, irbegin): generate()'s defaults
        r.params[0] = nanFallback(p.roomsize, 2.0);
        r.params[1] = nanFallback(p.roomfade, 0.1);
        r.params[2] = nanFallback(p.roomlp, 15000.0);
        r.params[3] = nanFallback(p.roomdim, 1000.0);
        r.params[4] = p.irspeed;
        r.params[5] = p.irbegin;
        r.ir = p.irSampleId;
        if (synchronousReverb_) {
            auto ir = buildImpulse(r.params, r.ir, stageSeed_);
            if (!ir) return -1;
            r.ready.store(new Convolver(*ir), std::memory_order_release);
            r.state.store(2, std::memory_order_release);
        } else {
            r.state.store(1, std::memory_order_release);
        }
        return k;
    }
    return -1;
}

void Engine::releaseVocoder(FxStage& v)
{
    if (v.vocoder < 0) return;
    vocoderBusy_[size_t(v.vocoder)] = false;
    v.vocoder = -1;
}

void Engine::releaseKabel(FxStage& v)
{
    if (v.kabel < 0) return;
    (*kabel_)[size_t(v.kabel)].stop();
    kabelBusy_[size_t(v.kabel)] = false;
    v.kabel = -1;
}

Engine::~Engine()
{
    {
        std::lock_guard<std::mutex> lock(builderMutex_);
        builderStop_ = true;
    }
    builderCv_.notify_all();
    if (builder_.joinable()) builder_.join();
    for (auto& o : *orbits_) {
        delete o.pending.exchange(nullptr);
        delete o.retired.exchange(nullptr);
    }
}

void Engine::prepare(double sampleRate, bool synchronousReverb)
{
    if (!waves_ || waves_->sampleRate() != sampleRate) {
        waves_ = std::make_unique<PeriodicWaves>(sampleRate);
        noises_ = std::make_unique<NoiseBuffers>(sampleRate);
        // synth.mjs makeSaturationCurve(2, ctx.sampleRate)
        sbdCurve_.resize(size_t(sampleRate));
        for (size_t i = 0; i < sbdCurve_.size(); ++i)
            sbdCurve_[i] = float(std::tanh(((double(i) * 2.0) / double(sbdCurve_.size()) - 1) * 2));
    }
    sampleRate_ = sampleRate;
    synchronousReverb_ = synchronousReverb;
    for (auto& o : *orbits_) {
        for (int c = 0; c < 2; ++c) {
            o.dry[size_t(c)].assign(kMaxBlock, 0.f);
            o.delaySend[size_t(c)].assign(kMaxBlock, 0.f);
            o.roomSend[size_t(c)].assign(kMaxBlock, 0.f);
            o.delayLine[size_t(c)].assign(size_t(sampleRate * 1.0) + kQuantum * 4, 0.f);
        }
        for (auto& m : o.mods) m.assign(kMaxBlock, 0.f);
    }
    stageDelays_.resize(kMaxStageDelays);
    for (auto& lines : stageDelays_)
        for (auto& line : lines) line.assign(size_t(sampleRate * 1.0) + kQuantum * 4, 0.f);
    if (!synchronousReverb && !builder_.joinable()) builder_ = std::thread([this] { builderLoop(); });
    reset();
}

void Engine::reset()
{
    for (auto& v : *voices_) {
        v.active = false;
        releaseStages(v);
        releaseSynth(v);
    }
    for (auto& b : *audioBuses_) b.used = false;
    if (orbitFx_) orbitFx_->reset();
    for (auto& o : *orbits_) {
        o.leslieOn = o.shimmerOn = false;
        o.leslieAmount = 0.5f;
        std::fill(std::begin(o.leslieSent), std::end(o.leslieSent), kNaNf);
        std::fill(std::begin(o.shimmerSent), std::end(o.shimmerSent), kNaNf);
    }
    if (fxDrains_)
        for (int k = 0; k < int(fxDrains_->size()); ++k)
            if ((*fxDrains_)[size_t(k)].active) {
                if (fxServer_) fxServer_->stop(k);
                (*fxDrains_)[size_t(k)].active = false;
            }
    // FX stage reverbs draw their noise from orbit 1's seed sequence (one per response), as the
    // reference harness's single generator does
    stageSeed_ = 0x9e3779b9u * 8u;
    for (auto& o : *orbits_) {
        o.used = false;
        o.hasDelay = o.hasReverb = o.hasDjf = false;
        o.reverb.reset();
        o.outGain.reset(1.f);
        o.delayTime.reset(0.f);
        o.feedback.reset(0.5f);
        o.djfValue.reset(0.5f);
        o.lastDelayTime = o.lastFeedback = -1.f;
        o.roomsize = 2;
        o.roomfade = 0.1;
        o.roomlp = 15000;
        o.roomdim = 1000;
        o.irspeed = o.irbegin = kNaNf;
        o.irId = 0;
        o.lastRoomInput = -1;
        o.delayWrite = 0;
        for (int c = 0; c < 2; ++c) {
            std::fill(o.delayLine[size_t(c)].begin(), o.delayLine[size_t(c)].end(), 0.f);
            o.djfS0[c] = o.djfS1[c] = 0;
        }
    }
    cutGroups_.fill(0);
    active_ = 0;
    absSample_ = 0;
}

Engine::Orbit* Engine::getOrbit(int number, int channel)
{
    auto& orbits = *orbits_;
    Orbit* freeSlot = nullptr;
    for (auto& o : orbits) {
        if (o.used && o.number == number) return &o;
        if (!o.used && !freeSlot) freeSlot = &o;
    }
    if (!freeSlot) return &orbits[0];
    Orbit& o = *freeSlot;
    o.used = true;
    o.number = number;
    // superdough: multiChannelOrbits && orbit > 0 ? [orbit * 2 - 1, orbit * 2] : [1, 2]; .channels() wins
    o.bus = channel >= 0 ? channel / 2 : (number > 0 ? number - 1 : 0);
    o.buildSeed = unsigned(0x9e3779b9u * unsigned(number + 7));
    return &o;
}

std::unique_ptr<ImpulseResponse> Engine::buildImpulse(const double* bp, uint32_t irId, unsigned& seed)
{
    if (irId == 0) return generateReverb(sampleRate_, bp[0], bp[1], bp[2], bp[3], seed++);
    const SampleData* s = store_ ? store_->get(irId) : nullptr;
    if (!s || s->frames <= 0) return nullptr;
    // decodeAudioData: at the context's rate (linear interpolation here)
    const double ratio = s->sampleRate / sampleRate_;
    const int64_t len = std::max<int64_t>(1, int64_t(std::llround(double(s->frames) / ratio)));
    std::vector<float> src(size_t(len * s->channels));
    for (int c = 0; c < s->channels; ++c) {
        const float* d = s->channel(c);
        for (int64_t i = 0; i < len; ++i) {
            const double x = double(i) * ratio;
            const int64_t a = std::min<int64_t>(int64_t(x), s->frames - 1), b = std::min<int64_t>(a + 1, s->frames - 1);
            src[size_t(c * len + i)] = float(d[a] + (d[b] - d[a]) * (x - double(a)));
        }
    }
    // reverb.mjs adjustLength(duration, buffer, irspeed = 1, irbegin = 0): same length as the
    // buffer, the first sampleRate * duration samples read at |speed| from irbegin (backwards
    // through Array.at() when speed < 1), the rest silent
    const double speed = std::isnan(bp[4]) ? 1.0 : bp[4], begin = std::isnan(bp[5]) ? 0.0 : bp[5];
    const int64_t offset = int64_t(std::floor(clampJs(begin, 0, 1) * double(len)));
    const double newLength = sampleRate_ * bp[0];
    auto ir = std::make_unique<ImpulseResponse>();
    ir->channels = s->channels;
    ir->length = int(len);
    ir->sampleRate = sampleRate_;
    ir->data.assign(size_t(len * s->channels), 0.f);
    for (int c = 0; c < s->channels; ++c) {
        const float* d = src.data() + c * len;
        float* out = ir->data.data() + c * len;
        for (int64_t i = 0; i < len && double(i) < newLength; ++i) {
            double pos = std::fmod(double(offset) + double(i) * std::fabs(speed), double(len));
            if (speed < 1) pos = -pos;
            int64_t k = int64_t(std::trunc(pos));
            if (k < 0) k += len;
            out[i] = k >= 0 && k < len ? d[k] : 0.f;
        }
    }
    return ir;
}

void Engine::builderLoop()
{
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(builderMutex_);
            builderCv_.wait_for(lock, std::chrono::milliseconds(5), [this] { return builderStop_; });
            if (builderStop_) return;
        }
        for (auto& r : *stageReverbs_) {
            const int state = r.state.load(std::memory_order_acquire);
            if (state == 1) {
                auto ir = buildImpulse(r.params, r.ir, stageSeed_);
                if (!ir) continue; // still loading
                r.ready.store(new Convolver(*ir), std::memory_order_release);
                int expected = 1;
                if (!r.state.compare_exchange_strong(expected, 2)) { // released meanwhile
                    delete r.ready.exchange(nullptr);
                    r.state.store(0);
                }
            } else if (state == 3) {
                delete r.ready.exchange(nullptr);
                r.state.store(0, std::memory_order_release);
            }
        }
        for (auto& o : *orbits_) {
            delete o.retired.exchange(nullptr);
            if (o.buildRequested.load()) {
                auto ir = buildImpulse(o.buildParams, o.buildIr, o.buildSeed);
                if (!ir) continue; // the impulse response sample is still loading: try again
                o.buildRequested = false;
                delete o.pending.exchange(new Convolver(*ir));
            }
        }
    }
}

void Engine::setupStage(FxStage& v, const SoundParams& p, bool main, double end, double endWithRelease, double cps,
                        double cycle, uint32_t seed)
{
    const double sr = sampleRate_;
    const double absoluteTime = cps > 0 ? cycle / cps : 0.0;
    v.numFilters = 0;
    v.vowel = -1;
    v.coarse = v.crush = 0;
    v.shapeOn = v.distortOn = v.tremoloOn = v.phaserOn = v.compressorOn = v.transientOn = false;
    v.pan = false;
    v.tremolo = Lfo{};
    v.phaserLfo = Lfo{};
    v.stageDelayOn = v.stageRoomOn = false;
    releaseVocoder(v);
    if (isSet(p.stretch)) {
        for (int k = 0; k < kMaxVocoders; ++k)
            if (!vocoderBusy_[size_t(k)]) {
                vocoderBusy_[size_t(k)] = true;
                (*vocoders_)[size_t(k)].reset();
                v.vocoder = k;
                break;
            }
        if (v.vocoder < 0) shortage(4);
        v.stretch = p.stretch;
        v.vocoderEnded = false;
        v.vocoderChannels = 1;
    }
    if (isSet(p.transient)) {
        auto timeToCoeff = [&](double t) { return 1 - std::exp(-(1 / sr) / t); };
        v.transientOn = true;
        v.trGainCoeff = timeToCoeff(0.2);
        v.trAvgGain = 1;
        v.trAttackCoeff = timeToCoeff(clampd(0.003, 0.0005, 0.05));
        v.trSustainCoeff = timeToCoeff(clampd(0.08, 0.01, 0.5));
        v.trAttackAmt = clampd(p.transient, -1, 1);
        v.trSustainAmt = clampd(nanFallback(p.transsustain, 0.0), -1, 1);
        v.trScaling = 0.5 + 5 * clampd(0.1, 0, 1);
        v.trMix = 1;
        v.trAttackEnv[0] = v.trAttackEnv[1] = v.trSustainEnv[0] = v.trSustainEnv[1] = 0;
        v.trEnd = endWithRelease;
    }
    double gain = nanFallback(p.gain, 0.8);
    gain *= nanFallback(p.velocity, 1.0);
    v.gain = gain;

    auto addFilter = [&](FilterType type, float frequency, float q, float a, float d, float s, float r, float env,
                         float anchor, float rate, float sync, float depth, float depthfrequency, float dc, float skew,
                         int8_t shape) {
        const int copies = p.ftype == FilterModel::Db24 ? 2 : 1;
        for (int c = 0; c < copies && v.numFilters < kMaxFilters; ++c) {
            const int i = v.numFilters++;
            v.filters[size_t(i)].type = type;
            v.filters[size_t(i)].reset();
            v.ladder[size_t(i)] = p.ftype == FilterModel::Ladder;
            v.ladders[size_t(i)] = Voice::LadderState{};
            v.filterQ[size_t(i)] = nanFallback(q, 1.0);
            v.filterDrive[size_t(i)] = nanFallback(p.drive, 0.69);
            AudioParam& fp = v.filterFreq[size_t(i)];
            fp.reset(frequency);
            v.filterBaseFreq[size_t(i)] = frequency;
            const bool hasEnvelope = isSet(a) || isSet(d) || isSet(s) || isSet(r) || isSet(env);
            if (hasEnvelope) {
                static const ADSR fd{0.005, 0.14, 0, 0.1};
                const ADSR fe = getADSRValues(a, d, s, r, &fd);
                const double e = nanFallback(env, 1.0), an = nanFallback(anchor, 0.0);
                const double envAbs = std::fabs(e), off = envAbs * an;
                double mn = clampd(std::pow(2.0, -off) * frequency, 0.0, 20000.0);
                double mx = clampd(std::pow(2.0, envAbs - off) * frequency, 0.0, 20000.0);
                if (e < 0) std::swap(mn, mx);
                getParamADSR(fp, fe.attack, fe.decay, fe.sustain, fe.release, mn, mx, 0, end, true);
            }
            // LFO on the frequency (createFilter)
            const double lfoRate = isSet(sync) ? cps * sync : (isSet(rate) ? double(rate) : kNaN);
            const bool hasLFO = isSet(depth) || isSet(depthfrequency) || isSet(skew) || shape >= 0 || !std::isnan(lfoRate);
            Lfo& l = v.filterLfo[size_t(i)];
            l = Lfo{};
            if (hasLFO) {
                const double dep = nanFallback(depth, 1.0);
                const double modDepth = isSet(depthfrequency) ? double(depthfrequency) : dep * frequency;
                l.on = modDepth != 0;
                l.depth = modDepth;
                l.dcoffset = nanFallback(dc, -0.5);
                l.skew = nanFallback(skew, 0.5);
                l.shape = modulationShape(shape);
                l.frequency = std::isnan(lfoRate) ? cps : lfoRate;
                l.mn = -frequency + 30;
                l.mx = 20000 - frequency;
                l.time = cps > 0 ? cycle / cps : 0;
                l.curve = 1;
                l.begin = 0;
                l.end = end;
            }
        }
    };
    if (isSet(p.cutoff))
        addFilter(FilterType::Lowpass, p.cutoff, p.resonance, p.lpattack, p.lpdecay, p.lpsustain, p.lprelease, p.lpenv,
                  p.fanchor, p.lprate, p.lpsync, p.lpdepth, p.lpdepthfrequency, p.lpdc, p.lpskew, p.lpshape);
    if (isSet(p.hcutoff))
        addFilter(FilterType::Highpass, p.hcutoff, p.hresonance, p.hpattack, p.hpdecay, p.hpsustain, p.hprelease, p.hpenv,
                  p.fanchor, p.hprate, p.hpsync, p.hpdepth, p.hpdepthfrequency, p.hpdc, p.hpskew, p.hpshape);
    if (isSet(p.bandf))
        addFilter(FilterType::Bandpass, p.bandf, p.bandq, p.bpattack, p.bpdecay, p.bpsustain, p.bprelease, p.bpenv,
                  p.fanchor, p.bprate, p.bpsync, p.bpdepth, p.bpdepthfrequency, p.bpdc, p.bpskew, p.bpshape);

    if (p.vowel >= 0 && p.vowel < int(std::size(kVowels))) {
        v.vowel = p.vowel;
        for (int k = 0; k < 5; ++k) {
            auto& f = v.vowelFilters[size_t(k)];
            f.type = FilterType::Bandpass;
            f.reset();
            f.setParams(sr, kVowels[p.vowel].freqs[k], kVowels[p.vowel].qs[k], 0);
        }
    }
    if (isSet(p.coarse)) v.coarse = std::max(1.0, double(v.coarseRaw = p.coarse));
    if (isSet(p.crush)) v.crush = std::max(1.0, double(v.crushRaw = p.crush));
    if (isSet(p.shape)) {
        v.shapeOn = true;
        v.shapeRaw = p.shape;
        v.shapeVolRaw = nanFallback(p.shapevol, 1.0);
        const double sh = p.shape < 1 ? double(p.shape) : 1.0 - 4e-10;
        v.shapeAmount = (2.0 * sh) / (1.0 - sh);
        v.shapeGain = clampd(nanFallback(p.shapevol, 1.0), 0.001, 1.0);
    }
    if (isSet(p.distort)) {
        v.distortOn = true;
        v.distortType = p.distorttype;
        v.distortRaw = p.distort;
        v.distortVolRaw = nanFallback(p.distortvol, 1.0);
        v.distortK = std::expm1(double(p.distort));
        v.distortGain = clampd(nanFallback(p.distortvol, 1.0), 0.001, 1.0);
    }
    const double tremoloRate = isSet(p.tremolosync) ? cps * p.tremolosync : (isSet(p.tremolo) ? double(p.tremolo) : kNaN);
    if (!std::isnan(tremoloRate)) {
        const double depth = nanFallback(p.tremolodepth, 1.0);
        v.tremoloOn = true;
        v.tremoloBase = std::max(1 - depth, 0.0);
        Lfo& l = v.tremolo;
        l.on = true;
        l.skew = isSet(p.tremoloskew) ? double(p.tremoloskew) : (p.tremoloshape >= 0 ? 0.5 : 1.0);
        l.frequency = tremoloRate;
        l.depth = depth;
        l.time = cps > 0 ? cycle / cps : 0;
        l.dcoffset = 0;
        l.shape = modulationShape(p.tremoloshape);
        l.phaseoffset = nanFallback(p.tremolophase, 0.0);
        l.mn = 0;
        l.mx = 1;
        l.curve = 1.5;
        l.begin = 0;
        l.end = endWithRelease;
    }
    if (isSet(p.compressor)) { // getCompressor
        v.compressorOn = true;
        v.compressor.prepare(sr);
        v.compBase[0] = p.compressor;
        v.compBase[1] = nanFallback(p.compressorRatio, 10);
        v.compBase[2] = nanFallback(p.compressorKnee, 10);
        v.compBase[3] = nanFallback(p.compressorAttack, 0.005);
        v.compBase[4] = nanFallback(p.compressorRelease, 0.05);
        v.compressor.setParams(float(v.compBase[0]), float(v.compBase[2]), float(v.compBase[1]), float(v.compBase[3]), float(v.compBase[4]));
    }
    if (isSet(p.pan)) {
        v.pan = true;
        v.panValue = float(2.0 * p.pan - 1.0);
    }
    const double phaserdepth = nanFallback(p.phaserdepth, 0.75);
    if (isSet(p.phaserrate) && phaserdepth > 0) { // getPhaser
        v.phaserOn = true;
        v.phaser.type = FilterType::Notch;
        v.phaser.reset();
        v.phaserFreq = nanFallback(p.phasercenter, 1000.0) + 282;
        v.phaserQ = 2 - std::min(std::max(phaserdepth * 2, 0.0), 1.9);
        const double sweep = nanFallback(p.phasersweep, 2000.0);
        Lfo& l = v.phaserLfo;
        l.on = true;
        l.frequency = p.phaserrate;
        l.depth = sweep * 2;
        l.dcoffset = -0.5;
        l.mn = -sweep;
        l.mx = sweep;
        l.time = absoluteTime; // getLfo: time ?? begin, begin = t
        l.begin = 0;
        l.end = endWithRelease;
    }
    // K() (generic-processor): from the first quantum at or after the onset to endWithRelease, the
    // strudel gate cc at 1 until the note's end
    releaseKabel(v);
    if (const kabel::Program* prog = kabel::KabelStore::instance().get(p.kabelId)) {
        for (int k = 0; k < kMaxKabel; ++k)
            if (!kabelBusy_[size_t(k)]) {
                kabelBusy_[size_t(k)] = true;
                v.kabel = k;
                (*kabel_)[size_t(k)].start(prog, sr, seed * 2654435761u + 1u);
                break;
            }
        if (v.kabel < 0) shortage(1); // the source plays without its K() graph
        v.kabelRunning = v.kabelEnded = v.kabelGateEnded = false;
        v.kabelGateEnd = end;
        v.kabelEnd = endWithRelease;
    }
    for (int f = 0; f < v.numFilters; ++f) v.filterLfo[size_t(f)].capture();
    v.tremolo.capture();
    v.phaserLfo.capture();
    v.modSum.fill(0.0);
    v.modulated = false;
    if (main) return;
    // key !== 'main': a FeedbackDelay and a reverb of the stage's own
    const double delay = nanFallback(p.delay, 0.0), delayfeedback = nanFallback(p.delayfeedback, 0.5);
    const double delaytime = isSet(p.delaytime) ? double(p.delaytime) : nanFallback(p.delaysync, 3.0 / 16.0) / cps;
    if (v.stageDelay >= 0) {
        stageDelayBusy_[size_t(v.stageDelay)] = false;
        v.stageDelay = -1;
    }
    if (delay > 0 && delaytime > 0 && delayfeedback > 0) {
        for (int k = 0; k < kMaxStageDelays && v.stageDelay < 0; ++k)
            if (!stageDelayBusy_[size_t(k)] && !stageDelays_.empty()) {
                stageDelayBusy_[size_t(k)] = true;
                v.stageDelay = k;
            }
        if (v.stageDelay < 0) shortage(16);
    }
    if (v.stageDelay >= 0) {
        v.stageDelayOn = true;
        v.stageDelayTime = std::min(delaytime, 1.0); // DelayNode maxDelayTime 1
        v.stageDelayFb = std::min(std::fabs(clampd(delayfeedback, 0, 0.98)), 0.995);
        v.stageDelayWet = delay;
        v.stageDelayDry = nanFallback(p.dry, 1.0);
        for (auto& line : stageDelays_[size_t(v.stageDelay)]) std::fill(line.begin(), line.end(), 0.f);
        v.stageDelayWrite = 0;
    }
    const double room = nanFallback(p.room, 0.0);
    if (room > 0) {
        v.stageRoomOn = true;
        v.stageRoom = room;
        v.stageRoomDry = nanFallback(p.dry, 1.0);
        v.stageReverb = requestStageReverb(p);
        if (v.stageReverb < 0) shortage(8);
    }
}

void Engine::trigger(int offset, const SoundParams& p, double duration, double cps, double cycle)
{
    auto& voices = *voices_;
    int playing = 0;
    for (const auto& v : voices) playing += v.active && !v.fading;
    while (playing >= kMaxPolyphony) {
        Voice* oldest = nullptr;
        for (auto& v : voices)
            if (v.active && !v.fading && (!oldest || v.id < oldest->id)) oldest = &v;
        if (!oldest) break;
        const double now = oldest->localTimeAt(offset, sampleRate_);
        oldest->out.cancelScheduledValues(now);
        oldest->out.setValueAtTime(oldest->out.value(), now);
        oldest->out.linearRampToValueAtTime(0.f, now + 0.25);
        oldest->endTime = std::min(oldest->endTime, now + 0.25);
        oldest->fading = true;
        --playing;
    }
    Voice* slot = nullptr;
    for (auto& v : voices)
        if (!v.active) {
            slot = &v;
            break;
        }
    if (!slot) {
        slot = &voices[0];
        for (auto& v : voices)
            if (v.id < slot->id) slot = &v;
    }
    const int index = int(slot - voices.data());
    if (!start(*slot, offset, p, duration, cps, cycle)) return;

    if (p.type == SoundType::Sample && isSet(p.cut)) {
        const int g = int(std::lround(p.cut)) & 63;
        if (const int prev = cutGroups_[size_t(g)]; prev > 0 && prev - 1 != index) {
            Voice& pv = voices[size_t(prev - 1)];
            if (pv.active) {
                const double now = pv.localTimeAt(offset, sampleRate_) + nanFallback(p.nudge);
                pv.out.setValueAtTime(1.f, now);
                pv.out.linearRampToValueAtTime(0.f, now + 0.01);
                pv.endTime = std::min(pv.endTime, now + 0.01);
            }
        }
        cutGroups_[size_t(g)] = index + 1;
    }
}

bool Engine::start(Voice& v, int offset, const SoundParams& p, double duration, double cps, double cycle)
{
    const double sr = sampleRate_;
    // superdough's absolute times: the onset t is cycle / cps on Strudel's clock. Some nodes use it:
    // FM modulators start at the context's start, phaser and pulse-width LFOs start their phase at t.
    const double absoluteTime = cps > 0 ? cycle / cps : 0.0;
    const double eventTime = double(absSample_ + offset) / sr; // engine clock (orbit automations)

    v.active = false;
    v.fading = false;
    v.startDelay = offset;
    v.absStart = absSample_ + offset;
    v.index = 0;
    v.t = 0.0;
    v.type = p.type;
    v.wave = p.wave;
    v.sourceChannels = 1;
    releaseSynth(v);
    v.fm = v.vib = v.noiseMix = false;
    v.sample = nullptr;
    v.sampleEnded = false;
    v.numFilters = 0;
    v.vowel = -1;
    v.coarse = v.crush = 0;
    v.shapeOn = v.distortOn = v.tremoloOn = v.phaserOn = v.compressorOn = v.transientOn = false;
    v.pan = v.fxRelease = false;
    v.tremolo = Lfo{};
    v.phaserLfo = Lfo{};
    v.pwLfo = Lfo{};
    v.wtLfo = Lfo{};
    v.warpLfo = Lfo{};
    v.frequency.reset(0.f);
    v.detune.reset(0.f);
    v.envGain.reset(1.f);
    v.fxReleaseGain.reset(1.f);
    v.out.reset(1.f);
    v.osc = Oscillator{};
    v.vibOsc = Oscillator{};
    v.brown = v.p0 = v.p1 = v.p2 = v.p3 = v.p4 = v.p5 = v.p6 = 0.0;
    v.seed = uint32_t(0x9e3779b9u ^ (voiceCounter_ * 2654435761u)) | 1u;
    v.coarseHeld[0] = v.coarseHeld[1] = 0.f;
    v.workletStarted = v.workletEnded = false;

    const double release = nanFallback(p.release, 0.01);
    const double end = duration;
    const double fxr = nanFallback(p.FXrelease, 0.0);
    const double endWithRelease = end + std::max(release, fxr);

    auto pitchEnvelope = [&](AudioParam& param, double holdEnd) { // getPitchEnvelope
        if (!(isSet(p.pattack) || isSet(p.pdecay) || isSet(p.psustain) || isSet(p.prelease) || isSet(p.penv))) return;
        const double penv = nanFallback(p.penv, 1.0);
        const bool expo = nanFallback(p.pcurve, 0.0) == 1.0;
        static const ADSR pd{0.2, 0.001, 1, 0.001};
        const ADSR pe = getADSRValues(p.pattack, p.pdecay, p.psustain, p.prelease, &pd);
        const double panchor = isSet(p.panchor) ? double(p.panchor) : pe.sustain;
        const double cents = penv * 100.0;
        getParamADSR(param, pe.attack, pe.decay, pe.sustain, pe.release, -cents * panchor, cents - cents * panchor, 0,
                     holdEnd, expo);
    };
    auto vibrato = [&] { // getVibratoOscillator
        if (nanFallback(p.vib) > 0) {
            v.vib = true;
            v.vibFreq = p.vib;
            v.vibDepthCents = nanFallback(p.vibmod, 0.5) * 100.0;
        }
    };
    auto fm = [&](double carrier) { // applyFM(param, value, begin)
        v.numFmEdges = 0;
        v.numFmOrder = 0;
        for (auto& op : v.fmOps) op.used = false;
        v.fmGainEdge.fill(-1);
        auto operatorFor = [&](int idx) -> Voice::FmOp& {
            Voice::FmOp& op = v.fmOps[size_t(idx)];
            if (op.used) return op;
            const FmOperator& d = p.fmOps[idx];
            op = Voice::FmOp{};
            op.used = true;
            op.freq = carrier * nanFallback(d.h, 1.0); // fm(): carrfreq * harmonicityRatio
            if (d.wave >= 4) {
                static const NoiseType types[] = {NoiseType::White, NoiseType::Pink, NoiseType::Brown, NoiseType::Crackle};
                op.noise = &noises_->get(types[d.wave - 4]);
                op.noisePos = int64_t(absoluteTime * sr); // started at the context's start, looped
            } else {
                op.osc.wave = Wave(d.wave);
                op.osc.phase = fracd(op.freq * absoluteTime); // osc.start() at the context's start
            }
            if (isSet(d.attack) || isSet(d.decay) || isSet(d.sustain) || isSet(d.release)) {
                op.env = true;
                const ADSR fe = getADSRValues(d.attack, d.decay, d.sustain, d.release, nullptr);
                getParamADSR(op.envGain, fe.attack, fe.decay, fe.sustain, fe.release, 0, 1, 0, duration, !d.envLinear);
            }
            return op;
        };
        for (int i = 1; i <= 8; ++i)
            for (int j = 0; j <= 8; ++j) {
                const double amt = p.fmMatrix[i][j];
                if (!(amt != 0) || std::isnan(amt)) continue; // if (!amt) continue
                operatorFor(i);
                const int edge = v.numFmEdges;
                v.fmGainEdge[size_t(i)] = int8_t(edge); // the source's gain (connected)
                if (j != 0) {
                    operatorFor(j);
                    v.fmGainEdge[size_t(j)] = -1; // a fresh gain node, left unconnected
                    if (v.fmOps[size_t(j)].noise) { // no frequency parameter: superdough logs and skips
                        v.fmGainEdge[size_t(i)] = -1;
                        continue;
                    }
                }
                v.fmEdges[size_t(edge)] = Voice::FmEdge{int8_t(i), int8_t(j), amt};
                ++v.numFmEdges;
            }
        if (v.numFmEdges == 0) return;
        v.fm = true;
        // cycles without a DelayNode are muted (Chromium, node-web-audio-api): their operators are silent
        bool reach[9][9] = {};
        for (int e = 0; e < v.numFmEdges; ++e)
            if (v.fmEdges[size_t(e)].dst != 0) reach[v.fmEdges[size_t(e)].src][v.fmEdges[size_t(e)].dst] = true;
        for (int k = 1; k <= 8; ++k)
            for (int a = 1; a <= 8; ++a)
                for (int b = 1; b <= 8; ++b) reach[a][b] = reach[a][b] || (reach[a][k] && reach[k][b]);
        // order: an operator after every live operator that modulates it
        bool done[9] = {};
        for (int pass = 0; pass < 8; ++pass)
            for (int k = 1; k <= 8; ++k) {
                if (done[k] || !v.fmOps[size_t(k)].used || reach[k][k]) continue;
                bool ready = true;
                for (int e = 0; e < v.numFmEdges; ++e) {
                    const auto& ed = v.fmEdges[size_t(e)];
                    if (ed.dst == k && !reach[ed.src][ed.src] && !done[ed.src]) ready = false;
                }
                if (!ready) continue;
                done[k] = true;
                v.fmOrder[size_t(v.numFmOrder++)] = int8_t(k);
            }
    };

    // ---- source ----
    double sourceStop = 0.0;
    static const ADSR synthDefaults{0.001, 0.05, 0.6, 0.01};
    if (p.type == SoundType::Synth && p.wave == SynthWave::Sbd) {
        // synth.mjs 'sbd': triangle with a pitch drop through a tanh shaper, plus a brown noise click
        const double decay = nanFallback(p.decay, 0.5), pdecay = nanFallback(p.pdecay, 0.5), penv = nanFallback(p.penv, 36);
        v.osc.wave = Wave::Triangle;
        v.frequency.setValue(p.frequency);
        v.detune.setValueAtTime(float(penv * 100), -absoluteTime); // setValueAtTime(penv * 100, 0)
        v.detune.setValueAtTime(float(penv * 100), 0);
        v.detune.exponentialRampToValueAtTime(0.001f, pdecay);
        v.sbdGain.reset(1.f);
        v.sbdGain.setValueAtTime(1.f, 0.02);
        v.sbdGain.exponentialRampToValueAtTime(0.001f, 0.02 + decay);
        v.sbdNoiseGain.reset(1.f);
        v.sbdNoiseGain.setValueAtTime(1.2f, 0);
        v.sbdNoiseGain.exponentialRampToValueAtTime(0.001f, 0.025);
        double stop = decay + 0.01;
        if (isSet(p.clip)) stop = std::min(p.clip * duration, stop);
        v.sbdMix.reset(1.f);
        v.sbdMix.setValueAtTime(1.f, stop - 0.01);
        v.sbdMix.linearRampToValueAtTime(0.f, stop);
        v.sbdCurve = sbdCurve_.data();
        v.sbdCurveSize = int(sbdCurve_.size());
        v.sbdNoise = &noises_->get(NoiseType::Brown);
        v.sbdNoisePos = 0;
        sourceStop = stop;
        v.sourceGain = 1.0;
    } else if (p.type == SoundType::Synth && p.wave == SynthWave::Supersaw) {
        const ADSR env = getADSRValues(p.attack, p.decay, p.sustain, p.release, &synthDefaults);
        v.sawVoices = int(clampd(nanFallback(p.unison, 5), 1, 100));
        v.sawFreqspread = isSet(p.detune) ? double(p.detune) : isSet(p.n) ? double(p.n) : 0.18; // detune ?? n ?? 0.18
        v.sawPanspread = v.sawVoices > 1 ? clampd(nanFallback(p.spread, 0.6), 0, 1) : 0;
        for (int k = 0; k < v.sawVoices; ++k) v.sawPhase[size_t(k)] = randomUnit(v.seed); // Math.random()
        v.frequency.setValue(p.frequency);
        pitchEnvelope(v.detune, duration);
        vibrato();
        fm(p.frequency);
        getParamADSR(v.envGain, env.attack, env.decay, env.sustain, env.release, 0, 0.3 / std::sqrt(v.sawVoices), 0,
                     duration, false);
        sourceStop = duration + env.release + 0.01;
        v.sourceChannels = 2;
        v.sourceGain = 1.0;
    } else if (p.type == SoundType::Synth && p.wave == SynthWave::Pulse) {
        double pwsweep = isSet(p.pwsweep) ? double(p.pwsweep) : kNaN;
        double pwrate = isSet(p.pwrate) ? double(p.pwrate) : kNaN;
        if (std::isnan(pwsweep)) pwsweep = !std::isnan(pwrate) ? 0.3 : 0;
        if (std::isnan(pwrate)) pwrate = 1;
        const ADSR env = getADSRValues(p.attack, p.decay, p.sustain, p.release, &synthDefaults);
        sourceStop = duration + env.release + 0.01;
        v.frequency.setValue(p.frequency);
        v.pulsewidth.reset(float(nanFallback(p.pw, 0.5)));
        pitchEnvelope(v.detune, duration);
        vibrato();
        fm(p.frequency);
        getParamADSR(v.envGain, env.attack, env.decay, env.sustain, env.release, 0, 1, 0, duration, false);
        if (pwsweep != 0) {
            Lfo& l = v.pwLfo;
            l.on = true;
            l.frequency = pwrate;
            l.depth = pwsweep;
            l.dcoffset = -0.5;
            l.mn = -0.5 * pwsweep;
            l.mx = 0.5 * pwsweep;
            l.time = absoluteTime; // time ?? begin
            l.begin = 0;
            l.end = sourceStop;
        }
        v.phi = -kPi;
        v.Y0 = v.Y1 = 0;
        v.B = 2.3;
        v.dphif = v.envf = 0;
        v.sourceGain = 1.0;
    } else if (p.type == SoundType::Synth && p.wave == SynthWave::Bus) { // registerSound('bus')
        static const ADSR busDefaults{0.001, 0.05, 1, 0.01};
        const ADSR env = getADSRValues(p.attack, p.decay, p.sustain, p.release, &busDefaults);
        getParamADSR(v.envGain, env.attack, env.decay, env.sustain, env.release, 0, 1, 0, duration, false);
        sourceStop = duration + env.release + 0.01;
    } else if (p.type == SoundType::Synth || p.type == SoundType::Noise) {
        const ADSR env = getADSRValues(p.attack, p.decay, p.sustain, p.release, &synthDefaults);
        getParamADSR(v.envGain, env.attack, env.decay, env.sustain, env.release, 0, 1, 0, duration, false);
        sourceStop = duration + env.release + 0.01;
        v.sourceGain = 0.3;
        if (p.type == SoundType::Synth) {
            switch (p.wave) {
                case SynthWave::Sine: v.osc.wave = Wave::Sine; break;
                case SynthWave::Square: v.osc.wave = Wave::Square; break;
                case SynthWave::Sawtooth: v.osc.wave = Wave::Sawtooth; break;
                default: v.osc.wave = Wave::Triangle; break;
            }
            if (p.customWave) v.osc.custom = WaveStore::instance().get(p.customWave);
            v.frequency.setValue(p.frequency);
            vibrato();
            pitchEnvelope(v.detune, duration);
            fm(p.frequency);
            if (nanFallback(p.noise) != 0.0) {
                v.noiseMix = true;
                v.noise = NoiseType::Pink;
                v.noiseDry = wetfade(p.noise);
                v.noiseWet = wetfade(1.0 - p.noise);
            }
        } else {
            v.noise = NoiseType(p.noiseType);
            v.density = nanFallback(p.density, 0.03);
        }
    } else if (p.type == SoundType::Wavetable) {
        const SampleData* s = store_ ? store_->get(p.sampleId) : nullptr;
        if (!s || s->frames <= 0) {
            missingSamples_.fetch_add(1, std::memory_order_relaxed);
            return false; // "[wavetable] still loading"
        }
        v.sample = s;
        v.wtFrameLen = p.frameLen;
        v.wtNumFrames = std::max<int>(1, int(s->frames / p.frameLen));
        if (s->frames < p.frameLen) v.wtFrameLen = int(s->frames); // a short file is one frame
        const ADSR env = getADSRValues(p.attack, p.decay, p.sustain, p.release, nullptr);
        double holdEnd = duration;
        if (isSet(p.clip)) holdEnd = std::min(p.clip * duration, holdEnd);
        const double endRel = holdEnd + env.release;
        sourceStop = endRel + 0.01;
        v.frequency.setValue(p.frequency);
        v.wtFreqspread = nanFallback(p.detune, 0.18);
        v.wtVoices = int(std::max(nanFallback(p.unison, 1), 1.0));
        v.wtVoices = std::min(v.wtVoices, 100);
        v.wtPanspread = nanFallback(p.spread, 0.7);
        v.wtWarpMode = p.warpmode;
        const double phaserand = isSet(p.wtphaserand) ? (p.wtphaserand ? 1 : 0) : (nanFallback(p.unison, 1) > 1 ? 1 : 0);
        for (int k = 0; k < v.wtVoices; ++k) v.sawPhase[size_t(k)] = randomUnit(v.seed) * phaserand; // Math.random() * phaserand
        // applyParameterModulators on position and warp
        auto modulate = [&](AudioParam& param, Lfo& lfo, float offset, float amountIn, float a, float d, float su, float r,
                            float rate, float sync, float depthIn, int8_t shape, float skew, float dc) {
            param.reset(float(nanFallback(offset, 0.0)));
            double amount = isSet(amountIn) ? double(amountIn) : ((isSet(a) || isSet(d) || isSet(su) || isSet(r)) ? 0.5 : 0.0);
            const double mn = nanFallback(offset, 0.0), mx = amount + mn;
            if (std::fabs(mx - mn) > 0) {
                static const ADSR wd{0, 0.5, 0, 0.1};
                const ADSR e = getADSRValues(a, d, su, r, &wd);
                getParamADSR(param, e.attack, e.decay, e.sustain, e.release, mn, mx, 0, holdEnd, false);
            }
            const double frequency = isSet(sync) ? cps * sync : (isSet(rate) ? double(rate) : kNaN);
            double depth = isSet(depthIn) ? double(depthIn) : ((!std::isnan(frequency) || shape >= 0 || isSet(skew)) ? 0.5 : 0.0);
            if (depth) {
                lfo.on = true;
                lfo.frequency = std::isnan(frequency) ? 1.0 : frequency;
                lfo.depth = depth;
                lfo.dcoffset = nanFallback(dc, 0.0);
                lfo.skew = nanFallback(skew, 0.5);
                lfo.shape = modulationShape(shape);
                lfo.mn = lfo.dcoffset * depth;
                lfo.mx = lfo.dcoffset * depth + depth;
                lfo.time = absoluteTime; // time ?? begin
                lfo.begin = 0;
                lfo.end = endRel;
            }
        };
        modulate(v.wtPosition, v.wtLfo, p.wt, p.wtenv, p.wtattack, p.wtdecay, p.wtsustain, p.wtrelease, p.wtrate, p.wtsync,
                 p.wtdepth, p.wtshape, p.wtskew, p.wtdc);
        modulate(v.wtWarp, v.warpLfo, p.warp, p.warpenv, p.warpattack, p.warpdecay, p.warpsustain, p.warprelease, p.warprate,
                 p.warpsync, p.warpdepth, p.warpshape, p.warpskew, p.warpdc);
        vibrato();
        fm(p.frequency);
        getParamADSR(v.envGain, env.attack, env.decay, env.sustain, env.release, 0, 0.3, 0, holdEnd, false);
        pitchEnvelope(v.detune, holdEnd);
        v.sourceChannels = 2;
        v.sourceGain = 1.0;
    } else if (p.type == SoundType::Buffer) {
        const SampleData* s = store_ ? store_->get(p.sampleId) : nullptr;
        if (!s || s->frames <= 0) return false;
        v.sample = s;
        v.playbackRate = 1.0;
        v.reverse = v.loop = false;
        v.position = 0;
        v.sampleStart = 0;
        v.sourceGain = 1.0;
        if (p.bufferMode == 1) { // bytebeat: ADSR 0..1, from the first render quantum after the onset
            const ADSR env = getADSRValues(p.attack, p.decay, p.sustain, p.release, &synthDefaults);
            getParamADSR(v.envGain, env.attack, env.decay, env.sustain, env.release, 0, 1, 0, duration, false);
            sourceStop = duration + env.release + 0.01;
            v.sourceChannels = 2;
        } else { // zzfx: the buffer as is (source.start(t)), until it ends
            sourceStop = s->duration();
        }
    } else if (p.type == SoundType::Sample) {
        const double speed = nanFallback(p.speed, 1.0);
        if (speed == 0.0) return false;
        const SampleData* s = store_ ? store_->get(p.sampleId) : nullptr;
        if (!s || s->frames <= 0) {
            missingSamples_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        v.sample = s;
        v.sourceChannels = std::min(2, s->channels);
        const ADSR env = getADSRValues(p.attack, p.decay, p.sustain, p.release, nullptr);
        const double bufferDuration = s->duration();
        double rate = std::fabs(speed) * std::pow(2.0, p.transpose / 12.0);
        if (p.unitCycles) rate *= bufferDuration;
        v.playbackRate = rate;
        v.reverse = speed < 0;
        const double begin = nanFallback(p.begin, 0.0), endFrac = nanFallback(p.end, 1.0);
        v.position = begin * bufferDuration;
        v.loop = nanFallback(p.loop) != 0.0;
        v.loopStart = nanFallback(p.loopBegin, 0.0) * bufferDuration;
        v.loopEnd = nanFallback(p.loopEnd, 1.0) * bufferDuration;
        v.sampleStart = nanFallback(p.nudge, 0.0);
        const double sliceDuration = (endFrac - begin) * (bufferDuration / rate);
        double holdDuration = duration;
        if (!isSet(p.clip) && !isSet(p.loop) && !isSet(p.release)) holdDuration = sliceDuration;
        getParamADSR(v.envGain, env.attack, env.decay, env.sustain, env.release, 0, 1, 0, holdDuration, false);
        sourceStop = holdDuration + env.release + 0.01;
        v.sourceGain = 1.0;
        vibrato();
        pitchEnvelope(v.detune, holdDuration);
    } else if (p.type == SoundType::Soundfont) { // fontloader.mjs registerSoundfonts / getFontBufferSource
        const SampleData* s = store_ ? store_->get(p.sampleId) : nullptr;
        if (!s || s->frames <= 0) {
            missingSamples_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        v.sample = s;
        v.sourceChannels = std::min(2, s->channels);
        const ADSR env = getADSRValues(p.attack, p.decay, p.sustain, p.release, nullptr);
        v.playbackRate = std::pow(2.0, (100.0 * p.sfMidi - s->baseDetune) / 1200.0);
        v.reverse = false;
        v.position = 0;
        v.loop = s->loopEnd > s->loopStart;
        v.loopStart = s->loopStart;
        v.loopEnd = s->loopEnd;
        v.sampleStart = 0;
        // getParamADSR(node.gain, attack, decay, sustain, release, 0, 0.3, time, holdEnd, 'linear')
        getParamADSR(v.envGain, env.attack, env.decay, env.sustain, env.release, 0, 0.3, 0, duration, false);
        sourceStop = duration + env.release + 0.01;
        v.sourceGain = 1.0;
        vibrato();
        pitchEnvelope(v.detune, duration);
    } else if (p.type == SoundType::ScSynth) { // a StrudelDirt instrument (DirtEvent.calcTimeSpan, sendSynth)
        if (!synthServer_) return false;
        SynthArg args[64];
        int n = 0;
        for (int k = 0; k < p.numScArgs; ++k) args[n++] = p.scArgs[k];
        auto set = [&](int8_t control, double value) {
            if (control < 0) return;
            for (int k = 0; k < n; ++k)
                if (args[k].control == uint8_t(control)) {
                    args[k].value = float(value);
                    return;
                }
            if (n < 64) args[n++] = SynthArg{uint8_t(control), float(value)};
        };
        const ScEnvelope env = scEnvelope(p, duration);
        const double sustain = env.sustain, total = env.total;
        const double speed = p.scSpeedValue;
        set(p.scSustain, sustain);
        set(p.scDelta, duration);
        set(p.scCps, cps);
        set(p.scCycle, cycle);
        set(p.scTotal, total);
        set(p.scRelease, p.scReleaseValue);
        set(p.scSpeed, speed);
        set(p.scEndSpeed, isSet(p.scAccelerate) ? speed * (1.0 + p.scAccelerate) : speed);
        int64_t readFrom = 0;
        const int slot = synthServer_->start(p.scDef, args, n, absSample_ + offset, readFrom);
        if (slot < 0) {
            shortage(128);
            return false;
        }
        v.scSlot = slot;
        v.scReadFrom = readFrom;
        v.scTotal = total;
        v.scGate = p.scGate;
        v.scGateClosed = false;
        v.scSilent = 0;
        if (p.scGate >= 0) { // gate 0 at the note's end; the voice lasts until the synth frees itself
            v.scGateAt = absSample_ + offset + int64_t(std::llround(sustain * sampleRate_));
            v.scTotal = sustain + kGateTail;
        }
        v.scAttack = env.attack;
        v.scDecay = env.decay;
        v.scLevel = env.level;
        v.scRelease = env.release;
        v.scCurve = env.curve;
        v.scHoldTime = sustain;
        v.scAmp = env.amp;
        sourceStop = v.scTotal;
        v.sourceGain = 1.0;
    } else {
        return false;
    }
    v.sourceStop = sourceStop;

    // ---- chain: the FX stages, then the value's own effects ----
    releaseStages(v);
    if (const FxChain* fx = FxChainStore::instance().get(p.fxChainId)) {
        for (int k = 0; k < fx->count; ++k) {
            const int slot = acquireStage();
            if (slot < 0) { // no stage left: the voice plays without the rest of its chain
                shortage(2);
                break;
            }
            v.stages[size_t(v.numStages++)] = int8_t(slot);
            setupStage((*stages_)[size_t(slot)], fx->stages[size_t(k)], false, end, endWithRelease, cps, cycle, v.seed + uint32_t(k + 1));
        }
    }
    setupStage(v, p, true, end, endWithRelease, cps, cycle, v.seed);
    if (fxr > release) {
        v.fxRelease = true;
        v.fxReleaseGain.setValueAtTime(1.f, end + release);
        v.fxReleaseGain.linearRampToValueAtTime(0.f, endWithRelease);
    }
    v.postgain = nanFallback(p.postgain, 1.0);
    // getAnalyserById(analyze, 2 ** (fft + 5)) on `post`
    v.analyze = p.analyze > 0 && p.analyze < AnalyserTaps::kMaxIds ? int(p.analyze) : 0;
    v.fftSize = 1 << std::clamp(int(nanFallback(p.fft, 8.0)) + 5, 5, 15);

    // ---- orbit and its effects ----
    const int orbitNumber = int(std::lround(nanFallback(p.orbit, 1.0)));
    Orbit* orbit = getOrbit(orbitNumber, p.channel);
    v.orbit = orbit;
    // duckorbit (controller.duck): the target orbits' output gain dips at t + onset
    for (int k = 0; k < p.numDuck; ++k) {
        Orbit* target = nullptr;
        for (auto& o : *orbits_)
            if (o.used && o.number == int(std::lround(p.duckorbit[k]))) target = &o;
        if (!target) continue; // "duck target orbit does not exist"
        const double onset = p.numDuckOnset ? p.duckonset[k < p.numDuckOnset ? k : 0] : 0.0;
        const double depth = p.numDuckDepth ? p.duckdepth[k < p.numDuckDepth ? k : 0] : 1.0;
        const double now = eventTime - 0.01; // webAudioTimeout(..., t - 0.01)
        AudioParam& g = target->outGain;
        const float curr = g.valueAt(std::max(now, 0.0));
        g.cancelScheduledValues(now);
        g.setValueAtTime(curr, now);
        if (!p.numDuckAttack) continue; // Math.max(undefined, 0.002) is NaN: superdough's ramps throw
        const double attack = std::max(double(p.duckattack[k < p.numDuckAttack ? k : 0]), 0.002);
        const double t0 = std::max(eventTime, now);
        const double ducked = clampd(1 - std::sqrt(depth), 0.01, curr);
        g.exponentialRampToValueAtTime(float(ducked), t0 + onset);
        g.exponentialRampToValueAtTime(1.f, t0 + onset + attack);
    }
    const double delay = nanFallback(p.delay, 0.0);
    const double delayfeedback = nanFallback(p.delayfeedback, 0.5);
    const double delaytime = isSet(p.delaytime) ? double(p.delaytime) : nanFallback(p.delaysync, 3.0 / 16.0) / cps;
    v.delaySend = 0;
    if (delay > 0 && delaytime > 0 && delayfeedback > 0) { // orbitBus.getDelay + sendDelay
        const double fb = clampd(delayfeedback, 0, 0.98);
        Orbit& o = *orbit;
        if (!o.hasDelay) {
            o.hasDelay = true;
            o.delayTime.reset(float(delaytime));
            o.feedback.reset(float(std::min(std::fabs(fb), 0.995)));
            o.lastDelayTime = float(delaytime);
            o.lastFeedback = float(fb);
        }
        if (o.lastDelayTime != float(delaytime)) {
            o.delayTime.setValueAtTime(float(delaytime), eventTime);
            o.lastDelayTime = float(delaytime);
        }
        if (o.lastFeedback != float(fb)) {
            o.feedback.setValueAtTime(float(fb), eventTime);
            o.lastFeedback = float(fb);
        }
        v.delaySend = delay;
    }
    v.roomSend = 0;
    const double room = nanFallback(p.room, 0.0);
    if (room > 0) { // orbitBus.getReverb + sendReverb
        Orbit& o = *orbit;
        // getReverb: regenerate when a passed value differs (hasChanged) or the impulse response
        // changed; generate() then uses this event's values, its own defaults for the others
        auto hasChanged = [](float now, double before) { return isSet(now) && double(now) != before; };
        auto hasChangedRaw = [](float now, float before) { return isSet(now) && !(now == before); };
        const bool changed = !o.hasReverb || hasChanged(p.roomsize, o.roomsize) || hasChanged(p.roomfade, o.roomfade) ||
                             hasChanged(p.roomlp, o.roomlp) || hasChanged(p.roomdim, o.roomdim) ||
                             hasChangedRaw(p.irspeed, o.irspeed) || hasChangedRaw(p.irbegin, o.irbegin) || p.irSampleId != o.irId;
        o.hasReverb = true;
        if (changed) {
            o.roomsize = nanFallback(p.roomsize, 2.0);
            o.roomfade = nanFallback(p.roomfade, 0.1);
            o.roomlp = nanFallback(p.roomlp, 15000.0);
            o.roomdim = nanFallback(p.roomdim, 1000.0);
            o.irspeed = p.irspeed;
            o.irbegin = p.irbegin;
            o.irId = p.irSampleId;
            o.buildParams[0] = o.roomsize;
            o.buildParams[1] = o.roomfade;
            o.buildParams[2] = o.roomlp;
            o.buildParams[3] = o.roomdim;
            o.buildParams[4] = o.irspeed;
            o.buildParams[5] = o.irbegin;
            o.buildIr = o.irId;
            if (synchronousReverb_) {
                if (auto ir = buildImpulse(o.buildParams, o.buildIr, o.buildSeed)) o.reverb = std::make_unique<Convolver>(*ir);
                else o.irId = 0; // not loaded yet: the next event tries again
            } else {
                o.buildRequested = true;
            }
        }
        v.roomSend = room;
    }
    if (orbitFx_) { // GlobalDirtEffect.set: dirt_leslie, dirt_shimmer (changed controls only)
        Orbit& o = *orbit;
        const int64_t at = absSample_ + offset;
        const int index = o.number % 16;
        if (isSet(p.leslie) || isSet(p.lrate) || isSet(p.lsize)) {
            const char* names[3];
            float values[3];
            int n = 0;
            const float now[3] = {p.leslie, p.lrate, p.lsize};
            static const char* const keys[3] = {"leslie", "lrate", "lsize"};
            for (int k = 0; k < 3; ++k)
                if (isSet(now[k]) && !(now[k] == o.leslieSent[k])) {
                    names[n] = keys[k];
                    values[n++] = now[k];
                    o.leslieSent[k] = now[k];
                }
            if (n > 0 || !o.leslieOn) orbitFx_->set(index, OrbitFxServer::Leslie, names, values, n, at);
            o.leslieOn = true;
            if (isSet(p.leslie)) o.leslieAmount = p.leslie; // the synth's default is 0.5
        }
        if (isSet(p.shimmer) || o.shimmerOn) {
            // StrudelDirt: delaytime in cycles (lock = 1), defaults 0.1875 / 0.15; it comes back kLatency late
            const double delaytime = std::max(0.0, nanFallback(p.delaytime, 0.1875) - OrbitFxServer::kLatency * cps / sr);
            const float now[4] = {float(delaytime), float(nanFallback(p.delayfeedback, 0.15)),
                                  isSet(p.shimmer) ? float(p.shimmer) : o.shimmerSent[2], float(cps)};
            static const char* const keys[4] = {"delaytime", "delayfeedback", "shimmer", "cps"};
            const char* names[4];
            float values[4];
            int n = 0;
            for (int k = 0; k < 4; ++k)
                if (isSet(now[k]) && !(now[k] == o.shimmerSent[k])) {
                    names[n] = keys[k];
                    values[n++] = now[k];
                    o.shimmerSent[k] = now[k];
                }
            if (n > 0 || !o.shimmerOn) orbitFx_->set(index, OrbitFxServer::Shimmer, names, values, n, at);
            o.shimmerOn = true;
        }
    }
    if (isSet(p.djf)) {
        Orbit& o = *orbit;
        o.hasDjf = true;
        o.djfValue.setValueAtTime(p.djf, eventTime);
    }
    v.dryOn = isSet(p.dry);
    v.dryGain = nanFallback(p.dry, 1.0);

    // ---- buses ----
    v.busOut = isSet(p.bus) ? busSlot(p.bus) : -1;
    v.busGain = nanFallback(p.busgain, 1.0);
    v.busIn = -1;
    if (p.type == SoundType::Synth && p.wave == SynthWave::Bus) v.busIn = busSlot(nanFallback(p.n, 0.0));
    v.readsBus = v.busIn >= 0;

    // ---- modulators (connectLFO / connectEnvelope / connectBusModulator): per FX stage then main,
    // every lfo, env, then bmod ----
    v.numMods = 0;
    v.orbitMods = false;
    v.sampleDetuneMod = 0;
    v.pwLfo.capture();
    v.wtLfo.capture();
    v.warpLfo.capture();
    {
        int slotOf[SoundParams::kMaxModulators];
        for (int k = 0; k < p.numModulators; ++k) {
            slotOf[k] = -1;
            const Modulator& md = p.modulators[k];
            // the stage holding the target node (nodeTracker[fxi])
            FxStage* st = &v;
            if (md.fxi >= 0) st = md.fxi < v.numStages ? &(*stages_)[size_t(v.stages[size_t(md.fxi)])] : nullptr;
            if (!st) continue;
            const bool isMain = st == &v;
            auto filterOf = [&](FilterType t) {
                for (int f = 0; f < st->numFilters; ++f)
                    if (st->filters[size_t(f)].type == t) return f;
                return -1;
            };
            auto filterGroup = [](int ti) { return ti == 0 ? FilterType::Lowpass : ti == 1 ? FilterType::Highpass : FilterType::Bandpass; };
            // the target's current value, and whether its node exists (else superdough logs and skips)
            double cur = 0;
            bool exists = true, isFrequency = false;
            const bool synth = v.type == SoundType::Synth;
            switch (md.target) {
                // ---- source nodes (main only) ----
                case ModTarget::SourceFrequency:
                    isFrequency = true;
                    exists = isMain && v.type != SoundType::Noise && v.type != SoundType::Buffer && !(synth && v.wave == SynthWave::Bus);
                    cur = v.type == SoundType::Sample || v.type == SoundType::Soundfont ? 0.0 : double(p.frequency); // samples: detune
                    break;
                case ModTarget::Post: exists = isMain; cur = v.postgain; break;
                case ModTarget::VibRate: exists = isMain && v.vib; isFrequency = true; cur = v.vibFreq; break;
                case ModTarget::VibDepth: exists = isMain && v.vib; cur = v.vibDepthCents; break;
                case ModTarget::PulseWidth: exists = isMain && synth && v.wave == SynthWave::Pulse; cur = nanFallback(p.pw, 0.5); break;
                case ModTarget::WtPosition: exists = isMain && v.type == SoundType::Wavetable; cur = nanFallback(p.wt, 0.0); break;
                case ModTarget::WtWarp: exists = isMain && v.type == SoundType::Wavetable; cur = nanFallback(p.warp, 0.0); break;
                case ModTarget::FreqSpread: case ModTarget::PanSpread: {
                    const bool saw = synth && v.wave == SynthWave::Supersaw, wt = v.type == SoundType::Wavetable;
                    exists = isMain && (saw || wt);
                    if (md.target == ModTarget::FreqSpread) cur = saw ? v.sawFreqspread : v.wtFreqspread;
                    else cur = saw ? (v.sawPanspread) : v.wtPanspread;
                    break;
                }
                case ModTarget::WtLfoFreq: case ModTarget::WtLfoDepth: case ModTarget::WtLfoSkew:
                case ModTarget::WarpLfoFreq: case ModTarget::WarpLfoDepth: case ModTarget::WarpLfoSkew: {
                    const bool warp = md.target >= ModTarget::WarpLfoFreq;
                    const Lfo& l = warp ? v.warpLfo : v.wtLfo;
                    exists = isMain && v.type == SoundType::Wavetable && l.on;
                    const int which = int(md.target) - int(warp ? ModTarget::WarpLfoFreq : ModTarget::WtLfoFreq);
                    isFrequency = which == 0;
                    cur = which == 0 ? l.frequency : which == 1 ? l.depth : l.skew;
                    break;
                }
                case ModTarget::PwLfoFreq: case ModTarget::PwLfoDepth:
                    exists = isMain && synth && v.wave == SynthWave::Pulse && v.pwLfo.on;
                    isFrequency = md.target == ModTarget::PwLfoFreq;
                    cur = isFrequency ? v.pwLfo.frequency : v.pwLfo.depth;
                    break;
                case ModTarget::FmIndex: case ModTarget::FmIndex2: case ModTarget::FmIndex3: case ModTarget::FmIndex4:
                case ModTarget::FmIndex5: case ModTarget::FmIndex6: case ModTarget::FmIndex7: case ModTarget::FmIndex8: {
                    const int op = int(md.target) - int(ModTarget::FmIndex) + 1;
                    exists = isMain && v.fm && v.fmOps[size_t(op)].used;
                    const int e = v.fmGainEdge[size_t(op)];
                    cur = e >= 0 ? v.fmEdges[size_t(e)].amt : 0.0;
                    break;
                }
                case ModTarget::FmFreq: case ModTarget::FmFreq2: case ModTarget::FmFreq3: case ModTarget::FmFreq4:
                case ModTarget::FmFreq5: case ModTarget::FmFreq6: case ModTarget::FmFreq7: case ModTarget::FmFreq8: {
                    const int op = int(md.target) - int(ModTarget::FmFreq) + 1;
                    exists = isMain && v.fm && v.fmOps[size_t(op)].used && !v.fmOps[size_t(op)].noise;
                    isFrequency = true;
                    cur = v.fmOps[size_t(op)].freq;
                    break;
                }
                case ModTarget::Djf: exists = isMain && isSet(p.djf); cur = p.djf; break;
                // ---- the stage's nodes ----
                case ModTarget::Gain: cur = st->gain; break;
                case ModTarget::Pan: exists = st->pan; cur = st->panValue; break;
                case ModTarget::LpfFrequency: case ModTarget::LpfQ: case ModTarget::HpfFrequency: case ModTarget::HpfQ:
                case ModTarget::BpfFrequency: case ModTarget::BpfQ: {
                    const int ti = int(md.target) - int(ModTarget::LpfFrequency);
                    const int f = filterOf(filterGroup(ti / 2));
                    exists = f >= 0;
                    if (exists) {
                        isFrequency = (ti & 1) == 0;
                        cur = isFrequency ? st->filterBaseFreq[size_t(f)] : st->filterQ[size_t(f)];
                    }
                    break;
                }
                case ModTarget::LpfLfoDepth: case ModTarget::LpfLfoDc: case ModTarget::LpfLfoSkew:
                case ModTarget::HpfLfoDepth: case ModTarget::HpfLfoDc: case ModTarget::HpfLfoSkew:
                case ModTarget::BpfLfoDepth: case ModTarget::BpfLfoDc: case ModTarget::BpfLfoSkew: {
                    const int ti = int(md.target) - int(ModTarget::LpfLfoDepth);
                    const int f = filterOf(filterGroup(ti / 3));
                    exists = f >= 0 && st->filterLfo[size_t(f)].on;
                    if (exists) {
                        const Lfo& l = st->filterLfo[size_t(f)];
                        cur = ti % 3 == 0 ? l.depth : ti % 3 == 1 ? l.dcoffset : l.skew;
                    }
                    break;
                }
                case ModTarget::TremoloRate: exists = st->tremoloOn; isFrequency = true; cur = st->tremolo.frequency; break;
                case ModTarget::TremoloGain: exists = st->tremoloOn; cur = st->tremoloBase; break;
                case ModTarget::TremoloSkew: exists = st->tremoloOn; cur = st->tremolo.skew; break;
                case ModTarget::PhaserFrequency: exists = st->phaserOn; isFrequency = true; cur = st->phaserFreq; break;
                case ModTarget::PhaserQ: exists = st->phaserOn; cur = st->phaserQ; break;
                case ModTarget::PhaserLfoFreq: exists = st->phaserOn; isFrequency = true; cur = st->phaserLfo.frequency; break;
                case ModTarget::PhaserLfoDepth: exists = st->phaserOn; cur = st->phaserLfo.depth; break;
                case ModTarget::VowelFreq:
                    exists = st->vowel >= 0;
                    isFrequency = true;
                    if (exists) cur = kVowels[st->vowel].freqs[0];
                    break;
                case ModTarget::Coarse: exists = st->coarse > 0; cur = st->coarseRaw; break;
                case ModTarget::Crush: exists = st->crush > 0; cur = st->crushRaw; break;
                case ModTarget::Shape: exists = st->shapeOn; cur = st->shapeRaw; break;
                case ModTarget::ShapeVol: exists = st->shapeOn; cur = st->shapeVolRaw; break;
                case ModTarget::Distort: exists = st->distortOn; cur = st->distortRaw; break;
                case ModTarget::DistortVol: exists = st->distortOn; cur = st->distortVolRaw; break;
                case ModTarget::CompThreshold: case ModTarget::CompRatio: case ModTarget::CompKnee:
                case ModTarget::CompAttack: case ModTarget::CompRelease:
                    exists = st->compressorOn;
                    cur = st->compBase[int(md.target) - int(ModTarget::CompThreshold)];
                    break;
                case ModTarget::Stretch: exists = st->vocoder >= 0; cur = st->stretch; break;
                // ---- delay and reverb: the orbit's (main) or the stage's own ----
                case ModTarget::DelaySend: exists = isMain ? v.delaySend > 0 : st->stageDelayOn; cur = isMain ? v.delaySend : st->stageDelayWet; break;
                case ModTarget::RoomSend: exists = isMain ? v.roomSend > 0 : st->stageRoomOn; cur = isMain ? v.roomSend : st->stageRoom; break;
                case ModTarget::DelayTime: case ModTarget::DelayFeedback:
                    if (isMain) {
                        exists = v.delaySend > 0;
                        cur = md.target == ModTarget::DelayTime ? v.orbit->lastDelayTime : std::min(std::fabs(double(v.orbit->lastFeedback)), 0.995);
                    } else {
                        exists = st->stageDelayOn;
                        cur = md.target == ModTarget::DelayTime ? st->stageDelayTime : st->stageDelayFb;
                    }
                    break;
                // ---- other modulators ----
                case ModTarget::ModFrequency: case ModTarget::ModDepth: case ModTarget::ModDcoffset: case ModTarget::ModSkew:
                case ModTarget::ModCurve: case ModTarget::ModAttack: case ModTarget::ModDecay: case ModTarget::ModSustain:
                case ModTarget::ModRelease: {
                    const int slot = md.targetIndex >= 0 && md.targetIndex < k ? slotOf[md.targetIndex] : -1;
                    exists = slot >= 0;
                    if (!exists) break;
                    const ModState& tm = v.mods[size_t(slot)];
                    switch (md.target) {
                        case ModTarget::ModFrequency: exists = tm.kind == 1; isFrequency = true; cur = tm.baseFrequency; break;
                        case ModTarget::ModDepth: cur = tm.kind == 1 ? tm.baseDepth : tm.env.depth; break;
                        case ModTarget::ModDcoffset: exists = tm.kind == 1; cur = tm.baseDcoffset; break;
                        case ModTarget::ModSkew: exists = tm.kind == 1; cur = tm.baseSkew; break;
                        case ModTarget::ModCurve: exists = tm.kind == 1; cur = tm.baseCurve; break;
                        case ModTarget::ModAttack: exists = tm.kind == 2; cur = tm.env.attack; break;
                        case ModTarget::ModDecay: exists = tm.kind == 2; cur = tm.env.decay; break;
                        case ModTarget::ModSustain: exists = tm.kind == 2; cur = tm.env.sustain; break;
                        default: exists = tm.kind == 2; cur = tm.env.release; break;
                    }
                    break;
                }
                default: exists = false; break;
            }
            if (!exists) continue;
            if (cur == 0) cur = 1;
            double mn = kNaN, mx = kNaN;
            if (isFrequency && cur >= 30) {
                mn = 20 - cur;
                mx = 24000 - cur;
            }
            const double depth = isSet(md.depthabs) ? double(md.depthabs) : nanFallback(md.depth, 1.0) * cur;
            ModState& m = v.mods[size_t(v.numMods)];
            m = ModState{};
            m.kind = md.kind;
            m.target = md.target;
            m.stage = isMain ? -1 : md.fxi;
            if (md.target >= ModTarget::ModFrequency) {
                m.targetSlot = slotOf[md.targetIndex];
                if (md.target >= ModTarget::ModAttack) v.mods[size_t(m.targetSlot)].env.paramMod = true;
            } else {
                st->modulated = true;
                if (isMain && (md.target == ModTarget::DelayTime || md.target == ModTarget::DelayFeedback || md.target == ModTarget::Djf))
                    v.orbitMods = true;
            }
            if (md.kind == 1) { // getLfo
                Lfo& l = m.lfo;
                l.on = true;
                l.frequency = m.baseFrequency = isSet(md.sync) ? md.sync * cps : nanFallback(md.rate, 1.0);
                l.time = nanFallback(md.retrig, 0.0) > 0.5 ? 0.0 : (cps > 0 ? cycle / cps : 0.0);
                l.depth = m.baseDepth = depth;
                l.dcoffset = m.baseDcoffset = nanFallback(md.dcoffset, -0.5);
                l.skew = m.baseSkew = nanFallback(md.skew, 0.5);
                l.curve = m.baseCurve = nanFallback(md.curve, 1.0);
                l.shape = modulationShape(md.shape);
                l.phaseoffset = 0;
                l.mn = std::isnan(mn) ? l.dcoffset * depth : mn;
                l.mx = std::isnan(mx) ? l.dcoffset * depth + depth : mx;
                l.begin = 0;
                l.end = endWithRelease;
            } else if (md.kind == 4) { // a live control: starts where the control is now (no glide from c0)
                m.liveKind = md.liveKind;
                m.liveA = md.liveA;
                m.liveB = md.liveB;
                m.liveC0 = md.liveC0;
                m.liveSlope = md.liveSlope;
                m.liveTarget = m.liveValue = liveControl_ ? liveControl_(m.liveKind, m.liveA, m.liveB) : double(md.liveC0);
                m.liveMin = mn;
                m.liveMax = mx;
            } else if (md.kind == 3) { // connectBusModulator: (dc + bus) * depth / 0.3, clamped by a waveshaper
                m.bus = busSlot(md.bus);
                if (m.bus < 0) continue;
                m.dc = nanFallback(md.dcoffset, 0.0);
                m.bmodDepth = depth / 0.3;
                m.bmodMin = mn;
                m.bmodMax = mx;
                v.readsBus = true;
            } else { // getEnvelope -> envelope-processor (parameter ranges applied)
                EnvMod& e = m.env;
                if (isSet(md.attack)) e.attack = std::max(0.0, double(md.attack));
                if (isSet(md.decay)) e.decay = std::max(0.0, double(md.decay));
                if (isSet(md.sustain)) e.sustain = clampd(md.sustain, 0, 1);
                if (isSet(md.release)) e.release = std::max(0.0, double(md.release));
                e.acurve = clampd(nanFallback(md.acurve, 0.0), -1, 1);
                e.dcurve = clampd(nanFallback(md.dcurve, 0.0), -1, 1);
                e.rcurve = clampd(nanFallback(md.rcurve, 0.0), -1, 1);
                e.depth = depth;
                if (!std::isnan(mn)) {
                    e.mn = mn;
                    e.mx = mx;
                }
                e.end = endWithRelease;
            }
            slotOf[k] = v.numMods++;
        }
    }

    v.fxSlot = -1;
    if (p.numFxModules > 0 && fxServer_) { // StrudelDirt's effect modules at the end of the chain
        const int slot = fxServer_->start(p.fxModules, p.numFxModules, absSample_ + offset, duration);
        if (slot < 0) {
            shortage(128);
        } else {
            v.fxSlot = slot;
            FxDrain& d = (*fxDrains_)[size_t(slot)];
            d = FxDrain{};
            d.active = true;
            d.start = absSample_ + offset;
            d.orbit = v.orbit;
            d.dry = v.dryOn ? v.dryGain : 1.0;
            d.delay = v.delaySend;
            d.room = v.roomSend;
            d.busOut = v.busOut;
            d.busGain = v.busGain;
        }
    }
    v.endTime = std::max(sourceStop, endWithRelease) + (v.compressorOn ? 0.006 : 0.0); // compressor pre-delay
    v.id = ++voiceCounter_;
    v.active = true;
    return true;
}

void Engine::processStage(FxStage& v, float& l, float& r, int& ch, int i, int quantumIndex, bool quantumStart,
                          int64_t index, int64_t absIndex, double t, const double* ms, bool main, double sourceStop)
{
    const double sr = sampleRate_, dt = 1.0 / sr;
    auto mod = [&](ModTarget target) { return ms[size_t(target)]; };
    if (quantumStart) {
        const double qt = double(index - quantumIndex) * dt;
        if (v.modulated) { // worklet parameters read once per quantum ([0]), the compressor's k-rate ones
            for (int f = 0; f < v.numFilters; ++f) {
                const FilterType ft = v.filters[size_t(f)].type;
                const size_t g = size_t(ModTarget::LpfLfoDepth) + size_t(ft == FilterType::Lowpass ? 0 : ft == FilterType::Highpass ? 3 : 6);
                v.filterLfo[size_t(f)].apply(0, ms[g], ms[g + 1], ms[g + 2]);
            }
            v.tremolo.apply(mod(ModTarget::TremoloRate), 0, 0, mod(ModTarget::TremoloSkew));
            v.phaserLfo.apply(mod(ModTarget::PhaserLfoFreq), mod(ModTarget::PhaserLfoDepth), 0, 0);
            if (v.coarse > 0) v.coarse = std::max(1.0, v.coarseRaw + mod(ModTarget::Coarse));
            if (v.crush > 0) v.crush = std::max(1.0, v.crushRaw + mod(ModTarget::Crush));
            if (v.shapeOn) {
                double sh = v.shapeRaw + mod(ModTarget::Shape);
                sh = sh < 1 ? sh : 1.0 - 4e-10;
                v.shapeAmount = (2.0 * sh) / (1.0 - sh);
                v.shapeGain = std::max(0.001, std::min(1.0, v.shapeVolRaw + mod(ModTarget::ShapeVol)));
            }
            if (v.compressorOn)
                v.compressor.setParams(float(v.compBase[0] + mod(ModTarget::CompThreshold)), float(v.compBase[2] + mod(ModTarget::CompKnee)),
                                       float(v.compBase[1] + mod(ModTarget::CompRatio)), float(v.compBase[3] + mod(ModTarget::CompAttack)),
                                       float(v.compBase[4] + mod(ModTarget::CompRelease)));
        }
        for (int f = 0; f < v.numFilters; ++f) v.filterLfo[size_t(f)].quantum(qt);
        v.tremolo.quantum(qt);
        v.phaserLfo.quantum(qt);
        for (int f = 0; f < v.numFilters; ++f) {  // ladder-processor reads frequency[0]
            if (!v.ladder[size_t(f)]) continue;
            auto& L = v.ladders[size_t(f)];
            const int ti = v.filters[size_t(f)].type == FilterType::Lowpass ? 0 : v.filters[size_t(f)].type == FilterType::Highpass ? 2 : 4;
            const double cutoff = (v.filterFreq[size_t(f)].valueAt(t) + ms[size_t(ModTarget::LpfFrequency) + size_t(ti)]) * 2 * kPi * dt;
            L.cutoff = cutoff > 1 ? 1 : cutoff;
            L.drive = clampd(std::exp(v.filterDrive[size_t(f)]), 0.1, 2000);
            L.k = std::min(8.0, (v.filterQ[size_t(f)] + ms[size_t(ModTarget::LpfQ) + size_t(ti)]) * 0.13);
            L.makeup = (1 / L.drive) * std::min(1.75, 1 + L.k);
        }
    }
    if (v.kabel >= 0) { // the worklet's output replaces the chain (input: its first channel)
        kabel::KabelState& ks = (*kabel_)[size_t(v.kabel)];
        if (quantumStart) {
            const double qt = double(index - quantumIndex) * dt;
            if (qt >= v.kabelEnd) v.kabelEnded = true;
            else if (qt >= 0) v.kabelRunning = true;
            if (v.kabelRunning && !v.kabelGateEnded && qt > v.kabelGateEnd) {
                ks.setGate(0);
                v.kabelGateEnded = true;
            }
        }
        if (v.kabelRunning && !v.kabelEnded) {
            ks.sample(l, l, r);
        } else {
            l = r = 0.f;
        }
        ch = 2;
    }
    if (v.vocoder >= 0) { // phase-vocoder-processor, one quantum later (see PhaseVocoder.h)
        PhaseVocoder& pv = (*vocoders_)[size_t(v.vocoder)];
        if (quantumStart) v.vocoderChannels = ch;
        const int nc = v.vocoderChannels;
        pv.push(0, quantumIndex, l);
        if (nc == 2) pv.push(1, quantumIndex, r);
        if (v.vocoderEnded) {
            l = r = 0.f;
        } else {
            l = pv.output(0, quantumIndex);
            r = nc == 2 ? pv.output(1, quantumIndex) : l;
        }
        ch = nc;
        if (quantumIndex == kQuantum - 1) {
            const double qt = double(index - quantumIndex) * dt;
            // no input any more once the source has stopped: the worklet returns false
            if (qt >= sourceStop) v.vocoderEnded = true;
            else pv.endQuantum(nc, v.stretch + mod(ModTarget::Stretch), double(absIndex - quantumIndex + kQuantum));
        }
    }
    if (v.transientOn) { // transient-processor: silent until the first quantum after the onset
        const double qt = double(index - quantumIndex) * dt;
        if (!(qt > 0) || qt >= v.trEnd) {
            l = r = 0.f;
        } else {
            float* io[2] = {&l, &r};
            for (int c = 0; c < ch; ++c) {
                const double sample = *io[c], x = std::fabs(sample);
                v.trAttackEnv[c] += (x - v.trAttackEnv[c]) * v.trAttackCoeff;
                v.trSustainEnv[c] += (x - v.trSustainEnv[c]) * v.trSustainCoeff;
                const double peak = clampd((v.trScaling * (v.trAttackEnv[c] - v.trSustainEnv[c])) / (v.trSustainEnv[c] + 1e-6), -1.5, 1.5);
                const double attGain = std::pow(10.0, v.trAttackAmt * (peak > 0 ? peak : 0) * 18 / 20);
                const double susGain = std::pow(10.0, v.trSustainAmt * (peak < 0 ? -peak : 0) * 36 / 20);
                const double g = clampd(attGain * susGain, 0, 8);
                v.trAvgGain += (g - v.trAvgGain) * v.trGainCoeff;
                const double makeup = v.trAvgGain > 1e-3 ? 1 / v.trAvgGain : 1;
                double y = sample + (sample * g * makeup - sample) * v.trMix;
                y /= 1 + std::fabs(y);
                *io[c] = float(y);
            }
            if (ch == 1) r = l;
        }
    }
    // gain stage
    const double gainNow = v.gain + mod(ModTarget::Gain);
    l *= float(gainNow);
    r *= float(gainNow);
    // filters
    for (int f = 0; f < v.numFilters; ++f) {
        const float lfo = float(v.filterLfo[size_t(f)].next(sr));
        if (v.ladder[size_t(f)]) {
            auto& L = v.ladders[size_t(f)];
            float* io[2] = {&l, &r};
            for (int c = 0; c < ch; ++c) {
                const double out = L.p3[c] * 0.360891 + L.p32[c] * 0.41729 + L.p33[c] * 0.177896 + L.p34[c] * 0.0439725;
                L.p34[c] = L.p33[c];
                L.p33[c] = L.p32[c];
                L.p32[c] = L.p3[c];
                L.p0[c] += (fastTanh(*io[c] * L.drive - L.k * out) - fastTanh(L.p0[c])) * L.cutoff;
                L.p1[c] += (fastTanh(L.p0[c]) - fastTanh(L.p1[c])) * L.cutoff;
                L.p2[c] += (fastTanh(L.p1[c]) - fastTanh(L.p2[c])) * L.cutoff;
                L.p3[c] += (fastTanh(L.p2[c]) - fastTanh(L.p3[c])) * L.cutoff;
                *io[c] = float(out * L.makeup);
            }
            if (ch == 1) r = l;
            continue;
        }
        const int ti = v.filters[size_t(f)].type == FilterType::Lowpass ? 0 : v.filters[size_t(f)].type == FilterType::Highpass ? 2 : 4;
        const float freq = v.filterFreq[size_t(f)].valueAt(t) + lfo + float(ms[size_t(ModTarget::LpfFrequency) + size_t(ti)]);
        const double q = v.filterQ[size_t(f)] + ms[size_t(ModTarget::LpfQ) + size_t(ti)];
        // coefficients follow automations and LFOs every 8 samples (Chromium: every sample)
        // (constant: setParams only recomputes when the values change)
        const bool varying = v.filterFreq[size_t(f)].automated() || v.filterLfo[size_t(f)].on || v.modulated;
        if ((i & 7) == 0 || !varying) v.filters[size_t(f)].setParams(sr, freq, q, 0.0);
        l = v.filters[size_t(f)].process(0, l);
        r = ch == 2 ? v.filters[size_t(f)].process(1, r) : l;
    }
    if (v.vowel >= 0) { // 5 band-passes in parallel, makeup gain 8
        const Formant& fm = kVowels[v.vowel];
        if (v.modulated && (i & 7) == 0) { // a modulator on the five frequencies
            const double df = mod(ModTarget::VowelFreq);
            for (int k = 0; k < 5; ++k) v.vowelFilters[size_t(k)].setParams(sr, fm.freqs[k] + df, fm.qs[k], 0);
        }
        double sl = 0, sr2 = 0;
        for (int k = 0; k < 5; ++k) {
            sl += v.vowelFilters[size_t(k)].process(0, l) * fm.gains[k];
            if (ch == 2) sr2 += v.vowelFilters[size_t(k)].process(1, r) * fm.gains[k];
        }
        l = float(sl * 8);
        r = ch == 2 ? float(sr2 * 8) : l;
    }
    if (v.coarse > 0) { // coarse-processor: n % coarse < 1, n within the render quantum
        if (std::fmod(double(quantumIndex), v.coarse) < 1) {
            v.coarseHeld[0] = l;
            v.coarseHeld[1] = r;
        }
        l = v.coarseHeld[0];
        r = v.coarseHeld[1];
    }
    if (v.crush > 0) { // Math.round: halves round up
        const double x = std::pow(2.0, v.crush - 1);
        l = float(std::floor(double(l) * x + 0.5) / x);
        r = float(std::floor(double(r) * x + 0.5) / x);
    }
    if (v.shapeOn) {
        const double s = v.shapeAmount;
        l = float(((1 + s) * l) / (1 + s * std::fabs(l)) * v.shapeGain);
        r = float(((1 + s) * r) / (1 + s * std::fabs(r)) * v.shapeGain);
    }
    if (v.distortOn) {
        if (v.modulated) { // distort-processor reads both parameters per sample
            v.distortK = std::expm1(v.distortRaw + mod(ModTarget::Distort));
            v.distortGain = clampd(v.distortVolRaw + mod(ModTarget::DistortVol), 0.001, 1.0);
        }
        l = float(v.distortGain * distortion(v.distortType, l, v.distortK));
        r = float(v.distortGain * distortion(v.distortType, r, v.distortK));
    }
    if (v.tremoloOn) {
        const float g = float(v.tremoloBase + mod(ModTarget::TremoloGain) + v.tremolo.next(sr));
        l *= g;
        r *= g;
    }
    if (v.compressorOn) {
        v.compressor.process(l, r, ch, (absIndex & 31) == 0);
        if (ch == 1) r = l;
    }
    if (v.pan) {
        float pl, pr;
        stereoPan(float(clampd(v.panValue + mod(ModTarget::Pan), -1, 1)), ch, l, r, pl, pr);
        l = pl;
        r = pr;
        ch = 2;
    }
    if (v.phaserOn) {
        const double cents = v.phaserLfo.next(sr);
        if ((i & 7) == 0)
            v.phaser.setParams(sr, (v.phaserFreq + mod(ModTarget::PhaserFrequency)) * (cents == 0.0 ? 1.0 : std::pow(2.0, cents / 1200.0)),
                               v.phaserQ + mod(ModTarget::PhaserQ), 0.0);
        l = v.phaser.process(0, l);
        r = ch == 2 ? v.phaser.process(1, r) : l;
    }
    if (main) return;
    if (v.stageDelayOn) { // dry * (dry ?? 1) + FeedbackDelay * delay
        auto& lines = stageDelays_[size_t(v.stageDelay)];
        const int dlen = int(lines[0].size());
        const double d = std::max(double(kQuantum), clampd(v.stageDelayTime + mod(ModTarget::DelayTime), 0, 1) * sr);
        const double fb = v.stageDelayFb + mod(ModTarget::DelayFeedback), wet = v.stageDelayWet + mod(ModTarget::DelaySend);
        double rp = v.stageDelayWrite - d;
        while (rp < 0) rp += dlen;
        const int a = int(rp), b = (a + 1) % dlen;
        const float fr = float(rp - a);
        float* io[2] = {&l, &r};
        for (int c = 0; c < 2; ++c) {
            auto& line = lines[size_t(c)];
            const float y = line[size_t(a)] + (line[size_t(b)] - line[size_t(a)]) * fr;
            const float x = *io[c]; // mono: r == l
            line[size_t(v.stageDelayWrite)] = x + float(fb) * y;
            *io[c] = float(x * v.stageDelayDry + y * wet);
        }
        v.stageDelayWrite = (v.stageDelayWrite + 1) % dlen;
        ch = 2;
    }
    if (v.stageRoomOn) { // dry * (dry ?? 1) + reverb * room (silent until the response is built)
        float wl = 0.f, wr = 0.f;
        if (v.stageReverb >= 0) {
            StageReverb& sr2 = (*stageReverbs_)[size_t(v.stageReverb)];
            if (Convolver* c = sr2.ready.load(std::memory_order_acquire)) {
                const float inL = l, inR = ch == 2 ? r : l;
                c->process(&inL, &inR, &wl, &wr, 1);
            }
        }
        const float inR = ch == 2 ? r : l;
        const double wet = v.stageRoom + mod(ModTarget::RoomSend);
        l = float(l * v.stageRoomDry + wl * wet);
        r = float(inR * v.stageRoomDry + wr * wet);
        ch = 2;
    }
}

void Engine::render(float* const* const* buses, int numBuses, int numSamples)
{
    numSamples = std::min(numSamples, kMaxBlock);
    const double sr = sampleRate_, dt = 1.0 / sr;
    const double liveSmoothing = 1.0 - std::exp(-1.0 / (0.01 * sr)); // live controls: ~10 ms
    const PeriodicWaves& waves = *waves_;
    for (auto& o : *orbits_) {
        if (!o.used) continue;
        if (o.modsUsed) {
            for (auto& m : o.mods) std::fill_n(m.begin(), std::min<size_t>(m.size(), size_t(kMaxBlock)), 0.f);
            o.modsUsed = false;
        }
        for (int c = 0; c < 2; ++c) {
            std::fill_n(o.dry[size_t(c)].begin(), numSamples, 0.f);
            std::fill_n(o.delaySend[size_t(c)].begin(), numSamples, 0.f);
            std::fill_n(o.roomSend[size_t(c)].begin(), numSamples, 0.f);
        }
    }
    int count = 0;
    for (auto& b : *audioBuses_)
        if (b.used)
            for (auto& c : b.buf) std::fill_n(c.begin(), numSamples, 0.f);
    if (synthServer_) { // the instruments' World, far enough for every voice reading it
        int64_t need = absSample_ + numSamples;
        for (const auto& v : *voices_)
            if (v.active && v.scSlot >= 0) need = std::max(need, v.scReadFrom + v.index + int64_t(numSamples));
        for (auto& v : *voices_) // gates closing before `need`: queued before the World renders there
            if (v.active && v.scSlot >= 0 && v.scGate >= 0 && !v.scGateClosed && v.scGateAt < need) {
                synthServer_->set(v.scSlot, v.scGate, 0.f, v.scGateAt);
                v.scGateClosed = true;
            }
        synthServer_->renderTo(need);
    }
    auto renderVoice = [&](Voice& v) {
        if (!v.active) return;
        Orbit& o = *v.orbit;
        int i = std::max(0, v.startDelay);
        v.startDelay = 0;
        for (; i < numSamples; ++i) {
            const double t = v.t;
            if (t >= v.endTime) {
                v.active = false;
                releaseStages(v);
                releaseSynth(v);
                break;
            }
            // render quanta on the engine clock (where AudioWorklets' process() calls fall)
            const int quantumIndex = int((v.absStart + v.index) & (kQuantum - 1));
            const bool quantumStart = quantumIndex == 0 || v.index == 0;
            auto& ms = v.modSum;
            if (v.numMods > 0) {
                ms.fill(0.0);
                for (int k = 0; k < v.numStages; ++k) {
                    FxStage& st = (*stages_)[size_t(v.stages[size_t(k)])];
                    if (st.modulated) st.modSum.fill(0.0);
                }
                for (int k = 0; k < v.numMods; ++k) std::fill(std::begin(v.mods[size_t(k)].in), std::end(v.mods[size_t(k)].in), 0.0);
                // modulators only target earlier ones: compute the later ones first
                for (int k = v.numMods - 1; k >= 0; --k) {
                    ModState& m = v.mods[size_t(k)];
                    double out;
                    if (m.kind == 1) {
                        if (quantumStart) { // lfo-processor reads its parameters at [0]
                            m.lfo.frequency = m.baseFrequency + m.in[0];
                            m.lfo.depth = m.baseDepth + m.in[1];
                            m.lfo.dcoffset = m.baseDcoffset + m.in[2];
                            m.lfo.skew = m.baseSkew + m.in[3];
                            m.lfo.curve = m.baseCurve + m.in[4];
                            m.lfo.quantum(double(v.index - quantumIndex) * dt);
                        }
                        out = m.lfo.next(sr);
                    } else if (m.kind == 4) { // a live control, read once per render quantum, smoothed (~10 ms)
                        if (quantumStart && liveControl_) m.liveTarget = liveControl_(m.liveKind, m.liveA, m.liveB);
                        m.liveValue += (m.liveTarget - m.liveValue) * liveSmoothing;
                        out = m.liveSlope * (m.liveValue - m.liveC0);
                        if (!std::isnan(m.liveMin)) out = clampd(out, m.liveMin, m.liveMax);
                    } else if (m.kind == 3) { // the bus mixed down to mono, as an AudioParam input
                        const auto& bb = (*audioBuses_)[size_t(m.bus)].buf;
                        out = (m.dc + 0.5 * (double(bb[0][size_t(i)]) + double(bb[1][size_t(i)]))) * m.bmodDepth;
                        if (!std::isnan(m.bmodMin)) out = clampJs(out, m.bmodMin, m.bmodMax);
                    } else {
                        if (quantumStart) m.env.quantum(double(v.index - quantumIndex) * dt);
                        out = m.env.paramMod ? m.env.sampleWith(m.in[1], m.in[5], m.in[6], m.in[7], m.in[8])
                                             : m.env.next(quantumIndex == 0, m.in[1]);
                    }
                    if (m.target >= ModTarget::ModFrequency) {
                        v.mods[size_t(m.targetSlot)].in[int(m.target) - int(ModTarget::ModFrequency)] += out;
                    } else {
                        FxStage& tgt = m.stage < 0 ? static_cast<FxStage&>(v) : (*stages_)[size_t(v.stages[size_t(m.stage)])];
                        tgt.modSum[size_t(m.target)] += out;
                    }
                }
                if (quantumStart) v.sampleDetuneMod = ms[size_t(ModTarget::SourceFrequency)];
                if (v.orbitMods) { // the orbit's delay and djf, summed over its voices
                    Orbit& ob = *v.orbit;
                    ob.modsUsed = true;
                    ob.mods[0][size_t(i)] += float(ms[size_t(ModTarget::DelayTime)]);
                    ob.mods[1][size_t(i)] += float(ms[size_t(ModTarget::DelayFeedback)]);
                    ob.mods[2][size_t(i)] += float(ms[size_t(ModTarget::Djf)]);
                }
                if (quantumStart && v.modulated) { // source LFOs (worklets reading [0])
                    v.pwLfo.apply(ms[size_t(ModTarget::PwLfoFreq)], ms[size_t(ModTarget::PwLfoDepth)], 0, 0);
                    v.wtLfo.apply(ms[size_t(ModTarget::WtLfoFreq)], ms[size_t(ModTarget::WtLfoDepth)], 0, ms[size_t(ModTarget::WtLfoSkew)]);
                    v.warpLfo.apply(ms[size_t(ModTarget::WarpLfoFreq)], ms[size_t(ModTarget::WarpLfoDepth)], 0,
                                    ms[size_t(ModTarget::WarpLfoSkew)]);
                }
            }
            auto mod = [&](ModTarget target) { return ms[size_t(target)]; };
            if (quantumStart) {
                // the quantum's start, from an exact sample count (before the onset for the first one)
                const double qt = double(v.index - quantumIndex) * dt;
                v.pwLfo.quantum(qt);
                v.wtLfo.quantum(qt);
                v.warpLfo.quantum(qt);
                if (qt >= v.sourceStop) v.workletEnded = true;
                else if (qt > 0) v.workletStarted = true; // currentTime > begin
                v.pulseEnv = 1;                           // `let env = 1` in each process() call
            }
            float l = 0.f, r = 0.f;
            int ch = 1;
            auto modulatedFrequency = [&](double& cents) {
                cents = v.detune.valueAt(t);
                if (v.vib) cents += v.vibOsc.next(waves, v.vibFreq + mod(ModTarget::VibRate)) * (v.vibDepthCents + mod(ModTarget::VibDepth));
                double f = v.frequency.valueAt(t) + mod(ModTarget::SourceFrequency);
                if (v.fm) {
                    double in[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0}; // frequency modulation into each target
                    for (int oi = 0; oi < v.numFmOrder; ++oi) {
                        const int k = v.fmOrder[size_t(oi)];
                        Voice::FmOp& op = v.fmOps[size_t(k)];
                        double m;
                        if (op.noise) {
                            m = (*op.noise)[size_t(op.noisePos++ % int64_t(op.noise->size()))];
                        } else {
                            m = op.osc.next(waves, op.freq + in[k] + ms[size_t(ModTarget::FmFreq) + size_t(k - 1)]);
                        }
                        if (op.env) m *= op.envGain.valueAt(t);
                        for (int e = 0; e < v.numFmEdges; ++e) {
                            const auto& ed = v.fmEdges[size_t(e)];
                            if (ed.src != k) continue;
                            const double amt = ed.amt + (v.fmGainEdge[size_t(k)] == e ? ms[size_t(ModTarget::FmIndex) + size_t(k - 1)] : 0.0);
                            in[ed.dst] += m * amt * op.freq;
                        }
                    }
                    f += in[0];
                }
                return f;
            };
            if (t < v.sourceStop) {
                if (v.type == SoundType::Synth && v.wave == SynthWave::Sbd) {
                    const double cents = v.detune.valueAt(t);
                    const double hz = (v.frequency.valueAt(t) + mod(ModTarget::SourceFrequency)) * (cents == 0.0 ? 1.0 : std::pow(2.0, cents / 1200.0));
                    double s = v.osc.next(waves, hz);
                    // WaveShaperNode: linear interpolation in the curve over [-1, 1]
                    const double pos = (clampd(s, -1, 1) + 1) * 0.5 * (v.sbdCurveSize - 1);
                    const int k = int(clampd(std::floor(pos), 0, v.sbdCurveSize - 2));
                    s = v.sbdCurve[k] + (v.sbdCurve[k + 1] - v.sbdCurve[k]) * (pos - k);
                    s *= v.sbdGain.valueAt(t);
                    const auto& nb = *v.sbdNoise;
                    const double n = nb[size_t(v.sbdNoisePos++ % int64_t(nb.size()))] * v.sbdNoiseGain.valueAt(t);
                    l = float((s + n) * v.sbdMix.valueAt(t));
                } else if (v.type == SoundType::Synth && v.wave == SynthWave::Supersaw) {
                    const double env = v.envGain.valueAt(t);
                    if (v.workletStarted && !v.workletEnded) {
                        double cents;
                        const double f = modulatedFrequency(cents);
                        const double ps = (v.sawPanspread + mod(ModTarget::PanSpread)) * 0.5 + 0.5;
                        double gL = std::sqrt(1 - ps), gR = std::sqrt(ps);
                        const double freq = f * (cents == 0.0 ? 1.0 : std::pow(2.0, cents / 100.0 / 12.0));
                        const int nv = v.sawVoices;
                        const double fs = v.sawFreqspread + mod(ModTarget::FreqSpread);
                        const double scale = nv < 2 ? 0 : fs / (nv - 1), center = fs * 0.5;
                        double sl = 0, sr2 = 0;
                        for (int k = 0; k < nv; ++k) {
                            const double det = nv < 2 ? 0 : k * scale - center;
                            const double dtk = fracd(freq * std::pow(2.0, det / 12.0) * dt);
                            double& ph = v.sawPhase[size_t(k)];
                            const double s = 2 * ph - 1 - polyBlep(ph, dtk);
                            sl += s * gL;
                            sr2 += s * gR;
                            double pn = ph + dtk;
                            if (pn >= 1.0) pn -= 1.0;
                            ph = pn;
                            std::swap(gL, gR);
                        }
                        l = float(sl * env);
                        r = float(sr2 * env);
                    }
                    ch = 2;
                } else if (v.type == SoundType::Synth && v.wave == SynthWave::Pulse) {
                    double out = 0;
                    const double pwl = v.pwLfo.next(sr);
                    if (v.workletStarted && !v.workletEnded) {
                        const double pw = (1 - clampd(v.pulsewidth.valueAt(t) + pwl + mod(ModTarget::PulseWidth), -0.99, 0.99)) * kPi;
                        double cents;
                        const double f = modulatedFrequency(cents);
                        const double freq = f * (cents == 0.0 ? 1.0 : std::pow(2.0, cents / 100.0 / 12.0));
                        const double dphi = freq * 2 * kPi * dt;
                        v.dphif += 0.1 * (dphi - v.dphif);
                        v.pulseEnv *= 0.9998;
                        v.envf += 0.1 * (v.pulseEnv - v.envf);
                        v.B = std::max(0.0, 2.3 * (1 - 0.0001 * freq));
                        v.phi += v.dphif;
                        if (v.phi >= kPi) v.phi -= 2 * kPi;
                        const double out0 = std::cos(v.phi + v.B * v.Y0);
                        v.Y0 = 0.5 * (out0 + v.Y0);
                        const double out1 = std::cos(v.phi + v.B * v.Y1 + pw);
                        v.Y1 = 0.5 * (out1 + v.Y1);
                        out = 0.15 * (out0 - out1) * v.envf;
                    }
                    l = float(out * v.envGain.valueAt(t));
                } else if (v.type == SoundType::Synth && v.wave == SynthWave::Bus) {
                    const double env = v.envGain.valueAt(t);
                    if (v.busIn >= 0) {
                        const auto& bb = (*audioBuses_)[size_t(v.busIn)].buf;
                        l = float(bb[0][size_t(i)] * env);
                        r = float(bb[1][size_t(i)] * env);
                    }
                    ch = 2;
                } else if (v.type == SoundType::Synth) {
                    double cents;
                    const double f = modulatedFrequency(cents);
                    const double hz = f * (cents == 0.0 ? 1.0 : std::pow(2.0, cents / 1200.0));
                    double s = v.wave == SynthWave::One ? 1.0 : v.osc.next(waves, hz);
                    if (v.noiseMix) s = s * v.noiseDry + v.noiseSample() * v.noiseWet;
                    l = float(s * v.sourceGain * v.envGain.valueAt(t));
                } else if (v.type == SoundType::Noise) {
                    l = float(v.noiseSample() * v.sourceGain * v.envGain.valueAt(t));
                } else if (v.type == SoundType::Wavetable) {
                    const double env = v.envGain.valueAt(t);
                    const double posMod = v.wtLfo.next(sr), warpMod = v.warpLfo.next(sr);
                    if (v.workletStarted && !v.workletEnded) {
                        double cents;
                        const double f0 = modulatedFrequency(cents);
                        const double f = f0 * (cents == 0.0 ? 1.0 : std::pow(2.0, cents / 100.0 / 12.0));
                        const double tablePos = clampd(v.wtPosition.valueAt(t) + posMod + mod(ModTarget::WtPosition), 0, 1);
                        const double idx = tablePos * (v.wtNumFrames - 1);
                        const int fIdx = int(idx);
                        const double interpT = idx - fIdx;
                        const double warpAmount = clampd(v.wtWarp.valueAt(t) + warpMod + mod(ModTarget::WtWarp), 0, 1);
                        const int nv = v.wtVoices;
                        const double panspread = nv > 1 ? clampd(v.wtPanspread + mod(ModTarget::PanSpread), 0, 1) : 0;
                        const double gain1 = std::sqrt(0.5 - 0.5 * panspread), gain2 = std::sqrt(0.5 + 0.5 * panspread);
                        const double normalizer = 1 / std::sqrt(double(nv));
                        const double wfs = std::max(0.0, v.wtFreqspread + mod(ModTarget::FreqSpread)); // min 0
                        const double scale = nv < 2 ? 0 : wfs / (nv - 1), center = wfs * 0.5;
                        const float* data = v.sample->channel(0);
                        const float* frameA = data + int64_t(fIdx) * v.wtFrameLen;
                        const float* frameB = data + int64_t(std::min(v.wtNumFrames - 1, fIdx + 1)) * v.wtFrameLen;
                        double sl = 0, sr2 = 0;
                        for (int k = 0; k < nv; ++k) {
                            const bool odd = (k & 1) == 1;
                            const double gL = odd ? gain2 : gain1, gR = odd ? gain1 : gain2;
                            const double det = nv < 2 ? 0 : k * scale - center;
                            const double dPhase = f * std::pow(2.0, det / 12.0) * dt;
                            double& ph = v.sawPhase[size_t(k)];
                            const double wph = warpPhase(ph, warpAmount, v.wtWarpMode);
                            const double s0 = sampleFrame(frameA, v.wtFrameLen, wph);
                            const double s1 = sampleFrame(frameB, v.wtFrameLen, wph);
                            double smp = (s1 - s0) * interpT + s0;
                            if (v.wtWarpMode == 21 && ph < warpAmount) smp = -smp;
                            sl += smp * gL * normalizer;
                            sr2 += smp * gR * normalizer;
                            ph = fracd(ph + dPhase);
                        }
                        l = float(sl * env);
                        r = float(sr2 * env);
                    } else {
                        v.wtPosition.valueAt(t);
                        v.wtWarp.valueAt(t);
                    }
                    ch = 2;
                } else if (v.type == SoundType::Buffer) {
                    const SampleData& s = *v.sample;
                    const bool bytebeat = v.sourceChannels == 2;
                    const double env = bytebeat ? v.envGain.valueAt(t) : 1.0;
                    if (!bytebeat || (v.workletStarted && !v.workletEnded)) {
                        const double frame = v.position * s.sampleRate;
                        const int64_t i0 = int64_t(frame);
                        if (i0 < s.frames) {
                            const float frac = float(frame - double(i0));
                            const float* d = s.channel(0);
                            const float a = d[i0], b = i0 + 1 < s.frames ? d[i0 + 1] : 0.f;
                            l = float((a + (b - a) * frac) * env);
                        }
                        v.position += dt;
                    }
                    r = l;
                    ch = bytebeat ? 2 : 1;
                } else if (v.type == SoundType::ScSynth) {
                    // dirt_gate: Env([0, 1, 1, 0], [fadeInTime, hold, fadeTime], \sin); DirtEvent sends
                    // fadeInTime nil (0) and fadeTime 0.001
                    const double fade = 0.001, total = v.scTotal;
                    double env = 1;
                    auto sinShape = [](double x) { return 0.5 - 0.5 * std::cos(kPi * clampd(x, 0, 1)); };
                    if (t > total - fade) env = sinShape((total - t) / fade);
                    env *= dirtEnvelope(t, v) * v.scAmp;
                    if (v.scSlot >= 0) {
                        const int64_t at = v.scReadFrom + v.index;
                        const float gl = synthServer_->sample(v.scSlot, 0, at), gr = synthServer_->sample(v.scSlot, 1, at);
                        l = float(gl * env);
                        r = float(gr * env);
                        // gate closed: the synth freed itself once its output stays at exactly 0
                        if (v.scGateClosed && at > v.scGateAt) {
                            v.scSilent = gl == 0.f && gr == 0.f ? v.scSilent + 1 : 0;
                            if (v.scSilent > int(0.25 * sampleRate_)) v.endTime = std::min(v.endTime, t);
                        }
                    }
                    ch = 2;
                } else if (v.sample && !v.sampleEnded && t >= v.sampleStart) {
                    const SampleData& s = *v.sample;
                    double cents = v.detune.valueAt(t) + v.sampleDetuneMod;
                    if (v.vib) cents += v.vibOsc.next(waves, v.vibFreq + mod(ModTarget::VibRate)) * (v.vibDepthCents + mod(ModTarget::VibDepth));
                    const double dur = s.duration();
                    double pos = v.position;
                    if (v.loop) {
                        const double ls = v.loopStart < v.loopEnd ? v.loopStart : 0.0;
                        const double le = v.loopStart < v.loopEnd ? v.loopEnd : dur;
                        if (pos >= le) pos = ls + std::fmod(pos - ls, std::max(1e-9, le - ls));
                    }
                    if (pos >= dur || pos < 0) {
                        v.sampleEnded = !v.loop;
                    } else {
                        const double frame = (v.reverse ? dur - pos : pos) * s.sampleRate - (v.reverse ? 1.0 : 0.0);
                        const int64_t i0 = int64_t(std::floor(frame));
                        const float frac = float(frame - double(i0));
                        auto read = [&](int c) {
                            const float* d = s.channel(c);
                            const int64_t a = std::clamp<int64_t>(i0, 0, s.frames - 1);
                            const int64_t b = std::clamp<int64_t>(i0 + 1, 0, s.frames - 1);
                            return d[a] + (d[b] - d[a]) * frac;
                        };
                        const float env = v.envGain.valueAt(t);
                        l = read(0) * env;
                        if (v.sourceChannels > 1) {
                            r = read(1) * env;
                            ch = 2;
                        }
                        pos += v.playbackRate * (cents == 0.0 ? 1.0 : std::pow(2.0, cents / 1200.0)) * dt;
                    }
                    v.position = pos;
                }
            }
            if (ch == 1) r = l;
            // the FX stages, then the value's own effects
            for (int k = 0; k < v.numStages; ++k)
                processStage((*stages_)[size_t(v.stages[size_t(k)])], l, r, ch, i, quantumIndex, quantumStart, v.index,
                             v.absStart + v.index, t, (*stages_)[size_t(v.stages[size_t(k)])].modSum.data(), false,
                             v.sourceStop);
            processStage(v, l, r, ch, i, quantumIndex, quantumStart, v.index, v.absStart + v.index, t, ms.data(), true,
                         v.sourceStop);
            double g = (v.postgain + mod(ModTarget::Post)) * v.out.valueAt(t);
            if (v.fxRelease) g *= v.fxReleaseGain.valueAt(t);
            l *= float(g);
            r *= float(g);
            if (v.analyze)
                if (float* a = analysers_->block(v.analyze, numSamples, v.fftSize)) a[i] += 0.5f * (l + r);
            if (v.fxSlot >= 0) { // StrudelDirt's effects: to their World, back to the orbit kLatency later
                fxServer_->write(v.fxSlot, absSample_ + i, l, r);
                v.t += dt;
                ++v.index;
                continue;
            }
            if (v.busOut >= 0) { // effectSend(post, bus, busgain)
                auto& bb = (*audioBuses_)[size_t(v.busOut)].buf;
                bb[0][size_t(i)] += float(l * v.busGain);
                bb[1][size_t(i)] += float(r * v.busGain);
            }
            // to the orbit
            const double dg = v.dryOn ? v.dryGain : 1.0;
            o.dry[0][size_t(i)] += float(l * dg);
            o.dry[1][size_t(i)] += float(r * dg);
            if (v.delaySend > 0) {
                const double send = v.delaySend + mod(ModTarget::DelaySend);
                o.delaySend[0][size_t(i)] += float(l * send);
                o.delaySend[1][size_t(i)] += float(r * send);
            }
            if (v.roomSend > 0) {
                const double send = v.roomSend + mod(ModTarget::RoomSend);
                o.roomSend[0][size_t(i)] += float(l * send);
                o.roomSend[1][size_t(i)] += float(r * send);
                o.lastRoomInput = absSample_ + i;
            }
            v.t += dt;
            ++v.index;
        }
        if (v.active) ++count;
    };
    // voices reading a bus render after the voices writing it (no delay, as in the WebAudio graph)
    bool anyReader = false;
    for (auto& v : *voices_) {
        v.level = 0;
        anyReader = anyReader || (v.active && v.readsBus);
    }
    if (!anyReader) {
        for (auto& v : *voices_) renderVoice(v);
    } else {
        auto reads = [](const Voice& v, int slot) {
            if (v.busIn == slot) return true;
            for (int k = 0; k < v.numMods; ++k)
                if (v.mods[size_t(k)].kind == 3 && v.mods[size_t(k)].bus == slot) return true;
            return false;
        };
        int maxLevel = 0;
        for (int pass = 0; pass < 4; ++pass)
            for (auto& rv : *voices_) {
                if (!rv.active || !rv.readsBus) continue;
                for (const auto& wv : *voices_)
                    if (&wv != &rv && wv.active && wv.busOut >= 0 && reads(rv, wv.busOut))
                        rv.level = std::max(rv.level, std::min(4, wv.level + 1));
                maxLevel = std::max(maxLevel, rv.level);
            }
        for (int level = 0; level <= maxLevel; ++level)
            for (auto& v : *voices_)
                if (v.level == level) renderVoice(v);
    }
    if (fxServer_) { // StrudelDirt's effects: process, then each chain's output to its orbit kLatency later
        // a voice that ended this block: its chain was written up to its end (releaseSynth)
        fxServer_->processTo(absSample_ + numSamples);
        for (int k = 0; k < int(fxDrains_->size()); ++k) {
            FxDrain& d = (*fxDrains_)[size_t(k)];
            if (!d.active) continue;
            Orbit& o = *d.orbit;
            for (int i = 0; i < numSamples; ++i) {
                const int64_t src = absSample_ + i - FxServer::kLatency;
                if (src < d.start || (d.voiceDone && src >= d.end)) continue;
                const float l = fxServer_->output(k, 0, src), r = fxServer_->output(k, 1, src);
                if (d.busOut >= 0) {
                    auto& bb = (*audioBuses_)[size_t(d.busOut)].buf;
                    bb[0][size_t(i)] += float(l * d.busGain);
                    bb[1][size_t(i)] += float(r * d.busGain);
                }
                o.dry[0][size_t(i)] += float(l * d.dry);
                o.dry[1][size_t(i)] += float(r * d.dry);
                if (d.delay > 0) {
                    o.delaySend[0][size_t(i)] += float(l * d.delay);
                    o.delaySend[1][size_t(i)] += float(r * d.delay);
                }
                if (d.room > 0) {
                    o.roomSend[0][size_t(i)] += float(l * d.room);
                    o.roomSend[1][size_t(i)] += float(r * d.room);
                    o.lastRoomInput = absSample_ + i;
                }
            }
            if (d.voiceDone && absSample_ + numSamples - FxServer::kLatency >= d.end) {
                fxServer_->stop(k);
                d.active = false;
            }
        }
    }
    if (orbitFx_) { // StrudelDirt's orbit effects on each orbit's dry sum, back kLatency later
        bool any = false;
        for (auto& o : *orbits_)
            if (o.used && (o.leslieOn || o.shimmerOn)) {
                any = true;
                for (int i = 0; i < numSamples; ++i)
                    orbitFx_->write(o.number % 16, absSample_ + i, o.dry[0][size_t(i)], o.dry[1][size_t(i)]);
            }
        orbitFx_->processTo(absSample_ + numSamples);
        if (any)
            for (auto& o : *orbits_) {
                if (!o.used || !(o.leslieOn || o.shimmerOn)) continue;
                const float keep = o.leslieOn ? 1.f - o.leslieAmount : 1.f; // leslie: ReplaceOut(dryBus, in * (1 - leslie))
                for (int i = 0; i < numSamples; ++i) {
                    const int64_t src = absSample_ + i - OrbitFxServer::kLatency;
                    for (int c = 0; c < 2; ++c)
                        o.dry[size_t(c)][size_t(i)] = o.dry[size_t(c)][size_t(i)] * keep + orbitFx_->output(o.number % 16, c, src);
                }
            }
    }
    renderOrbits(buses, numBuses, numSamples);
    // analyser slot 0: the main output (Concha's shaders read it as Shadertoy's audio texture)
    if (numBuses > 0 && buses[0] && buses[0][0])
        if (float* a = analysers_->block(0, numSamples, 1024)) {
            const float* l = buses[0][0];
            const float* r = buses[0][1] ? buses[0][1] : l;
            for (int i = 0; i < numSamples; ++i) a[i] += 0.5f * (l[i] + r[i]);
        }
    analysers_->endBlock(numSamples);
    absSample_ += numSamples;
    active_.store(count, std::memory_order_relaxed);
}

void Engine::renderOrbits(float* const* const* buses, int numBuses, int numSamples)
{
    const double sr = sampleRate_, dt = 1.0 / sr;
    for (auto& o : *orbits_) {
        if (!o.used) continue;
        // a new impulse response is ready: swap it in (the builder frees the old one)
        if (Convolver* p = o.pending.load(std::memory_order_acquire); p && !o.retired.load(std::memory_order_acquire)) {
            o.pending.store(nullptr, std::memory_order_release);
            o.retired.store(o.reverb.release(), std::memory_order_release);
            o.reverb.reset(p);
        }
        auto& dl = o.delayLine;
        const int dlen = int(dl[0].size());
        float roomOut[2][kQuantum];
        const int bus = numBuses > 0 ? o.bus % numBuses : 0;
        float* outL = numBuses > 0 && buses[bus] ? buses[bus][0] : nullptr;
        float* outR = numBuses > 0 && buses[bus] ? buses[bus][1] : nullptr;
        // the convolver idles once its input has been silent for longer than any response (8 s)
        const bool reverbActive = o.reverb && o.hasReverb && o.lastRoomInput >= 0 &&
                                  absSample_ + numSamples - o.lastRoomInput < int64_t(sr * 8.0);
        for (int start = 0; start < numSamples;) {
            // chunks end on render quantum boundaries (djf reads its value once per quantum)
            const int64_t abs0 = absSample_ + start;
            const int n = int(std::min<int64_t>(numSamples - start, kQuantum - (abs0 & (kQuantum - 1))));
            if ((abs0 & (kQuantum - 1)) == 0 && o.hasDjf)
                o.djfBlockValue = o.djfValue.valueAt(double(abs0) * dt) + (o.modsUsed ? o.mods[2][size_t(start)] : 0.f);
            if (reverbActive)
                o.reverb->process(o.roomSend[0].data() + start, o.roomSend[1].data() + start, roomOut[0], roomOut[1], n);
            for (int k = 0; k < n; ++k) {
                const int i = start + k;
                const double t = double(absSample_ + i) * dt;
                float sl = o.dry[0][size_t(i)], srr = o.dry[1][size_t(i)];
                if (o.hasDelay) {
                    // DelayNode in a cycle: at least one render quantum of delay; linear interpolation
                    const double dtime = double(o.delayTime.valueAt(t)) + (o.modsUsed ? o.mods[0][size_t(i)] : 0.f);
                    const double d = std::max(double(kQuantum), clampd(dtime, 0, 1) * sr); // DelayNode: [0, maxDelayTime]
                    const float fb = o.feedback.valueAt(t) + (o.modsUsed ? o.mods[1][size_t(i)] : 0.f);
                    double rp = o.delayWrite - d;
                    while (rp < 0) rp += dlen;
                    const int a = int(rp), b = (a + 1) % dlen;
                    const float fr = float(rp - a);
                    for (int c = 0; c < 2; ++c) {
                        auto& line = dl[size_t(c)];
                        const float y = line[size_t(a)] + (line[size_t(b)] - line[size_t(a)]) * fr;
                        line[size_t(o.delayWrite)] = o.delaySend[size_t(c)][size_t(i)] + fb * y;
                        if (c == 0) sl += y;
                        else srr += y;
                    }
                    o.delayWrite = (o.delayWrite + 1) % dlen;
                }
                if (reverbActive) {
                    sl += roomOut[0][k];
                    srr += roomOut[1][k];
                }
                if (o.hasDjf) { // djf-processor
                    const double value = clampd(o.djfBlockValue, 0, 1);
                    int type = 0;
                    double vv = 1;
                    if (value > 0.51) {
                        type = 2;
                        vv = (value - 0.5) * 2;
                    } else if (value < 0.49) {
                        type = 1;
                        vv = value * 2;
                    }
                    if (type) {
                        const double cutoff = clampd(std::pow(vv * 11, 4), 0, sr / 2 - 1);
                        const double cc = clampd(2 * std::sin(cutoff * kPi * dt), 0, 1.14);
                        const double rr = std::pow(0.5, 8 * 0.1 + 1);
                        const double mrc = 1 - rr * cc;
                        float* io[2] = {&sl, &srr};
                        for (int c = 0; c < 2; ++c) {
                            const double x = *io[c];
                            o.djfS0[c] = mrc * o.djfS0[c] - cc * o.djfS1[c] + cc * x;
                            o.djfS1[c] = mrc * o.djfS1[c] + cc * o.djfS0[c];
                            *io[c] = float(type == 1 ? o.djfS1[c] : x - o.djfS1[c]);
                        }
                    }
                }
                const float g = o.outGain.valueAt(t);
                if (outL) outL[i] += sl * g;
                if (outR) outR[i] += srr * g;
            }
            start += n;
        }
    }
}

} // namespace concha::sd
