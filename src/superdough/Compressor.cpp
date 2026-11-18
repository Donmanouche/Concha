#include "superdough/Compressor.h"

#include <algorithm>

namespace concha::sd {

namespace {
constexpr float kPiOverTwo = 1.57079632679489661923f;
inline float dbToLin(float db) { return std::pow(10.f, 0.05f * db); }
inline float linToDb(float x) { return x > 0 ? 20.f * std::log10(x) : -1000.f; }
// Chromium DynamicsCompressor defaults for the parameters the node does not expose
constexpr float kReleaseZone1 = 0.09f, kReleaseZone2 = 0.16f, kReleaseZone3 = 0.42f, kReleaseZone4 = 0.98f;
constexpr float kPreDelaySeconds = 0.006f;
} // namespace

void Compressor::prepare(double sampleRate)
{
    sampleRate_ = sampleRate;
    reset();
}

void Compressor::reset()
{
    // The node exists before the note and has been processing silence: settled state.
    detectorAverage_ = 1;
    compressorGain_ = 1;
    maxAttackCompressionDiffDb_ = -1;
    for (auto& b : preDelay_) b.fill(0.f);
    const int frames = std::min(kMaxPreDelayFrames - 1, int(kPreDelaySeconds * sampleRate_));
    preDelayRead_ = 0;
    preDelayWrite_ = frames;
    dbThreshold_ = dbKnee_ = ratioCache_ = -1e30f;
}

void Compressor::setParams(float threshold, float knee, float ratio, float attack, float release)
{
    // DynamicsCompressorNode ranges
    threshold_ = std::clamp(threshold, -100.f, 0.f);
    knee_ = std::clamp(knee, 0.f, 40.f);
    ratio_ = std::clamp(ratio, 1.f, 20.f);
    attack_ = std::clamp(attack, 0.f, 1.f);
    release_ = std::clamp(release, 0.f, 1.f);
}

float Compressor::kneeCurve(float x, float k) const
{
    if (x < linearThreshold_) return x;
    return linearThreshold_ + (1 - std::exp(-k * (x - linearThreshold_))) / k;
}

float Compressor::saturate(float x, float k) const
{
    if (x < kneeThreshold_) return kneeCurve(x, k);
    const float xDb = linToDb(x);
    const float yDb = ykneeThresholdDb_ + slope_ * (xDb - kneeThresholdDb_);
    return dbToLin(yDb);
}

float Compressor::slopeAt(float x, float k) const
{
    if (x < linearThreshold_) return 1;
    const float x2 = x * 1.001f;
    const float xDb = linToDb(x), x2Db = linToDb(x2);
    const float yDb = linToDb(kneeCurve(x, k)), y2Db = linToDb(kneeCurve(x2, k));
    return (y2Db - yDb) / (x2Db - xDb);
}

float Compressor::kAtSlope(float desiredSlope) const
{
    const float x = dbToLin(dbThreshold_ + dbKnee_);
    float minK = 0.1f, maxK = 10000.f, k = 5.f;
    for (int i = 0; i < 15; ++i) {
        if (slopeAt(x, k) < desiredSlope) maxK = k;
        else minK = k;
        k = std::sqrt(minK * maxK);
    }
    return k;
}

float Compressor::updateStaticCurveParameters(float dbThreshold, float dbKnee, float ratio)
{
    if (dbThreshold != dbThreshold_ || dbKnee != dbKnee_ || ratio != ratioCache_) {
        dbThreshold_ = dbThreshold;
        linearThreshold_ = dbToLin(dbThreshold);
        dbKnee_ = dbKnee;
        ratioCache_ = ratio;
        slope_ = 1 / ratio;
        const float k = kAtSlope(1 / ratio);
        kneeThresholdDb_ = dbThreshold + dbKnee;
        kneeThreshold_ = dbToLin(kneeThresholdDb_);
        ykneeThresholdDb_ = linToDb(kneeCurve(kneeThreshold_, k));
        K_ = k;
    }
    return K_;
}

void Compressor::beginDivision()
{
    const float k = updateStaticCurveParameters(threshold_, knee_, ratio_);
    // makeup gain, with Chromium's perceptual tuning
    const float fullRangeGain = saturate(1, k);
    const float fullRangeMakeupGain = std::pow(1 / fullRangeGain, 0.6f);
    masterLinearGain_ = dbToLin(0.f) * fullRangeMakeupGain;
    attackFrames_ = std::max(0.001f, attack_) * float(sampleRate_);
    const float releaseFrames = float(sampleRate_) * release_;
    satReleaseFrames_ = 0.0025f * float(sampleRate_);
    const float y1 = releaseFrames * kReleaseZone1, y2 = releaseFrames * kReleaseZone2;
    const float y3 = releaseFrames * kReleaseZone3, y4 = releaseFrames * kReleaseZone4;
    kA_ = 0.9999999999999998f * y1 + 1.8432219684323923e-16f * y2 - 1.9373394351676423e-16f * y3 + 8.824516011816245e-18f * y4;
    kB_ = -1.5788320352845888f * y1 + 2.3305837032074286f * y2 - 0.9141194204840429f * y3 + 0.1623677525612032f * y4;
    kC_ = 0.5334142869106424f * y1 - 1.272736789213631f * y2 + 0.9258856042207512f * y3 - 0.18656310191776226f * y4;
    kD_ = 0.08783463138207234f * y1 - 0.1694162967925622f * y2 + 0.08588057951595272f * y3 - 0.00429891410546283f * y4;
    kE_ = -0.042416883008123074f * y1 + 0.1115693827987602f * y2 - 0.09764676325265872f * y3 + 0.028494263462021576f * y4;

    if (std::isnan(detectorAverage_) || std::isinf(detectorAverage_)) detectorAverage_ = 1;
    const float desiredGain = detectorAverage_;
    scaledDesiredGain_ = std::asin(desiredGain) / kPiOverTwo;
    const bool isReleasing = scaledDesiredGain_ > compressorGain_;
    float compressionDiffDb = linToDb(compressorGain_ / scaledDesiredGain_);
    if (isReleasing) {
        maxAttackCompressionDiffDb_ = -1;
        if (std::isnan(compressionDiffDb) || std::isinf(compressionDiffDb)) compressionDiffDb = -1;
        float x = std::min(0.f, std::max(-12.f, compressionDiffDb));
        x = 0.25f * (x + 12);
        const float x2 = x * x, x3 = x2 * x, x4 = x2 * x2;
        const float relFrames = kA_ + kB_ * x + kC_ * x2 + kD_ * x3 + kE_ * x4;
        envelopeRate_ = dbToLin(5.f / relFrames);
    } else {
        if (std::isnan(compressionDiffDb) || std::isinf(compressionDiffDb)) compressionDiffDb = 1;
        if (maxAttackCompressionDiffDb_ == -1 || maxAttackCompressionDiffDb_ < compressionDiffDb)
            maxAttackCompressionDiffDb_ = compressionDiffDb;
        const float effAttenDiffDb = std::max(0.5f, maxAttackCompressionDiffDb_);
        const float x = 0.25f / effAttenDiffDb;
        envelopeRate_ = 1 - std::pow(x, 1 / attackFrames_);
    }
}

void Compressor::process(float& left, float& right, int channels, bool divisionStart)
{
    if (divisionStart) beginDivision();
    float* io[2] = {&left, &right};
    float compressorInput = 0;
    for (int c = 0; c < channels; ++c) {
        const float x = *io[c];
        preDelay_[size_t(c)][size_t(preDelayWrite_)] = x;
        compressorInput = std::max(compressorInput, std::fabs(x));
    }
    const float absInput = compressorInput;
    const float shapedInput = saturate(absInput, K_);
    const float attenuation = absInput <= 0.0001f ? 1 : shapedInput / absInput;
    float attenuationDb = -linToDb(attenuation);
    attenuationDb = std::max(2.f, attenuationDb);
    const float satReleaseRate = dbToLin(attenuationDb / satReleaseFrames_) - 1;
    const float rate = attenuation > detectorAverage_ ? satReleaseRate : 1;
    detectorAverage_ += (attenuation - detectorAverage_) * rate;
    detectorAverage_ = std::min(1.f, detectorAverage_);
    if (std::isnan(detectorAverage_) || std::isinf(detectorAverage_)) detectorAverage_ = 1;
    if (envelopeRate_ < 1) {
        compressorGain_ += (scaledDesiredGain_ - compressorGain_) * envelopeRate_;
    } else {
        compressorGain_ *= envelopeRate_;
        compressorGain_ = std::min(1.f, compressorGain_);
    }
    const float postWarpCompressorGain = std::sin(kPiOverTwo * compressorGain_);
    const float totalGain = masterLinearGain_ * postWarpCompressorGain; // effect blend 1: wet only
    for (int c = 0; c < channels; ++c) *io[c] = preDelay_[size_t(c)][size_t(preDelayRead_)] * totalGain;
    preDelayRead_ = (preDelayRead_ + 1) & (kMaxPreDelayFrames - 1);
    preDelayWrite_ = (preDelayWrite_ + 1) & (kMaxPreDelayFrames - 1);
}

} // namespace concha::sd
