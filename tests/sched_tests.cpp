// Scheduler: AudioScheduler alone (events injected by hand), then the whole chain
// PatternWorker (Strudel in QuickJS) -> AudioScheduler on a simulated DAW transport.
#include "check.h"
#include "sched/AudioScheduler.h"
#include "sched/PatternWorker.h"

#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

using namespace concha;

namespace {

constexpr double kSr = 48000.0;

struct Captured {
    int64_t sample; // absolute sample position (block start + offset)
    std::vector<uint8_t> bytes;
    bool sound = false;
};

struct Recorder final : AudioScheduler::Sink {
    std::vector<Captured> out;
    int64_t blockStart = 0;
    void midi(int offset, const uint8_t* b, int n) override { out.push_back({blockStart + offset, {b, b + n}, false}); }
    void sound(int offset, const SchedEvent&, double) override { out.push_back({blockStart + offset, {}, true}); }
};

struct Rig {
    SpscRing<SchedEvent> ring{4096};
    SeqLock<TransportState> transport;
    SeqLock<SwapPoint> swap;
    AudioScheduler sched{ring, transport, swap};
    Recorder rec;
    Rig() { sched.prepare(kSr); }
    void block(const HostPosition& h, int n)
    {
        sched.process(h, n, 4.0, rec);
        rec.blockStart += n;
    }
};

SchedEvent midiEvent(uint32_t epoch, int32_t lap, double ppq, uint8_t status, uint8_t d1, uint8_t d2, uint32_t gen = 0)
{
    SchedEvent e;
    e.type = SchedEvent::Type::Midi;
    e.epoch = epoch;
    e.lap = lap;
    e.ppq = ppq;
    e.codeGeneration = gen;
    e.midi[0] = status;
    e.midi[1] = d1;
    e.midi[2] = d2;
    e.midiLength = 3;
    return e;
}

HostPosition at(double ppq, double bpm = 120.0, bool playing = true)
{
    HostPosition h;
    h.playing = playing;
    h.ppq = ppq;
    h.bpm = bpm;
    return h;
}

} // namespace

TEST("scheduler: events land on their exact sample")
{
    Rig r;
    r.block(at(0.0), 512); // first playing block: epoch 1
    const uint32_t ep = r.sched.epoch();
    // 120 BPM, 48 kHz: one beat = 24000 samples. Block 2 starts at 512 samples = 512/24000 beats.
    r.ring.push(midiEvent(ep, 0, 1.0, 0x90, 60, 100));  // beat 1 -> sample 24000
    r.ring.push(midiEvent(ep, 0, 0.5, 0x90, 62, 100));  // sample 12000
    double ppq = 512 / 24000.0;
    for (int i = 0; i < 100; ++i) {
        r.block(at(ppq), 512);
        ppq += 512 / 24000.0;
    }
    CHECK(r.rec.out.size() == 2);
    if (r.rec.out.size() == 2) {
        CHECK(r.rec.out[0].sample == 12000);
        CHECK(r.rec.out[1].sample == 24000);
    }
}

TEST("scheduler: tempo change moves the samples, not the beats")
{
    Rig r;
    r.block(at(0.0, 120.0), 480);
    const uint32_t ep = r.sched.epoch();
    r.ring.push(midiEvent(ep, 0, 1.0, 0x90, 60, 100));
    // first 480 samples at 120 BPM = 0.02 beats; then 60 BPM: 1 beat = 48000 samples.
    double ppq = 0.02;
    for (int i = 0; i < 200 && r.rec.out.empty(); ++i) {
        r.block(at(ppq, 60.0), 480);
        ppq += 480 / 48000.0;
    }
    CHECK(r.rec.out.size() == 1);
    if (!r.rec.out.empty()) {
        CHECK(r.rec.out[0].sample == 480 + int64_t(std::floor(0.98 * 48000.0 + 1e-6)));
    }
}

TEST("scheduler: a block crossing the loop end is split, next lap continues")
{
    Rig r;
    HostPosition h = at(3.9);
    h.looping = true;
    h.loopStart = 0.0;
    h.loopEnd = 4.0;
    r.block(h, 480); // 3.9 .. 3.92
    const uint32_t ep = r.sched.epoch();
    r.ring.push(midiEvent(ep, 1, 0.01, 0x90, 64, 100)); // lap 1, just after the loop start
    // Next block: 3.92 + 0.2 beats crosses 4.0 after 0.08 beats (1920 samples) -> event at 0.01 beat in
    // lap 1 is 1920 + 240 samples into the block.
    h.ppq = 3.92;
    r.block(h, 4800);
    CHECK(r.rec.out.size() == 1);
    if (!r.rec.out.empty()) CHECK(r.rec.out[0].sample == 480 + 1920 + 240);
    // The host then reports the wrapped position: continuous, no jump.
    const auto jumps = r.sched.stats().jumps.load();
    h.ppq = 0.12;
    r.block(h, 480);
    CHECK(r.sched.stats().jumps.load() == jumps);
    CHECK(r.sched.lap() == 1);
}

TEST("scheduler: host that wraps at block boundaries")
{
    Rig r;
    HostPosition h = at(3.98);
    h.looping = true;
    h.loopEnd = 4.0;
    r.block(h, 480);           // ends exactly at 4.0
    h.ppq = 0.0;               // host restarts the loop on the next block
    r.block(h, 480);
    CHECK(r.sched.stats().jumps.load() == 1); // only the initial start
    CHECK(r.sched.lap() == 1);
}

TEST("scheduler: a jump drops queued events and releases notes")
{
    Rig r;
    r.block(at(0.0), 480);
    const uint32_t ep = r.sched.epoch();
    r.ring.push(midiEvent(ep, 0, 0.01, 0x90, 60, 100)); // note on inside block 2
    r.ring.push(midiEvent(ep, 0, 2.0, 0x90, 61, 100));  // later
    r.block(at(0.02), 480);
    CHECK(r.rec.out.size() == 1);
    r.rec.out.clear();
    r.block(at(10.0), 480); // jump
    CHECK(r.sched.epoch() == ep + 1);
    // note 60 was on: it is released at the jump
    bool released = false;
    for (const auto& c : r.rec.out) released |= c.bytes.size() == 3 && c.bytes[0] == 0x80 && c.bytes[1] == 60;
    CHECK(released);
    r.rec.out.clear();
    for (int i = 0; i < 300; ++i) r.block(at(10.0 + 0.02 * (i + 1)), 480);
    CHECK(r.rec.out.empty()); // the old event at beat 2 never plays
}

TEST("scheduler: same sample, note-off before note-on")
{
    Rig r;
    r.block(at(0.0), 480);
    const uint32_t ep = r.sched.epoch();
    r.ring.push(midiEvent(ep, 0, 0.03, 0x90, 60, 100));
    r.ring.push(midiEvent(ep, 0, 0.03, 0x80, 60, 0));
    r.block(at(0.02), 480);
    CHECK(r.rec.out.size() == 2);
    if (r.rec.out.size() == 2) {
        CHECK(r.rec.out[0].bytes[0] == 0x80);
        CHECK(r.rec.out[1].bytes[0] == 0x90);
    }
}

TEST("scheduler: new code replaces the old from the swap point, note-offs kept")
{
    Rig r;
    r.block(at(0.0), 480);
    const uint32_t ep = r.sched.epoch();
    r.ring.push(midiEvent(ep, 0, 0.5, 0x90, 60, 100, 0)); // old code, after the swap point
    r.ring.push(midiEvent(ep, 0, 0.6, 0x80, 61, 0, 0));   // old note-off: kept
    r.ring.push(midiEvent(ep, 0, 0.7, 0x90, 70, 100, 1)); // new code
    SwapPoint sp;
    sp.codeGeneration = 1;
    sp.epoch = ep;
    sp.lap = 0;
    sp.ppq = 0.4;
    r.swap.store(sp);
    double ppq = 0.02;
    for (int i = 0; i < 60; ++i) {
        r.block(at(ppq), 480);
        ppq += 0.02;
    }
    CHECK(r.rec.out.size() == 2);
    if (r.rec.out.size() == 2) {
        CHECK(r.rec.out[0].bytes[1] == 61);
        CHECK(r.rec.out[1].bytes[1] == 70);
    }
}

TEST("scheduler: late events within tolerance play at sample 0, older ones are dropped")
{
    Rig r;
    r.block(at(0.0), 480);
    r.block(at(0.02), 480);
    const uint32_t ep = r.sched.epoch();
    r.ring.push(midiEvent(ep, 0, 0.035, 0x90, 60, 100)); // 0.005 beats late
    r.ring.push(midiEvent(ep, 0, 0.0, 0x90, 61, 100));   // 0.04 beats late
    r.block(at(0.04), 480);
    CHECK(r.rec.out.size() == 1);
    if (!r.rec.out.empty()) CHECK(r.rec.out[0].bytes[1] == 60);
    CHECK(r.sched.stats().late.load() == 1);
}

// ---- whole chain: Strudel -> pattern thread -> audio thread ----

namespace {
struct Chain {
    SpscRing<SchedEvent> ring{16384};
    SeqLock<TransportState> transport;
    SeqLock<SwapPoint> swap;
    PatternWorker worker;
    AudioScheduler sched{ring, transport, swap};
    Recorder rec;
    static PatternWorker::Options options()
    {
        PatternWorker::Options o;
        o.lookaheadSeconds = 0.5;
        o.period = std::chrono::milliseconds(1);
        return o;
    }
    Chain() : worker(ring, transport, swap, options()) { sched.prepare(kSr); }

    // Evaluated, and the new code has taken over (the swap happens on the pattern thread).
    bool waitReady(uint64_t evaluations = 1)
    {
        for (int i = 0; i < 2000; ++i) {
            const auto s = worker.status();
            if (s.ready && !s.evaluating && s.evaluations >= evaluations) {
                if (!s.lastOk) return false;
                if (!s.activeCode.empty()) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
    // A DAW runs the plug-in stopped before playing: the start gets prepared.
    void prepareStart(HostPosition h)
    {
        h.playing = false;
        for (int i = 0; i < 1000 && !worker.queuedUpTo(0, h.ppq + 0.4); ++i) {
            sched.process(h, 512, 4.0, rec);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        rec.out.clear();
        rec.blockStart = 0;
    }
    // Runs the simulated DAW faster than real time, but each block waits until the pattern thread
    // has queued its events (as real time would give it time to).
    void play(HostPosition h, int blocks, int n)
    {
        for (int i = 0; i < blocks; ++i) {
            sched.process(h, n, 4.0, rec);
            rec.blockStart += n;
            const double next = h.ppq + n * h.bpm / 60.0 / kSr;
            int32_t lap = sched.lap();
            double target = next;
            if (h.looping && next >= h.loopEnd) {
                target = h.loopStart + (next - h.loopEnd);
                ++lap;
            }
            for (int w = 0; w < 2000 && !worker.queuedUpTo(lap, target + 0.25); ++w)
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            h.ppq = target;
        }
    }
};
} // namespace

TEST("chain: Strudel MIDI notes on the exact samples, across DAW loops")
{
    Chain c;
    c.worker.start();
    c.worker.requestEvaluate(R"(note("c4 d4 e4 f4").midi())"); // 1 cycle = 4 beats: one note per beat
    CHECK(c.waitReady());
    HostPosition h;
    h.playing = true;
    h.bpm = 120.0;
    h.looping = true;
    h.loopStart = 0.0;
    h.loopEnd = 4.0;
    c.prepareStart(h);
    // 120 BPM at 48 kHz: 24000 samples per beat. 3 laps of 4 beats = 288000 samples; blocks of 500 do
    // not divide a beat, so blocks cross beats and the loop end at arbitrary places.
    c.play(h, 288000 / 500, 500);
    c.worker.stop();

    std::vector<int64_t> ons;
    std::vector<int> pitches;
    for (const auto& e : c.rec.out)
        if (e.bytes.size() == 3 && (e.bytes[0] & 0xf0) == 0x90) {
            ons.push_back(e.sample);
            pitches.push_back(e.bytes[1]);
        }
    static const int expected[4] = {60, 62, 64, 65};
    CHECK(ons.size() == 12);
    for (size_t i = 0; i < ons.size(); ++i) {
        CHECK(ons[i] == int64_t(i) * 24000);
        CHECK(pitches[i] == expected[i % 4]);
    }
    CHECK(c.sched.stats().late.load() == 0);
    CHECK(c.sched.stats().jumps.load() == 1); // the start only: the loops are continuous
}

TEST("chain: CC, program change, pitch bend and their timing")
{
    Chain c;
    c.worker.start();
    c.worker.requestEvaluate(R"(ccv("0 0.5 1").ccn(74).progNum(3).midichan(2).midi())");
    CHECK(c.waitReady());
    HostPosition h;
    h.playing = true;
    h.bpm = 90.0; // 32000 samples per beat; a third of a 4-beat cycle = 4/3 beats
    c.prepareStart(h);
    c.play(h, 140, 1024);
    c.worker.stop();
    std::vector<Captured> cc;
    int pcs = 0;
    for (const auto& e : c.rec.out) {
        if (e.bytes.size() == 3 && e.bytes[0] == 0xb1) cc.push_back(e);
        if (e.bytes.size() == 2 && e.bytes[0] == 0xc1 && e.bytes[1] == 3) ++pcs;
    }
    CHECK(cc.size() >= 3);
    if (cc.size() >= 3) {
        CHECK(cc[0].bytes[1] == 74 && cc[0].bytes[2] == 0);
        CHECK(cc[1].bytes[2] == 64 && cc[2].bytes[2] == 127);
        CHECK(cc[1].sample == int64_t(std::floor(4.0 / 3.0 * 32000.0)));
        CHECK(cc[2].sample == int64_t(std::floor(8.0 / 3.0 * 32000.0)));
    }
    CHECK(pcs >= 3);
}

TEST("chain: a slow evaluation does not interrupt the music")
{
    Chain c;
    c.worker.start();
    c.worker.requestEvaluate(R"(note("c4*4").midi())");
    CHECK(c.waitReady());
    HostPosition h;
    h.playing = true;
    h.bpm = 120.0;
    c.prepareStart(h);
    // New code that takes ~300 ms to evaluate, requested while playing.
    c.worker.requestEvaluate("const t0 = Date.now(); while (Date.now() - t0 < 300) {}\nnote(\"e4*4\").midi()");
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(150)) { // during the evaluation
        c.play(h, 4, 480);
        h.ppq = c.rec.blockStart * 2.0 / kSr;
    }
    for (int i = 0; i < 2000 && c.worker.status().evaluating; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    c.play(h, 300, 480);
    c.worker.stop();
    int oldNotes = 0, newNotes = 0;
    for (const auto& e : c.rec.out)
        if (e.bytes.size() == 3 && (e.bytes[0] & 0xf0) == 0x90) (e.bytes[1] == 60 ? oldNotes : newNotes)++;
    CHECK(oldNotes > 0);  // the old code kept playing during the evaluation
    CHECK(newNotes > 0);  // then the new one took over
    CHECK(c.sched.stats().late.load() == 0);
}

TEST("chain: start from a stopped playhead in the middle of the song")
{
    Chain c;
    c.worker.start();
    c.worker.requestEvaluate(R"(note("c4 d4 e4 f4").midi())");
    CHECK(c.waitReady());
    HostPosition h;
    h.playing = false;
    h.bpm = 120.0;
    h.ppq = 9.0; // playhead parked on beat 9 (third cycle, second note: d4)
    for (int i = 0; i < 400 && !c.worker.queuedUpTo(0, 9.5); ++i) { // stopped: the start is prepared
        c.sched.process(h, 512, 4.0, c.rec);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    c.rec.out.clear();
    c.rec.blockStart = 0;
    h.playing = true;
    c.play(h, 100, 512); // 51200 samples: beats 9 to 11.13
    c.worker.stop();
    std::vector<std::pair<int64_t, int>> ons;
    for (const auto& e : c.rec.out)
        if (e.bytes.size() == 3 && (e.bytes[0] & 0xf0) == 0x90) ons.emplace_back(e.sample, e.bytes[1]);
    CHECK(ons.size() == 3);
    if (ons.size() == 3) {
        CHECK(ons[0].first == 0 && ons[0].second == 62);     // the very first sample
        CHECK(ons[1].first == 24000 && ons[1].second == 64);
        CHECK(ons[2].first == 48000 && ons[2].second == 65);
    }
    CHECK(c.sched.stats().late.load() == 0);
}

int main() { return RUN_TESTS(); }
