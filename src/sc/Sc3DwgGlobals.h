#pragma once

// sc3-plugins' dwglib (DWGUGens, OteyPianoUGens) calls two of its free functions as ::groupdelay and
// ::PhaseDelayDerive. Wrapped in its unit's namespace (cmake/Sc3Plugins.cmake), they live there:
// global ones forward to them.

namespace CONCHA_SC3_UNIT {
float groupdelay(float, float*, int, float*, int, float);
float PhaseDelayDerive(float, float*, int, float*, int, float);
} // namespace CONCHA_SC3_UNIT

static inline float groupdelay(float f, float* B, int sB, float* A, int sA, float fs)
{
    return CONCHA_SC3_UNIT::groupdelay(f, B, sB, A, sA, fs);
}
static inline float PhaseDelayDerive(float o, float* B, int sB, float* A, int sA, float d = 0.0005f)
{
    return CONCHA_SC3_UNIT::PhaseDelayDerive(o, B, sB, A, sA, d);
}
