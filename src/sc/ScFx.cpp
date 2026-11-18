#include "sc/ScFx.h"

#include "sc/ScDefs.h"

#include <algorithm>
#include <cstring>

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
} // namespace

ScFx::ScFx(double sampleRate) : sampleRate_(sampleRate)
{
    for (auto& r : in_) r.assign(kRing, 0.f);
    for (auto& r : out_) r.assign(kRing, 0.f);
    outBlock_.assign(size_t(kSlots * 2 * 64), 0.f);
    outs_.resize(kSlots * 2);
    ins_.resize(kSlots * 2);
    for (int c = 0; c < kSlots * 2; ++c) outs_[size_t(c)] = outBlock_.data() + c * 64;
    msg_.reserve(4096);
    ConchaScOptions o{sampleRate, 64, kSlots * 2, kSlots * 2, "", 0};
    world_ = concha_sc_new(&o);
    if (!world_) return;
    for (const auto& d : scDefs()) { // the modules (not the instruments)
        if (d.instrument || d.bytes.empty()) continue;
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
    processed_ = 0;
}

ScFx::~ScFx()
{
    if (world_) concha_sc_free(world_);
}

int64_t ScFx::oscTime(int64_t sample) const { return concha_sc_seconds_to_osc(double(sample) / sampleRate_); }

void ScFx::queue(int64_t at)
{
    concha_sc_queue_rt(world_, msg_.data(), int(msg_.size()), oscTime(at));
}

int ScFx::start(const sd::FxModule* modules, int count, int64_t at, double sustain)
{
    if (!ok()) return -1;
    int slot = -1;
    for (int k = 0; k < kSlots; ++k)
        if (!busy_[size_t(k)]) {
            slot = k;
            break;
        }
    if (slot < 0) return -1;
    const auto& defs = scDefs();
    const int numOut = kSlots * 2;
    const float inBus = float(numOut + slot * 2), privateBus = float(numOut * 2 + slot * 2), outBus = float(slot * 2);
    at = std::max(at, processed_);
    const int32_t group = nextNode_++;
    msg_.clear();
    {
        Writer w{msg_};
        w.str("/g_new");
        w.str(",iii");
        w.i32(group);
        w.i32(1); // addToTail
        w.i32(0);
    }
    queue(at);
    auto synth = [&](std::string_view name, const sd::SynthArg* args, int n, int sustainArg, float in, float out) {
        msg_.clear();
        Writer w{msg_};
        w.str("/s_new");
        char tags[10 + 2 * 64] = ",siiisfsf"; // no allocation on the audio thread
        n = std::min(n, 64);
        for (int i = 0; i < n; ++i) std::memcpy(tags + 9 + 2 * i, "if", 3);
        w.str(tags);
        w.str(name);
        w.i32(-1);
        w.i32(1);
        w.i32(group);
        w.str("in");
        w.f32(in);
        w.str("out");
        w.f32(out);
        for (int i = 0; i < n; ++i) {
            w.i32(args[i].control);
            w.f32(i == sustainArg ? float(sustain) : args[i].value);
        }
        queue(at);
    };
    synth("concha_fx_in2", nullptr, 0, -1, inBus, privateBus);
    for (int k = 0; k < count; ++k) {
        const auto& m = modules[k];
        if (m.def < 0 || m.def >= int(defs.size())) continue;
        if (trace) trace(at, defs[size_t(m.def)], m.args, m.numArgs, m.sustainArg, sustain);
        synth(defs[size_t(m.def)].name, m.args, m.numArgs, m.sustainArg, privateBus, privateBus);
    }
    synth("concha_fx_out2", nullptr, 0, -1, privateBus, outBus);
    busy_[size_t(slot)] = true;
    group_[size_t(slot)] = group;
    return slot;
}

void ScFx::stop(int slot)
{
    if (slot < 0 || slot >= kSlots || !busy_[size_t(slot)]) return;
    msg_.clear();
    Writer w{msg_};
    w.str("/n_free");
    w.str(",i");
    w.i32(group_[size_t(slot)]);
    queue(processed_);
    busy_[size_t(slot)] = false;
}

void ScFx::processTo(int64_t end)
{
    if (!ok()) return;
    while (processed_ + 64 <= end) {
        const size_t at = size_t(processed_ & (kRing - 1));
        for (int c = 0; c < kSlots * 2; ++c) ins_[size_t(c)] = in_[size_t(c)].data() + at;
        concha_sc_process(world_, ins_.data(), kSlots * 2, outs_.data(), kSlots * 2, 64, oscTime(processed_));
        for (int c = 0; c < kSlots * 2; ++c) {
            std::memcpy(out_[size_t(c)].data() + at, outs_[size_t(c)], 64 * sizeof(float));
            std::memset(in_[size_t(c)].data() + at, 0, 64 * sizeof(float)); // a slot not written stays silent
        }
        processed_ += 64;
    }
}

} // namespace concha::sc
