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

namespace {
// The plug-in's live values: CC 7 on channel 2 at 0.5, macro 3 at 0.25.
struct ControlHost final : PatternHost {
    bool fetchText(const std::string&, std::string&, std::string& error) override
    {
        error = "no fetch";
        return false;
    }
    std::string libraryMap(const std::string&, std::string&) override { return {}; }
    std::string libraryAutoload() override { return "{\"version\":1,\"names\":[]}"; }
    double control(int kind, int a, int b) override
    {
        if (kind == 0 && a == 7 && b == 2) return 0.5;
        if (kind == 1 && a == 3) return 0.25;
        return 0.0;
    }
};
} // namespace

TEST("chain: MIDI in, midikeys plays a note at once, midin and macro read live values")
{
    ControlHost host;
    SpscRing<MidiInNote> notes{256};
    SpscRing<SchedEvent> ring{16384};
    SeqLock<TransportState> transport;
    SeqLock<SwapPoint> swap;
    PatternWorker::Options o;
    o.lookaheadSeconds = 0.5;
    o.period = std::chrono::milliseconds(1);
    o.engine.host = &host;
    o.midiNotes = &notes;
    PatternWorker worker(ring, transport, swap, o);
    AudioScheduler sched{ring, transport, swap};
    sched.prepare(kSr);
    Recorder rec;
    worker.start();
    worker.requestEvaluate(R"(
const kb = await midikeys('my keyboard')
const cc = await midin('my keyboard')
stack(
  kb().midi(),
  note("c2").ccn(10).ccv(cc(7, 2)).midi(),
  note("d2").ccn(11).ccv(macro(3)).midi()
))");
    bool ready = false;
    for (int i = 0; i < 2000 && !ready; ++i) {
        const auto s = worker.status();
        ready = s.ready && !s.evaluating && s.evaluations >= 1 && s.lastOk && !s.activeCode.empty();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(ready);
    HostPosition h;
    h.bpm = 120.0;
    for (int i = 0; i < 200 && !worker.queuedUpTo(0, 0.4); ++i) { // stopped: the start is prepared
        sched.process(h, 512, 4.0, rec);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    rec.out.clear();
    rec.blockStart = 0;
    h.playing = true;
    const int n = 480; // 0.02 beats per block
    bool pushed = false;
    double notePpq = 0;
    for (int b = 0; b < 400; ++b) {
        sched.process(h, n, 4.0, rec);
        rec.blockStart += n;
        if (b == 100 && !pushed) { // a key pressed in this block: played 10 ms (0.02 beats) later
            const TransportState ts = transport.load();
            MidiInNote k;
            k.epoch = ts.epoch;
            k.lap = ts.lap;
            k.ppq = notePpq = ts.ppq + 0.02 + 0.02;
            k.note = 67;
            k.velocity = 100;
            k.channel = 1;
            pushed = notes.push(k);
            for (int w = 0; w < 500; ++w) std::this_thread::sleep_for(std::chrono::microseconds(200)); // the worker plays it
        }
        h.ppq += n * h.bpm / 60.0 / kSr;
        for (int w = 0; w < 2000 && !worker.queuedUpTo(0, h.ppq + 0.25); ++w) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    worker.stop();
    int64_t keyOn = -1;
    int cc10 = -1, cc11 = -1;
    for (const auto& e : rec.out) {
        if (e.bytes.size() == 3 && (e.bytes[0] & 0xf0) == 0x90 && e.bytes[1] == 67) keyOn = e.sample;
        if (e.bytes.size() == 3 && (e.bytes[0] & 0xf0) == 0xb0 && e.bytes[1] == 10) cc10 = e.bytes[2];
        if (e.bytes.size() == 3 && (e.bytes[0] & 0xf0) == 0xb0 && e.bytes[1] == 11) cc11 = e.bytes[2];
    }
    CHECK(pushed);
    CHECK(keyOn == int64_t(std::llround(notePpq * 24000))); // on its sample, not late
    CHECK(cc10 == 63 || cc10 == 64);                         // cc(7, 2) = 0.5
    CHECK(cc11 == 31 || cc11 == 32);                         // macro(3) = 0.25
    CHECK(sched.stats().late.load() == 0);
}

// ---- MQTT and serial (src/io/ExternalOut) against a local broker and a pseudo-terminal ----
#include "io/ExternalOut.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <string>

namespace {
// One client: (WebSocket upgrade if asked), CONNECT -> CONNACK, then the first PUBLISH.
struct FakeBroker {
    int listenFd = -1, port = 0;
    std::thread thread;
    std::string clientId, topic, payload;
    std::atomic<bool> done{false};
    explicit FakeBroker(bool ws)
    {
        listenFd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listenFd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        listen(listenFd, 1);
        socklen_t len = sizeof a;
        getsockname(listenFd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        thread = std::thread([this, ws] { serve(ws); });
    }
    ~FakeBroker()
    {
        ::shutdown(listenFd, SHUT_RDWR);
        ::close(listenFd);
        if (thread.joinable()) thread.join();
    }
    static bool readN(int fd, uint8_t* p, size_t n)
    {
        size_t got = 0;
        while (got < n) {
            pollfd pf{fd, POLLIN, 0};
            if (poll(&pf, 1, 3000) <= 0) return false;
            const ssize_t r = ::read(fd, p + got, n - got);
            if (r <= 0) return false;
            got += size_t(r);
        }
        return true;
    }
    // one MQTT packet, unwrapping a masked WebSocket frame first when ws
    static bool packet(int fd, bool ws, std::vector<uint8_t>& out)
    {
        std::vector<uint8_t> data;
        if (ws) {
            uint8_t h[2];
            if (!readN(fd, h, 2)) return false;
            uint64_t len = h[1] & 0x7f;
            if (len == 126) {
                uint8_t e[2];
                readN(fd, e, 2);
                len = (e[0] << 8) | e[1];
            }
            uint8_t mask[4];
            readN(fd, mask, 4);
            data.resize(len);
            readN(fd, data.data(), len);
            for (size_t i = 0; i < len; ++i) data[i] ^= mask[i % 4];
        } else {
            uint8_t h;
            if (!readN(fd, &h, 1)) return false;
            data.push_back(h);
            size_t len = 0, mul = 1;
            uint8_t b;
            do {
                readN(fd, &b, 1);
                data.push_back(b);
                len += (b & 0x7f) * mul;
                mul *= 128;
            } while (b & 0x80);
            const size_t at = data.size();
            data.resize(at + len);
            readN(fd, data.data() + at, len);
        }
        out = data;
        return true;
    }
    void serve(bool ws)
    {
        const int fd = accept(listenFd, nullptr, nullptr);
        if (fd < 0) return;
        if (ws) {
            std::string req;
            uint8_t c;
            while (req.find("\r\n\r\n") == std::string::npos && readN(fd, &c, 1)) req += char(c);
            const std::string ok = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                   "Sec-WebSocket-Protocol: mqtt\r\n\r\n";
            ::write(fd, ok.data(), ok.size());
        }
        std::vector<uint8_t> p;
        if (packet(fd, ws, p) && (p[0] & 0xf0) == 0x10) {
            const size_t at = p.size() - 0; // client id: after the 10 bytes of variable header
            size_t i = 1;
            while (p[i] & 0x80) ++i;
            const size_t vh = i + 1 + 10;
            const size_t n = (p[vh] << 8) | p[vh + 1];
            clientId.assign(p.begin() + long(vh + 2), p.begin() + long(vh + 2 + n));
            (void)at;
            const uint8_t ack[4] = {0x20, 0x02, 0x00, 0x00};
            if (ws) {
                const uint8_t frame[6] = {0x82, 0x04, ack[0], ack[1], ack[2], ack[3]};
                ::write(fd, frame, 6);
            } else
                ::write(fd, ack, 4);
            if (packet(fd, ws, p) && (p[0] & 0xf0) == 0x30) {
                size_t k = 1;
                while (p[k] & 0x80) ++k;
                const size_t t = k + 1;
                const size_t tl = (p[t] << 8) | p[t + 1];
                topic.assign(p.begin() + long(t + 2), p.begin() + long(t + 2 + tl));
                payload.assign(p.begin() + long(t + 2 + tl), p.end());
            }
        }
        done = true;
        ::close(fd);
    }
};
} // namespace

TEST("io: MQTT over TCP and over a WebSocket, as Paho sends it")
{
    for (bool ws : {false, true}) {
        FakeBroker broker(ws);
        io::MqttMessage m;
        m.host = (ws ? "ws://127.0.0.1:" : "mqtt://127.0.0.1:") + std::to_string(broker.port) + "/mqtt";
        m.client = ws ? "strudel-ws" : "strudel-tcp";
        m.topic = "/move/robot1";
        m.payload = R"({"move":"forward","duration":0.5,"cps":0.5})";
        io::sendMqtt(m, io::Clock::now());
        for (int i = 0; i < 300 && !broker.done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(broker.clientId == m.client);
        CHECK(broker.topic == m.topic);
        CHECK(broker.payload == m.payload);
    }
    const auto log = io::takeMessages();
    CHECK(log.size() >= 2 && log[0].find("connected") != std::string::npos);
    // a packet: PUBLISH QoS 0, remaining length over 127 bytes takes two bytes
    const auto p = io::mqttPublishPacket("t", std::string(200, 'x'));
    CHECK(p[0] == 0x30 && p[1] == ((203 % 128) | 0x80) && p[2] == 1 && p.size() == 206);
}

TEST("io: serial bytes reach the port (a pseudo-terminal)")
{
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(master >= 0);
    if (master < 0) return;
    grantpt(master);
    unlockpt(master);
    const std::string slave = ptsname(master);
    io::sendSerial(slave, 115200, {'m', 'o', 'v', 'e', '(', ')', 0x7c, 0x12, 0x34, 0x3b}, io::Clock::now());
    std::vector<uint8_t> got;
    for (int i = 0; i < 300 && got.size() < 10; ++i) {
        pollfd pf{master, POLLIN, 0};
        if (poll(&pf, 1, 10) > 0) {
            uint8_t buf[64];
            const ssize_t r = ::read(master, buf, sizeof buf);
            if (r > 0) got.insert(got.end(), buf, buf + r);
        }
    }
    CHECK(got.size() == 10 && got[6] == 0x7c && got[9] == 0x3b);
    ::close(master);
}
