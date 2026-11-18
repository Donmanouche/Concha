#include "sc/ScHost.h"

#include "sc/ScDefs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

namespace concha::sc {

namespace {
// OSC encoding into a preallocated buffer
struct Writer {
    std::vector<char>& b;
    void pad() { while (b.size() % 4) b.push_back('\0'); }
    void str(std::string_view s)
    {
        b.insert(b.end(), s.begin(), s.end());
        b.push_back('\0');
        pad();
    }
    void i32(int32_t v)
    {
        for (int k = 3; k >= 0; --k) b.push_back(char((uint32_t(v) >> (k * 8)) & 0xff));
    }
    void f32(float f)
    {
        int32_t v;
        std::memcpy(&v, &f, 4);
        i32(v);
    }
};
} // namespace

ScHost::ScHost(double sampleRate) : sampleRate_(sampleRate)
{
    for (auto& r : ring_) r.assign(kRing, 0.f);
    block_.assign(size_t(kSlots * 2 * 64), 0.f);
    outs_.resize(kSlots * 2);
    for (int c = 0; c < kSlots * 2; ++c) outs_[size_t(c)] = block_.data() + c * 64;
    msg_.reserve(4096);
    ConchaScOptions o{sampleRate, 64, kSlots * 2, 0, "", 0};
    world_ = concha_sc_new(&o);
    if (!world_) return;
    // the instruments' SynthDefs; /d_recv completes on scsynth's NRT thread, then in the World's
    // block processing: render silent blocks until a /sync comes back
    loadNewDefs();
    // synths free themselves (doneAction) before the voice frees them: no "/n_free: node not found"
    const char quiet[] = {'/', 'e', 'r', 'r', 'o', 'r', 0, 0, ',', 'i', 0, 0, 0, 0, 0, 0};
    concha_sc_send(world_, quiet, int(sizeof(quiet)));
    ready_ = concha_sc_sync(world_, 5000) == 1;
    rendered_ = 0;
}

void ScHost::loadNewDefs()
{
    if (!world_) return;
    std::lock_guard<std::mutex> lock(defsMutex_);
    const int n = scDefCount();
    const std::string* lastFile = nullptr; // a user file of several SynthDefs is sent once
    for (int i = loadedDefs_.load(); i < n; ++i) {
        const ScDef& d = scDef(i);
        if (!d.instrument || d.bytes.empty() || (d.owned && d.owned.get() == lastFile)) continue;
        lastFile = d.owned.get();
        std::vector<char> m;
        Writer w{m};
        w.str("/d_recv");
        w.str(",b");
        w.i32(int32_t(d.bytes.size()));
        m.insert(m.end(), d.bytes.begin(), d.bytes.end());
        w.pad();
        concha_sc_send(world_, m.data(), int(m.size()));
    }
    loadedDefs_.store(n);
}

ScHost::~ScHost()
{
    if (world_) concha_sc_free(world_);
}

int64_t ScHost::oscTime(int64_t sample) const
{
    return concha_sc_seconds_to_osc(double(sample) / sampleRate_);
}

int ScHost::start(int def, const sd::SynthArg* args, int numArgs, int64_t onset, int64_t& readFrom)
{
    if (!ok() || def < 0 || def >= loadedDefs_.load(std::memory_order_acquire)) return -1;
    int slot = -1;
    for (int k = 0; k < kSlots; ++k)
        if (!busy_[size_t(k)]) {
            slot = k;
            break;
        }
    if (slot < 0) return -1;
    const ScDef& d = scDef(def);
    if (trace) trace(onset, d, args, numArgs);
    const int32_t node = nextNode_++;
    if (nextNode_ > 1 << 30) nextNode_ = 1000;
    // /s_new name node addToTail(1) group 0, out <slot bus>, controls...
    msg_.clear();
    Writer w{msg_};
    w.str("/s_new");
    char tags[8 + 2 * 64] = ",siiisf"; // no allocation on the audio thread
    numArgs = std::min(numArgs, 64);
    for (int i = 0; i < numArgs; ++i) std::memcpy(tags + 7 + 2 * i, "if", 3);
    w.str(tags);
    w.str(d.name);
    w.i32(node);
    w.i32(1);
    w.i32(0);
    w.str("out");
    w.f32(float(slot * 2));
    for (int i = 0; i < numArgs; ++i) {
        w.i32(args[i].control);
        w.f32(args[i].value);
    }
    // a synth asked for a time the World has already rendered starts with its next block
    const int64_t at = std::max(onset, rendered_);
    if (!concha_sc_queue_rt(world_, msg_.data(), int(msg_.size()), oscTime(at))) return -1;
    busy_[size_t(slot)] = true;
    node_[size_t(slot)] = node;
    readFrom = (at / 64) * 64; // Out.ar: from the start of its block
    return slot;
}

void ScHost::stop(int slot)
{
    if (slot < 0 || slot >= kSlots || !busy_[size_t(slot)]) return;
    msg_.clear();
    Writer w{msg_};
    w.str("/n_free");
    w.str(",i");
    w.i32(node_[size_t(slot)]);
    concha_sc_queue_rt(world_, msg_.data(), int(msg_.size()), oscTime(rendered_));
    busy_[size_t(slot)] = false;
}

void ScHost::processBlock()
{
    concha_sc_process(world_, nullptr, 0, outs_.data(), kSlots * 2, 64, oscTime(rendered_));
    const size_t at = size_t(rendered_ & (kRing - 1)); // kRing is a multiple of 64
    for (int c = 0; c < kSlots * 2; ++c) std::memcpy(ring_[size_t(c)].data() + at, outs_[size_t(c)], 64 * sizeof(float));
    rendered_ += 64;
}

void ScHost::renderTo(int64_t end)
{
    if (!ok()) return;
    while (rendered_ < end) processBlock();
}

} // namespace concha::sc
