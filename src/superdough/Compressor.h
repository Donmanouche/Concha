#pragma once

// DynamicsCompressorNode, ported from Chromium's DynamicsCompressorKernel (what strudel.cc runs on):
// knee curve fitted to the ratio, adaptive release (4th order polynomial over the release zones),
// a 6 ms pre-delay (the node's latency), envelope rates updated every 32 frames on the context's
// render quanta. Up to 2 channels; real-time safe.

#include <array>
#include <cmath>

namespace concha::sd {

class Compressor {
public:
    void prepare(double sampleRate);
    void reset();
    // k-rate parameters (DynamicsCompressorNode: threshold dB, knee dB, ratio, attack s, release s)
    void setParams(float threshold, float knee, float ratio, float attack, float release);

    // One sample; `divisionStart`: this sample starts a 32-frame division (on the context's clock).
    void process(float& left, float& right, int channels, bool divisionStart);

private:
    float kneeCurve(float x, float k) const;
    float saturate(float x, float k) const;
    float slopeAt(float x, float k) const;
    float kAtSlope(float desiredSlope) const;
    float updateStaticCurveParameters(float dbThreshold, float dbKnee, float ratio);
    void beginDivision();

    static constexpr int kMaxPreDelayFrames = 1024;
    double sampleRate_ = 48000.0;
    float threshold_ = -24, knee_ = 30, ratio_ = 12, attack_ = 0.003f, release_ = 0.25f;

    // static curve
    float dbThreshold_ = -1e30f, dbKnee_ = -1e30f, ratioCache_ = -1e30f;
    float linearThreshold_ = 0, slope_ = 1, kneeThresholdDb_ = 0, kneeThreshold_ = 0, ykneeThresholdDb_ = 0, K_ = 1;

    // per-division values
    float masterLinearGain_ = 1, scaledDesiredGain_ = 1, envelopeRate_ = 1, satReleaseFrames_ = 1;
    float kA_ = 0, kB_ = 0, kC_ = 0, kD_ = 0, kE_ = 0, attackFrames_ = 1;

    // state
    float detectorAverage_ = 0, compressorGain_ = 1, maxAttackCompressionDiffDb_ = -1;
    std::array<std::array<float, kMaxPreDelayFrames>, 2> preDelay_{};
    int preDelayRead_ = 0, preDelayWrite_ = 0;
};

} // namespace concha::sd
