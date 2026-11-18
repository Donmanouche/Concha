// Phase 0 spike: two scsynth Worlds in one process, run from one thread as two plug-in
// instances would be, each fed sample-clock time-tagged bundles.
// Checks: both start, the SynthDef loads in both, each note starts on the expected sample and
// has its own frequency, and the two Worlds do not leak into each other.
// usage: spike_sc_two_worlds <concha_ping.scsyndef> [plugin dir]

#include "sc/ConchaSc.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// Minimal OSC encoder (big-endian, 4-byte padding).
struct Osc {
    std::string b;
    void pad() { while (b.size() % 4) b.push_back('\0'); }
    void str(const std::string& s) { b += s; b.push_back('\0'); pad(); }
    void i32(int32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(char((uint32_t(v) >> s) & 0xff)); }
    void f32(float v) { int32_t i; std::memcpy(&i, &v, 4); i32(i); }
    void blob(const std::string& d) { i32(int32_t(d.size())); b += d; pad(); }
    void u64(uint64_t v) { for (int s = 56; s >= 0; s -= 8) b.push_back(char((v >> s) & 0xff)); }
};

std::string message(const std::string& addr, const std::string& tags, auto&&... args)
{
    Osc m;
    m.str(addr);
    m.str("," + tags);
    (
        [&](auto&& a) {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, int>) m.i32(a);
            else if constexpr (std::is_same_v<T, float>) m.f32(a);
            else if constexpr (std::is_same_v<T, std::string>) {
                if (tags.find('b') != std::string::npos && a.size() > 64) m.blob(a); else m.str(a);
            } else m.str(std::string(a));
        }(args),
        ...);
    return m.b;
}

std::string bundle(int64_t oscTime, const std::string& msg)
{
    Osc b;
    b.str("#bundle");
    b.u64(uint64_t(oscTime));
    b.i32(int32_t(msg.size()));
    b.b += msg;
    return b.b;
}

std::string readFile(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Instance {
    ConchaScWorld* world = nullptr;
    std::vector<float> left, right;
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <concha_ping.scsyndef> [plugin dir]\n", argv[0]);
        return 2;
    }
    const std::string def = readFile(argv[1]);
    constexpr double sr = 48000.0;
    constexpr int block = 512;

    ConchaScOptions opt{};
    opt.sampleRate = sr;
    opt.maxBlockSize = block;
    opt.numOutputs = 2;
    opt.numInputs = 0;
    opt.pluginPath = argc > 2 ? argv[2] : "/usr/lib/SuperCollider/plugins";
    opt.verbosity = 1;

    Instance a, b;
    a.world = concha_sc_new(&opt);
    b.world = concha_sc_new(&opt);
    if (!a.world || !b.world) { std::fprintf(stderr, "World_New failed\n"); return 1; }
    std::printf("two Worlds created, block size %d\n", concha_sc_block_size(a.world));

    // /d_recv is asynchronous (NRT thread): run some silent blocks while it completes.
    const std::string recv = message("/d_recv", "b", def);
    concha_sc_send(a.world, recv.data(), int(recv.size()));
    concha_sc_send(b.world, recv.data(), int(recv.size()));

    int64_t sampleClock = 0;
    float silenceL[block], silenceR[block];
    auto runBlock = [&](Instance& inst, bool keep) {
        float* outs[2] = {silenceL, silenceR};
        concha_sc_process(inst.world, nullptr, 0, outs, 2, block, concha_sc_seconds_to_osc(sampleClock / sr));
        if (keep) {
            inst.left.insert(inst.left.end(), silenceL, silenceL + block);
            inst.right.insert(inst.right.end(), silenceR, silenceR + block);
        }
    };
    for (int i = 0; i < 20; ++i) {
        runBlock(a, false);
        runBlock(b, false);
        sampleClock += block;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Notes: A at 0.5 s (440 Hz), B at 0.25 s (660 Hz), relative to the start of recording.
    const int64_t recordStart = sampleClock;
    const int64_t onsetA = recordStart + 24000, onsetB = recordStart + 12000;
    const std::string na = bundle(concha_sc_seconds_to_osc(onsetA / sr),
                                  message("/s_new", "siiisf", "concha_ping", -1, 0, 0, "freq", 440.0f));
    const std::string nb = bundle(concha_sc_seconds_to_osc(onsetB / sr),
                                  message("/s_new", "siiisf", "concha_ping", -1, 0, 0, "freq", 660.0f));
    concha_sc_send(a.world, na.data(), int(na.size()));
    concha_sc_send(b.world, nb.data(), int(nb.size()));

    const auto t0 = std::chrono::steady_clock::now();
    const int blocks = int(sr * 1.5) / block;
    for (int i = 0; i < blocks; ++i) {
        runBlock(a, true);
        runBlock(b, true);
        sampleClock += block;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    auto report = [&](const char* name, const Instance& inst, int64_t expectedOnset, double expectedFreq) {
        int first = -1;
        for (size_t i = 0; i < inst.left.size(); ++i)
            if (std::fabs(inst.left[i]) > 1e-6f) { first = int(i); break; }
        int crossings = 0;
        const int from = first, to = std::min<int>(first + 4800, int(inst.left.size())); // first 100 ms
        for (int i = from + 1; first >= 0 && i < to; ++i)
            if ((inst.left[size_t(i - 1)] < 0.f) != (inst.left[size_t(i)] < 0.f)) ++crossings;
        const double freq = crossings / 2.0 / 0.1;
        float peak = 0.f;
        for (float s : inst.left) peak = std::max(peak, std::fabs(s));
        const int expected = int(expectedOnset - recordStart);
        std::printf("%s: onset at sample %d (expected %d, %+d), ~%.0f Hz (expected %.0f), peak %.3f\n", name, first,
                    expected, first - expected, freq, expectedFreq, peak);
        return first >= 0 && std::abs(first - expected) <= 1 && std::fabs(freq - expectedFreq) < 15.0;
    };
    const bool okA = report("World A", a, onsetA, 440.0);
    const bool okB = report("World B", b, onsetB, 660.0);
    std::printf("rendered 2 x %.1f s in %.1f ms\n", blocks * block / sr, ms);

    concha_sc_free(a.world);
    concha_sc_free(b.world);
    std::printf("%s\n", okA && okB ? "PASS" : "FAIL");
    std::fflush(stdout);
    // The system UIUGens.so starts a mouse thread that aborts in its static destructor: skip
    // the exit handlers. (Concha will not load UIUGens.)
    std::_Exit(okA && okB ? 0 : 1);
}
