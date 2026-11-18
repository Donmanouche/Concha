#pragma once

// One sound event for superdough C++: the hap's value as superdough reads it, flattened into a
// fixed struct so it can travel through the lock-free queue. A NaN float means "not set"
// (undefined in JS), so superdough's `value.x ?? default` logic ports as is.
//
// The pattern thread fills it (sched/PatternWorker, from the JS value resolved by
// js/src/sounds.mjs); the audio thread plays it (superdough/Engine).

#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

namespace concha::sd {

// Number controls read by superdough C++ (canonical Strudel names).
#define CONCHA_SD_NUMBER_PARAMS(X)                                                                                     \
    X(gain) X(velocity) X(postgain) X(attack) X(decay) X(sustain) X(release) X(pan) X(orbit) X(n)                    \
    X(cutoff) X(resonance) X(lpattack) X(lpdecay) X(lpsustain) X(lprelease) X(lpenv)                                 \
    X(hcutoff) X(hresonance) X(hpattack) X(hpdecay) X(hpsustain) X(hprelease) X(hpenv)                               \
    X(bandf) X(bandq) X(bpattack) X(bpdecay) X(bpsustain) X(bprelease) X(bpenv) X(fanchor)                           \
    X(speed) X(begin) X(end) X(loop) X(loopBegin) X(loopEnd) X(cut) X(clip) X(nudge)                                 \
    X(vib) X(vibmod) X(penv) X(pattack) X(pdecay) X(psustain) X(prelease) X(pcurve) X(panchor)                       \
    X(fmi) X(fmh) X(fmattack) X(fmdecay) X(fmsustain) X(fmrelease)                                                     \
    X(noise) X(density) X(FXrelease) X(octave)                                                                        \
    X(delay) X(delayfeedback) X(delaysync) X(delaytime) X(dry) X(room) X(roomsize) X(roomfade) X(roomlp) X(roomdim)  \
    X(irspeed) X(irbegin) X(bus) X(busgain)                                                                                             \
    X(djf) X(coarse) X(crush) X(shape) X(shapevol) X(distort) X(distortvol)                                          \
    X(tremolo) X(tremolosync) X(tremolodepth) X(tremoloskew) X(tremolophase)                                         \
    X(phaserrate) X(phaserdepth) X(phasercenter) X(phasersweep)                                                      \
    X(lprate) X(lpsync) X(lpdepth) X(lpdepthfrequency) X(lpdc) X(lpskew)                                             \
    X(hprate) X(hpsync) X(hpdepth) X(hpdepthfrequency) X(hpdc) X(hpskew)                                             \
    X(bprate) X(bpsync) X(bpdepth) X(bpdepthfrequency) X(bpdc) X(bpskew) X(drive)                                    \
    X(unison) X(spread) X(detune) X(pw) X(pwrate) X(pwsweep)                                                        \
    X(compressor) X(compressorRatio) X(compressorKnee) X(compressorAttack) X(compressorRelease)                     \
    X(wt) X(wtenv) X(wtattack) X(wtdecay) X(wtsustain) X(wtrelease) X(wtrate) X(wtsync) X(wtdepth) X(wtdc) X(wtskew) \
    X(warp) X(warpenv) X(warpattack) X(warpdecay) X(warpsustain) X(warprelease) X(warprate) X(warpsync) X(warpdepth)  \
    X(warpdc) X(warpskew) X(wtphaserand) X(transient) X(transsustain) X(stretch)

enum class SoundType : uint8_t { Synth, Noise, Sample, Buffer, Wavetable, Unsupported };
enum class SynthWave : uint8_t { Triangle, Square, Sawtooth, Sine, One, User, Supersaw, Pulse, Sbd, Bus, Other };
enum class FilterModel : uint8_t { Db12, Ladder, Db24 };

// superdough modulators (modulators.mjs connectLFO / connectEnvelope) on a parameter of the voice.
enum class ModTarget : uint8_t {
    None, SourceFrequency, Gain, Post, Pan, LpfFrequency, LpfQ, HpfFrequency, HpfQ, BpfFrequency, BpfQ,
    TremoloRate, TremoloGain, PhaserFrequency, PhaserQ, Coarse, Crush, Shape, Distort, DelaySend, RoomSend, Dry,
    VibRate, VibDepth, PulseWidth, WtPosition, WtWarp,
    Stretch, TremoloSkew,
    LpfLfoDepth, LpfLfoDc, LpfLfoSkew, HpfLfoDepth, HpfLfoDc, HpfLfoSkew, BpfLfoDepth, BpfLfoDc, BpfLfoSkew, // filter LFOs
    VowelFreq, ShapeVol, DistortVol, CompThreshold, CompRatio, CompKnee, CompAttack, CompRelease,
    DelayTime, DelayFeedback, Djf, FreqSpread, PanSpread,
    WtLfoFreq, WtLfoDepth, WtLfoSkew, WarpLfoFreq, WarpLfoDepth, WarpLfoSkew, PwLfoFreq, PwLfoDepth,
    PhaserLfoFreq, PhaserLfoDepth,
    FmIndex, FmIndex2, FmIndex3, FmIndex4, FmIndex5, FmIndex6, FmIndex7, FmIndex8, // fm_N_gain
    FmFreq, FmFreq2, FmFreq3, FmFreq4, FmFreq5, FmFreq6, FmFreq7, FmFreq8,         // fm_N frequency
    // another modulator's parameter (targetIndex), in ModState::in order
    ModFrequency, ModDepth, ModDcoffset, ModSkew, ModCurve, ModAttack, ModDecay, ModSustain, ModRelease,
    Count
};
struct Modulator {
    uint8_t kind = 0; // 0 none, 1 lfo, 2 env, 3 bmod (dcoffset = its dc)
    float bus = 0;    // bmod: the bus read
    ModTarget target = ModTarget::None;
    int8_t targetIndex = -1; // ModFrequency...: index of the modulated modulator
    int8_t fxi = -1;         // the FX stage whose nodes it modulates (-1: main)
    float rate = std::numeric_limits<float>::quiet_NaN(), sync = std::numeric_limits<float>::quiet_NaN();
    float depth = std::numeric_limits<float>::quiet_NaN(), depthabs = std::numeric_limits<float>::quiet_NaN();
    float dcoffset = std::numeric_limits<float>::quiet_NaN(), skew = std::numeric_limits<float>::quiet_NaN();
    float curve = std::numeric_limits<float>::quiet_NaN(), retrig = std::numeric_limits<float>::quiet_NaN();
    int8_t shape = -1;
    float attack = std::numeric_limits<float>::quiet_NaN(), decay = std::numeric_limits<float>::quiet_NaN();
    float sustain = std::numeric_limits<float>::quiet_NaN(), release = std::numeric_limits<float>::quiet_NaN();
    float acurve = 0, dcurve = 0, rcurve = 0;
};

// One FM operator of applyFM's matrix (fmhN, fmwaveN, fmenvN, fmattackN...).
struct FmOperator {
    float h = std::numeric_limits<float>::quiet_NaN();
    float attack = std::numeric_limits<float>::quiet_NaN(), decay = std::numeric_limits<float>::quiet_NaN();
    float sustain = std::numeric_limits<float>::quiet_NaN(), release = std::numeric_limits<float>::quiet_NaN();
    uint8_t wave = 0; // sine, square, sawtooth, triangle, then noises: white, pink, brown, crackle
    bool envLinear = false;
};

struct SoundParams {
    static constexpr int kMaxModulators = 8;
    Modulator modulators[kMaxModulators];
    int8_t numModulators = 0;

#define CONCHA_SD_DECLARE(name) float name = std::numeric_limits<float>::quiet_NaN();
    CONCHA_SD_NUMBER_PARAMS(CONCHA_SD_DECLARE)
#undef CONCHA_SD_DECLARE

    SoundType type = SoundType::Synth;
    SynthWave wave = SynthWave::Triangle; // Synth
    uint8_t noiseType = 0;                // Noise: NoiseType
    uint8_t bufferMode = 0;               // Buffer (rendered in JS): 0 zzfx, 1 bytebeat
    int8_t warpmode = 0, wtshape = -1, warpshape = -1; // Wavetable
    uint16_t frameLen = 2048;
    FilterModel ftype = FilterModel::Db12;
    // FM matrix: operators 1..8 (operator 1 also from fmh / fmattack... above) and
    // fmMatrix[i][j] = amount from operator i into j (0 = the carrier's frequency); 0 = none, as
    // `if (!amt) continue` in applyFM
    FmOperator fmOps[9];
    float fmMatrix[9][9] = {};
    uint8_t fmWave = 0;                   // Wave of the FM modulator (sine, square, sawtooth, triangle)
    bool fmEnvLinear = false;             // fmenv: 'lin' (default 'exp')
    bool unitCycles = false;              // unit("c")
    int8_t channel = -1;                  // first output channel from .channels() (0-based), -1: orbit's
    int8_t vowel = -1;                    // vowelFormant index, -1: none
    int8_t distorttype = 0;               // distortionAlgorithms index
    int8_t tremoloshape = -1, lpshape = -1, hpshape = -1, bpshape = -1; // LFO shapes, -1: not set
    int8_t numDuck = 0;
    float duckorbit[4] = {}, duckonset[4] = {}, duckattack[4] = {}, duckdepth[4] = {};
    int8_t numDuckOnset = 0, numDuckAttack = 0, numDuckDepth = 0;
    uint32_t customWave = 0;              // partials (or n) -> waveformN table in the WaveStore, 0: stock wave

    float frequency = 0.f;  // getFrequencyFromValue(value) for synths (Hz)
    float transpose = 0.f;  // samples: semitones from getCommonSampleInfo
    uint32_t sampleId = 0;  // samples: SampleCache id (0 = none)
    uint32_t fxChainId = 0;  // FX: FxChainStore chain, 0 = none
    uint32_t kabelId = 0;    // K(): KabelStore program, 0 = none
    uint32_t irSampleId = 0; // ir: the reverb's impulse response (a sample), 0 = generated
    uint32_t soundHash = 0; // name of the sound, for logs
};

inline bool isSet(float v) { return !std::isnan(v); }
inline float orDefault(float v, float fallback) { return std::isnan(v) ? fallback : v; }

} // namespace concha::sd
