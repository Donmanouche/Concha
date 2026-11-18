#pragma once

// Concha's interface to the embedded scsynth (src/sc/SC_HostDriver.cpp).
// One ConchaScWorld per plug-in instance. Packets are OSC messages or bundles, as for scsynth;
// bundle time tags use the driver's sample clock (see concha_sc_process).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ConchaScWorld ConchaScWorld;

typedef struct ConchaScOptions {
    double sampleRate;
    int maxBlockSize;      // largest block passed to concha_sc_process (multiple of 64)
    int numOutputs;        // output bus channels (e.g. 32 for 16 stereo outputs)
    int numInputs;
    const char* pluginPath; // UGen plug-in folders, ':'-separated; NULL for the built-in default
    int verbosity;          // scsynth -V
} ConchaScOptions;

__attribute__((visibility("default"))) ConchaScWorld* concha_sc_new(const ConchaScOptions* options);
__attribute__((visibility("default"))) void concha_sc_free(ConchaScWorld* world);

// Thread-safe; the packet is copied. Returns 1 when queued.
__attribute__((visibility("default"))) int concha_sc_send(ConchaScWorld* world, const char* packet, int size);

// Audio thread. numSamples: multiple of concha_sc_block_size(). oscTime: OSC time (2^-32 s
// units) of the first sample, in the same clock as the bundles' time tags.
__attribute__((visibility("default"))) void concha_sc_process(ConchaScWorld* world, const float* const* inputs,
                                                              int numInputs, float* const* outputs, int numOutputs,
                                                              int numSamples, int64_t oscTime);

__attribute__((visibility("default"))) int concha_sc_block_size(ConchaScWorld* world);
__attribute__((visibility("default"))) int64_t concha_sc_seconds_to_osc(double seconds);

#ifdef __cplusplus
}
#endif
