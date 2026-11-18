// scsynth audio driver for Concha: the plug-in's audio callback runs the World.
// Built with SC_AUDIO_API=7 (an API number SuperCollider does not use), see cmake/ScSynth.cmake.
//
// Time: scsynth schedules OSC bundles in OSC time units (2^-32 s). This driver keeps its own
// sample-counted OSC clock: block n starts at n * blockSize / sampleRate seconds. Concha stamps
// its bundles in the same clock, so scheduling is sample-exact and does not drift from the DAW.
//
// Based on SC_Jack.cpp (SuperCollider, GPL-3.0-or-later).

#include "SC_CoreAudio.h"
#include "SC_HiddenWorld.h"
#include "SC_Prototypes.h"
#include "SC_Time.hpp"
#include "SC_WorldOptions.h"

#include "sc/ConchaSc.h"

#include <cmath>
#include <cstring>

int32 server_timeseed() { return timeSeed(); }
int64 oscTimeNow() { return OSCTime(getTime()); }
void initializeScheduler() {}

class SC_HostDriver final : public SC_AudioDriver {
public:
    explicit SC_HostDriver(World* world) : SC_AudioDriver(world) {}

    // numSamples must be a multiple of the World's block size (64).
    void Process(const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples,
                 int64 oscTime)
    {
        World* world = mWorld;
        try {
            mFromEngine.Free();
            mToEngine.Perform();
            mOscPacketsToEngine.Perform();

            const int bufFrames = world->mBufLength;
            const int numBufs = numSamples / bufFrames;
            float* inBuses = world->mAudioBus + world->mNumOutputs * bufFrames;
            float* outBuses = world->mAudioBus;
            int32* inTouched = world->mAudioBusTouched + world->mNumOutputs;
            int32* outTouched = world->mAudioBusTouched;
            const int minInputs = sc_min(numInputs, (int)world->mNumInputs);
            const int minOutputs = sc_min(numOutputs, (int)world->mNumOutputs);

            const int64 oscInc = mOSCincrement;
            const double oscToSamples = mOSCtoSamples;
            mOSCbuftime = oscTime;

            for (int i = 0, pos = 0; i < numBufs; ++i, world->mBufCounter++, pos += bufFrames) {
                const int32 bufCounter = world->mBufCounter;
                for (int k = 0; k < minInputs; ++k) {
                    std::memcpy(inBuses + k * bufFrames, inputs[k] + pos, sizeof(float) * (size_t)bufFrames);
                    inTouched[k] = bufCounter;
                }

                int64 schedTime;
                const int64 nextTime = oscTime + oscInc;
                while ((schedTime = mScheduler.NextTime()) <= nextTime) {
                    const float diffTime = (float)(schedTime - oscTime) * (float)oscToSamples + 0.5f;
                    const float diffTimeFloor = std::floor(diffTime);
                    world->mSampleOffset = (int)diffTimeFloor;
                    world->mSubsampleOffset = diffTime - diffTimeFloor;
                    if (world->mSampleOffset < 0)
                        world->mSampleOffset = 0;
                    else if (world->mSampleOffset >= world->mBufLength)
                        world->mSampleOffset = world->mBufLength - 1;
                    SC_ScheduledEvent event = mScheduler.Remove();
                    event.Perform();
                }
                world->mSampleOffset = 0;
                world->mSubsampleOffset = 0.f;

                World_Run(world);

                for (int k = 0; k < minOutputs; ++k) {
                    float* dst = outputs[k] + pos;
                    if (outTouched[k] == bufCounter)
                        std::memcpy(dst, outBuses + k * bufFrames, sizeof(float) * (size_t)bufFrames);
                    else
                        std::memset(dst, 0, sizeof(float) * (size_t)bufFrames);
                }
                mOSCbuftime = oscTime = nextTime;
            }
        } catch (std::exception& exc) {
            scprintf("SC_HostDriver: exception in real time: %s\n", exc.what());
        } catch (...) {
            scprintf("SC_HostDriver: unknown exception in real time\n");
        }
        mAudioSync.Signal();
    }

protected:
    bool DriverSetup(int* outNumSamples, double* outSampleRate) override
    {
        if (!mPreferredSampleRate || !mPreferredHardwareBufferFrameSize) return false;
        *outSampleRate = mPreferredSampleRate;
        *outNumSamples = (int)mPreferredHardwareBufferFrameSize;
        return true;
    }
    bool DriverStart() override { return true; }
    bool DriverStop() override { return true; }
};

SC_AudioDriver* SC_NewAudioDriver(World* inWorld) { return new SC_HostDriver(inWorld); }

// ---- Concha C API (sc/ConchaSc.h) ---------------------------------------------------------

namespace {
// Replies (/done, /fail, /n_go...) are ignored for now; Concha will route them to its console.
void ignoreReply(ReplyAddress*, char*, int) {}
} // namespace

extern "C" {

ConchaScWorld* concha_sc_new(const ConchaScOptions* o)
{
    WorldOptions options;
    options.mRealTime = true;
    options.mMemoryLocking = false;
    options.mPreferredSampleRate = (uint32)o->sampleRate;
    options.mPreferredHardwareBufferFrameSize = (uint32)o->maxBlockSize;
    options.mBufLength = 64;
    options.mNumOutputBusChannels = (uint32)o->numOutputs;
    options.mNumInputBusChannels = (uint32)o->numInputs;
    options.mNumAudioBusChannels = (uint32)(o->numOutputs + o->numInputs + 1024);
    options.mNumBuffers = 4096;
    options.mMaxNodes = 4096;
    options.mMaxGraphDefs = 2048;
    options.mRealTimeMemorySize = 64 * 1024; // KiB
    options.mNumRGens = 64;
    options.mLoadGraphDefs = 0;
    options.mVerbosity = o->verbosity;
    options.mUGensPluginPath = o->pluginPath;
    options.mSharedMemoryID = 0;
    return reinterpret_cast<ConchaScWorld*>(World_New(&options));
}

void concha_sc_free(ConchaScWorld* w)
{
    if (!w) return;
    World_Cleanup(reinterpret_cast<World*>(w), false);
}

int concha_sc_send(ConchaScWorld* w, const char* packet, int size)
{
    return World_SendPacket(reinterpret_cast<World*>(w), size, const_cast<char*>(packet), &ignoreReply) ? 1 : 0;
}

void concha_sc_process(ConchaScWorld* w, const float* const* inputs, int numInputs, float* const* outputs,
                       int numOutputs, int numSamples, int64_t oscTime)
{
    auto* world = reinterpret_cast<World*>(w);
    static_cast<SC_HostDriver*>(world->hw->mAudioDriver)
        ->Process(inputs, numInputs, outputs, numOutputs, numSamples, (int64)oscTime);
}

int concha_sc_block_size(ConchaScWorld* w) { return reinterpret_cast<World*>(w)->mBufLength; }

int64_t concha_sc_seconds_to_osc(double seconds) { return (int64_t)std::llround(seconds * 4294967296.0); }

} // extern "C"
