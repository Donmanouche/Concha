#include "sc/ScOrbitFx.h"

#include "sc/ScDefs.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace concha::sc {

namespace {
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
const char* const kDefs[] = {"dirt_leslie2", "dirt_shimmer2"};
} // namespace

ScOrbitFx::ScOrbitFx(double sampleRate) : sampleRate_(sampleRate)
{
    for (auto& r : in_) r.assign(kRing, 0.f);
    for (auto& r : out_) r.assign(kRing, 0.f);
    outBlock_.assign(size_t(kOrbits * 2 * 64), 0.f);
    outs_.resize(kOrbits * 2);
    ins_.resize(kOrbits * 2);
    for (int c = 0; c < kOrbits * 2; ++c) outs_[size_t(c)] = outBlock_.data() + c * 64;
    msg_.reserve(1024);
    ConchaScOptions o{sampleRate, 64, kOrbits * 2, kOrbits * 2, "", 0};
    world_ = concha_sc_new(&o);
    if (!world_) return;
    for (const char* name : kDefs) {
        const int i = scDefIndex(name, true);
        if (i < 0) continue;
        const auto& d = scDefs()[size_t(i)];
        std::vector<char> m;
        Writer w{m};
        w.str("/d_recv");
        w.str(",b");
        w.i32(int32_t(d.bytes.size()));
        m.insert(m.end(), d.bytes.begin(), d.bytes.end());
        w.pad();
        concha_sc_send(world_, m.data(), int(m.size()));
    }
    const char quiet[] = {'/', 'e', 'r', 'r', 'o', 'r', 0, 0, ',', 'i', 0, 0, 0, 0, 0, 0};
    concha_sc_send(world_, quiet, int(sizeof(quiet)));
    ready_ = concha_sc_sync(world_, 5000) == 1;
}

ScOrbitFx::~ScOrbitFx()
{
    if (world_) concha_sc_free(world_);
}

int64_t ScOrbitFx::oscTime(int64_t sample) const { return concha_sc_seconds_to_osc(double(sample) / sampleRate_); }

void ScOrbitFx::set(int orbit, Effect effect, const char* const* names, const float* values, int count, int64_t at)
{
    if (!ok() || orbit < 0 || orbit >= kOrbits) return;
    at = std::max(at, processed_);
    int32_t& node = node_[size_t(orbit)][size_t(effect)];
    msg_.clear();
    Writer w{msg_};
    char tags[10 + 2 * 8]; // no allocation on the audio thread
    count = std::min(count, 8);
    if (node == 0) { // GlobalDirtEffect.play: dryBus / effectBus of the orbit, its state
        node = nextNode_++;
        w.str("/s_new");
        std::memcpy(tags, ",siiisfsf", 10);
        for (int i = 0; i < count; ++i) std::memcpy(tags + 9 + 2 * i, "sf", 3);
        w.str(tags);
        w.str(kDefs[effect]);
        w.i32(node);
        w.i32(1); // addToTail
        w.i32(0);
        w.str("dryBus");
        w.f32(float(kOrbits * 2 + orbit * 2)); // the orbit's input pair
        w.str("effectBus");
        w.f32(float(orbit * 2));
    } else { // GlobalDirtEffect.set: the changed controls
        w.str("/n_set");
        std::memcpy(tags, ",i", 3);
        for (int i = 0; i < count; ++i) std::memcpy(tags + 2 + 2 * i, "sf", 3);
        w.str(tags);
        w.i32(node);
    }
    for (int i = 0; i < count; ++i) {
        w.str(names[i]);
        w.f32(values[i]);
    }
    concha_sc_queue_rt(world_, msg_.data(), int(msg_.size()), oscTime(at));
}

void ScOrbitFx::reset()
{
    for (auto& n : node_)
        for (auto& node : n) {
            if (node == 0) continue;
            msg_.clear();
            Writer w{msg_};
            w.str("/n_free");
            w.str(",i");
            w.i32(node);
            concha_sc_queue_rt(world_, msg_.data(), int(msg_.size()), oscTime(processed_));
            node = 0;
        }
}

void ScOrbitFx::processTo(int64_t end)
{
    if (!ok()) return;
    while (processed_ + 64 <= end) {
        const size_t at = size_t(processed_ & (kRing - 1));
        for (int c = 0; c < kOrbits * 2; ++c) ins_[size_t(c)] = in_[size_t(c)].data() + at;
        concha_sc_process(world_, ins_.data(), kOrbits * 2, outs_.data(), kOrbits * 2, 64, oscTime(processed_));
        for (int c = 0; c < kOrbits * 2; ++c) {
            std::memcpy(out_[size_t(c)].data() + at, outs_[size_t(c)], 64 * sizeof(float));
            std::memset(in_[size_t(c)].data() + at, 0, 64 * sizeof(float));
        }
        processed_ += 64;
    }
}

} // namespace concha::sc
