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
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <vector>

int32 server_timeseed() { return timeSeed(); }
int64 oscTimeNow() { return OSCTime(getTime()); }
void initializeScheduler() {}

int PerformOSCMessage(World* inWorld, int inSize, char* inData, ReplyAddress* inReply);
namespace {
void ignoreReply(ReplyAddress*, char*, int) {}
}

class SC_HostDriver final : public SC_AudioDriver {
public:
    explicit SC_HostDriver(World* world) : SC_AudioDriver(world)
    {
        mRtReply.mReplyFunc = &ignoreReply;
        mRtReply.mReplyData = nullptr;
        mRtReply.mSocket = 0;
    }

    // Messages from the audio thread (the one calling Process), run at their sample: a fixed queue,
    // no allocation (World_SendPacket mallocs).
    static constexpr int kRtQueue = 512, kRtMsgBytes = 2048;
    struct RtMsg {
        int64 time = 0;
        uint64_t seq = 0; // same time: in the order queued
        int size = 0;
        bool used = false;
        char data[kRtMsgBytes];
    };
    bool QueueRt(const char* data, int size, int64 time)
    {
        if (size <= 0 || size > kRtMsgBytes) return false;
        for (auto& m : mRt) {
            if (m.used) continue;
            std::memcpy(m.data, data, (size_t)size);
            m.size = size;
            m.time = time;
            m.seq = mRtSeq++;
            m.used = true;
            ++mRtPending;
            return true;
        }
        return false;
    }

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
                while (mRtPending > 0) { // the audio thread's messages due in this block, in time order
                    RtMsg* next = nullptr;
                    for (auto& m : mRt)
                        if (m.used && m.time < nextTime &&
                            (!next || m.time < next->time || (m.time == next->time && m.seq < next->seq)))
                            next = &m;
                    if (!next) break;
                    const float diff = (float)(next->time - oscTime) * (float)oscToSamples + 0.5f;
                    world->mSampleOffset = sc_clip((int)std::floor(diff), 0, world->mBufLength - 1);
                    world->mSubsampleOffset = 0.f;
                    PerformOSCMessage(world, next->size, next->data, &mRtReply);
                    next->used = false;
                    --mRtPending;
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
        // the non-real-time thread only when it has something to do (or now and then): signalling it
        // after every 64-sample block woke three threads 750 times a second each for nothing
        HiddenWorld* hw = world->hw;
        if (mFromEngine.HasData() || hw->mTriggers.HasData() || hw->mNodeMsgs.HasData() || hw->mNodeEnds.HasData() ||
            hw->mDeleteGraphDefs.HasData() || ++mIdleSignals >= 64) {
            mIdleSignals = 0;
            mAudioSync.Signal();
        }
    }

protected:
    int mIdleSignals = 0;
    std::vector<RtMsg> mRt = std::vector<RtMsg>(kRtQueue);
    int mRtPending = 0;
    uint64_t mRtSeq = 0;
    ReplyAddress mRtReply;

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

// UGen plug-ins compiled in (STATIC_PLUGINS) that scsynth's initialize_library() does not load
// itself: the rest of SuperCollider's own, and the sc3-plugins families (cmake/ScSynth.cmake).
extern InterfaceTable gInterfaceTable;
void Chaos_Load(InterfaceTable*);
void DemoUGens_Load(InterfaceTable*);
void UnpackFFTUGens_Load(InterfaceTable*);
void PV_ThirdParty_Load(InterfaceTable*);
void ML_UGens_Load(InterfaceTable*);
void MCLDDistortion_Load(InterfaceTable*);
void Membrane_Load(InterfaceTable*);
void Mda_Load(InterfaceTable*);
void FM7_Load(InterfaceTable*);
void AY_Load(InterfaceTable*);
void Ladspa_Load(InterfaceTable*);
void Blackrain_Load(InterfaceTable*);
void MCLDOsc_Load(InterfaceTable*);
void JoshPV_Load(InterfaceTable*);
void ConchaUI_Load(InterfaceTable*);

namespace {
void loadExtraUGens()
{
    static bool loaded = false; // the unit definitions are global, once per process
    if (loaded) return;
    loaded = true;
    for (auto load : {Chaos_Load, DemoUGens_Load, UnpackFFTUGens_Load, PV_ThirdParty_Load, ML_UGens_Load,
                      MCLDDistortion_Load, Membrane_Load, Mda_Load, FM7_Load, AY_Load, Ladspa_Load, Blackrain_Load,
                      MCLDOsc_Load, JoshPV_Load, ConchaUI_Load})
        load(&gInterfaceTable);
}
} // namespace

// ---- Concha C API (sc/ConchaSc.h) ---------------------------------------------------------

// Replies (/done, /fail, /n_go...) are ignored (ignoreReply).

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
    options.mMaxWireBufs = 512; // StrudelDirt's superprimes needs more than the default 64
    options.mLoadGraphDefs = 0;
    options.mVerbosity = o->verbosity;
    options.mUGensPluginPath = o->pluginPath;
    options.mSharedMemoryID = 0;
    World* world = World_New(&options); // the first one runs initialize_library() (core UGens)
    if (world) loadExtraUGens();
    return reinterpret_cast<ConchaScWorld*>(world);
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

int concha_sc_sync(ConchaScWorld* w, int timeoutMs)
{
    // /sync answers /synced once every earlier asynchronous command (/d_recv...) is done; the World
    // must run (silent blocks) for their last stage, which happens in its block processing
    auto* world = reinterpret_cast<World*>(w);
    std::atomic<bool> done{false};
    ReplyFunc onReply = [](ReplyAddress* addr, char* msg, int size) {
        if (size >= 7 && std::strncmp(msg, "/synced", 7) == 0)
            static_cast<std::atomic<bool>*>(addr->mReplyData)->store(true);
    };
    char packet[16] = {'/', 's', 'y', 'n', 'c', 0, 0, 0, ',', 'i', 0, 0, 0, 0, 0, 1};
    if (!World_SendPacketWithContext(world, (int)sizeof(packet), packet, onReply, &done)) return 0;
    auto* driver = static_cast<SC_HostDriver*>(world->hw->mAudioDriver);
    int64 t = 0;
    for (int ms = 0; ms < timeoutMs && !done.load(); ++ms) {
        driver->Process(nullptr, 0, nullptr, 0, world->mBufLength, t);
        t += (int64)std::llround(world->mBufLength / world->mSampleRate * 4294967296.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done.load() ? 1 : 0;
}

int concha_sc_queue_rt(ConchaScWorld* w, const char* message, int size, int64_t oscTime)
{
    auto* world = reinterpret_cast<World*>(w);
    return static_cast<SC_HostDriver*>(world->hw->mAudioDriver)->QueueRt(message, size, (int64)oscTime) ? 1 : 0;
}

int concha_sc_block_size(ConchaScWorld* w) { return reinterpret_cast<World*>(w)->mBufLength; }

int64_t concha_sc_seconds_to_osc(double seconds) { return (int64_t)std::llround(seconds * 4294967296.0); }

} // extern "C"
