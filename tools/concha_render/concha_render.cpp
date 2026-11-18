// concha_render: renders Strudel code with superdough C++ into a WAV file, without the plug-in.
//
//   concha_render [--cycles N] [--cps X] [--rate SR] [--stereo-outputs K] [--trace-sc file] <code.js | -> <out.wav>
//   --trace-sc writes one JSON line per StrudelDirt instrument: the hap ("hap", its value without
//   Concha's __ keys, cycle, duration in cycles, cps, onset sample) and the synth started ("synth":
//   onset, SynthDef, controls), for tools/sc/compare_strudeldirt.py.
//
// Same path as the plug-in: Strudel in QuickJS -> SoundConverter -> superdough Engine, samples
// through the sample library (named folders of ~/.config/Concha/library.txt, http cache).
// All the events are queried first, the samples they use are loaded, then the audio is rendered
// in blocks of 128 with each event on its exact sample. Output K (default 1) stereo pairs.

#include "library/LibraryHost.h"
#include "library/SampleLibrary.h"
#include "pattern/PatternEngine.h"
#include "sched/SoundConvert.h"
#include "sc/ScFx.h"
#include "sc/ScOrbitFx.h"
#include "sc/ScDefs.h"
#include "sc/ScHost.h"
#include "superdough/Engine.h"
#include "superdough/PhaseVocoder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <thread>
#include <vector>

using namespace concha;

namespace {
std::string readAll(const char* path)
{
    if (std::strcmp(path, "-") == 0) return {std::istreambuf_iterator<char>(std::cin), {}};
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void writeWav(const char* path, const std::vector<float>& interleaved, int channels, int rate)
{
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = uint32_t(interleaved.size() * 4);
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(3); // float
    u16(uint16_t(channels));
    u32(uint32_t(rate));
    u32(uint32_t(rate * channels * 4));
    u16(uint16_t(channels * 4));
    u16(32);
    f.write("data", 4);
    u32(bytes);
    f.write(reinterpret_cast<const char*>(interleaved.data()), bytes);
}
std::string jsonEscape(const std::string& s)
{
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if (uint8_t(c) < 0x20) continue;
        o += c;
    }
    return o;
}
} // namespace

int main(int argc, char** argv)
{
    double cycles = 4, cps = 0.5;
    int rate = 48000, outputs = 1;
    std::string tracePath;
    std::FILE* trace = nullptr;
    std::vector<const char*> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--cycles" && i + 1 < argc) cycles = std::atof(argv[++i]);
        else if (a == "--cps" && i + 1 < argc) cps = std::atof(argv[++i]);
        else if (a == "--rate" && i + 1 < argc) rate = std::atoi(argv[++i]);
        else if (a == "--stereo-outputs" && i + 1 < argc) outputs = std::max(1, std::atoi(argv[++i]));
        else if (a == "--trace-sc" && i + 1 < argc) tracePath = argv[++i];
        else files.push_back(argv[i]);
    }
    if (files.size() != 2) {
        std::fprintf(stderr, "usage: %s [--cycles N] [--cps X] [--rate SR] [--stereo-outputs K] <code.js | -> <out.wav>\n", argv[0]);
        return 2;
    }
    auto& library = lib::SampleLibrary::instance();
    lib::LibraryHost host(library);
    PatternEngine::Options eo;
    eo.host = &host;
    PatternEngine engine(eo);
    engine.setSampleRate(rate);
    if (!engine.ok()) {
        std::fprintf(stderr, "engine: %s\n", engine.initError().c_str());
        return 1;
    }
    for (const auto& m : sc::loadUserSynthDefFolders(library.synthDefFolders())) // the user's SynthDefs
        std::fprintf(stderr, "[info] %s\n", m.c_str());
    registerSuperDirtSynths(engine);
    if (!tracePath.empty() && !(trace = std::fopen(tracePath.c_str(), "w"))) {
        std::fprintf(stderr, "cannot write %s\n", tracePath.c_str());
        return 1;
    }
    const auto r = engine.evaluate(readAll(files[0]));
    for (const auto& m : engine.takeLog())
        if (m.message.find("loaded") == std::string::npos) std::fprintf(stderr, "[%s] %s\n", m.type.c_str(), m.message.c_str());
    if (!r.ok) {
        std::fprintf(stderr, "error: %s\n", r.error.c_str());
        return 1;
    }
    // query in quarter cycles, as the plug-in's pattern thread does (each query has a time limit)
    std::vector<PatternEvent> events, chunk;
    std::string err;
    const auto q0 = std::chrono::steady_clock::now();
    for (double c = 0; c < cycles; c += 0.25) {
        if (!engine.query(c, std::min(cycles, c + 0.25), cps, chunk, &err)) {
            std::fprintf(stderr, "query: %s\n", err.c_str());
            return 1;
        }
        events.insert(events.end(), chunk.begin(), chunk.end());
    }
    const double queryMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - q0).count();
    SoundConverter conv;
    conv.library = &library;
    conv.sampleRate = rate;
    struct Ev {
        int64_t sample;
        sd::SoundParams p;
        double duration, cycle;
    };
    std::vector<Ev> sounds;
    std::vector<std::string> log;
    for (const auto& e : events) {
        if (e.kind != "sound") continue;
        Ev ev{int64_t(std::floor(e.begin / cps * rate + 1e-6)), {}, e.duration / cps, e.begin};
        if (!conv.convert(e, ev.p, log)) continue;
        if (trace && ev.p.type == sd::SoundType::ScSynth) {
            std::string j = "{\"hap\":{";
            bool first = true;
            for (const auto& [k, v] : e.params) {
                if (k.rfind("__", 0) == 0) continue;
                j += (first ? "\"" : ",\"") + jsonEscape(k) + "\":";
                first = false;
                if (v.type == ParamValue::Type::Number) {
                    char b[40];
                    std::snprintf(b, sizeof b, "%.9g", v.number);
                    j += b;
                } else j += "\"" + jsonEscape(v.string) + "\"";
            }
            const auto env = sd::Engine::scEnvelope(ev.p, ev.duration);
            char b[480];
            std::snprintf(b, sizeof b,
                          "},\"cycle\":%.12g,\"duration\":%.12g,\"cps\":%.12g,\"sample\":%lld,\"envelope\":{\"holdtime\":%.9g,"
                          "\"totalDuration\":%.9g,\"attack\":%.9g,\"decay\":%.9g,\"hold\":%.9g,\"release\":%.9g,\"curve\":%.9g,"
                          "\"amp\":%.9g}}\n",
                          e.begin, e.duration, cps, (long long)ev.sample, env.sustain, env.total, env.attack, env.decay, env.level,
                          env.release, env.curve, env.amp);
            std::fputs((j + b).c_str(), trace);
        }
        if (!std::isnan(ev.p.stretch)) { // started earlier (vocoder latency); superdough skips it if that is in the past
            ev.sample -= int64_t(std::llround(sd::kStretchAdvanceSeconds(rate) * rate));
            if (ev.sample < 0) continue;
        }
        if (ev.p.numFxModules > 0) { // StrudelDirt's effects give the voice back kLatency samples later
            ev.sample -= sd::FxServer::kLatency;
            if (ev.sample < 0) ev.sample = 0;
        }
        sounds.push_back(ev);
    }
    for (const auto& m : log) std::fprintf(stderr, "[warning] %s\n", m.c_str());
    // wait for the samples (60 s at most)
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (const auto& s : sounds)
        while (s.p.sampleId && !library.isLoaded(s.p.sampleId) && std::chrono::steady_clock::now() < until) {
            for (const auto& m : library.takeErrors()) std::fprintf(stderr, "[error] %s\n", m.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    for (const auto& m : library.takeErrors()) std::fprintf(stderr, "[error] %s\n", m.c_str());
    std::stable_sort(sounds.begin(), sounds.end(), [](const Ev& a, const Ev& b) { return a.sample < b.sample; });

    sd::Engine sd;
    sd.prepare(rate, true); // reverb impulse responses built inline: the render does not wait for them
    sd.setSampleStore(&library.store());
    sc::ScHost scHost(rate); // StrudelDirt's instruments
    if (scHost.ok()) sd.setSynthServer(&scHost);
    else std::fprintf(stderr, "[warning] scsynth did not start: StrudelDirt's instruments are silent\n");
    if (trace)
        scHost.trace = [trace](int64_t onset, const sc::ScDef& d, const sd::SynthArg* args, int n) {
            std::string j = "{\"synth\":\"" + jsonEscape(d.name) + "\",\"sample\":" + std::to_string(onset) + ",\"args\":{";
            for (int i = 0; i < n; ++i) {
                char b[64];
                std::snprintf(b, sizeof b, "%.9g", double(args[i].value));
                const std::string name = args[i].control < d.controls.size() ? d.controls[args[i].control] : std::to_string(args[i].control);
                j += (i ? ",\"" : "\"") + jsonEscape(name) + "\":" + b;
            }
            std::fputs((j + "}}\n").c_str(), trace);
        };
    sc::ScFx scFx(rate); // StrudelDirt's effects
    if (scFx.ok()) sd.setFxServer(&scFx);
    if (trace)
        scFx.trace = [trace](int64_t at, const sc::ScDef& d, const sd::SynthArg* args, int n, int sustainArg, double sustain) {
            std::string j = "{\"module\":\"" + jsonEscape(d.name) + "\",\"sample\":" + std::to_string(at) + ",\"args\":{";
            for (int i = 0; i < n; ++i) {
                char b[64];
                std::snprintf(b, sizeof b, "%.9g", i == sustainArg ? sustain : double(args[i].value));
                const std::string name = args[i].control < d.controls.size() ? d.controls[args[i].control] : std::to_string(args[i].control);
                j += (i ? ",\"" : "\"") + jsonEscape(name) + "\":" + b;
            }
            std::fputs((j + "}}\n").c_str(), trace);
        };
    sc::ScOrbitFx scOrbitFx(rate); // StrudelDirt's orbit effects
    if (scOrbitFx.ok()) sd.setOrbitFxServer(&scOrbitFx);
    const int64_t total = int64_t(cycles / cps * rate) + rate * 2; // 2 s of tail
    const int block = 128;
    std::vector<float> out(size_t(total) * size_t(outputs) * 2, 0.f);
    std::vector<std::vector<float>> bus(size_t(outputs) * 2, std::vector<float>(block));
    std::vector<std::array<float*, 2>> ptrs(static_cast<size_t>(outputs), std::array<float*, 2>{});
    std::vector<float* const*> buses(static_cast<size_t>(outputs), nullptr);
    size_t next = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int64_t pos = 0; pos < total; pos += block) {
        const int n = int(std::min<int64_t>(block, total - pos));
        for (int b = 0; b < outputs; ++b) {
            for (int c = 0; c < 2; ++c) std::fill(bus[size_t(b * 2 + c)].begin(), bus[size_t(b * 2 + c)].end(), 0.f);
            ptrs[size_t(b)] = {bus[size_t(b * 2)].data(), bus[size_t(b * 2 + 1)].data()};
            buses[size_t(b)] = ptrs[size_t(b)].data();
        }
        while (next < sounds.size() && sounds[next].sample < pos + n) {
            const auto& s = sounds[next++];
            sd.trigger(int(std::max<int64_t>(0, s.sample - pos)), s.p, s.duration, cps, s.cycle);
        }
        sd.render(buses.data(), outputs, n);
        for (int i = 0; i < n; ++i)
            for (int b = 0; b < outputs; ++b)
                for (int c = 0; c < 2; ++c) out[size_t((pos + i) * outputs * 2 + b * 2 + c)] = bus[size_t(b * 2 + c)][size_t(i)];
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    writeWav(files[1], out, outputs * 2, rate);
    float peak = 0.f;
    for (float v : out) peak = std::max(peak, std::fabs(v));
    std::fprintf(stderr, "%zu sounds, %.1f s rendered in %.0f ms (%.1fx real time), queries %.0f ms, peak %.3f, missing samples %llu\n",
                 sounds.size(), total / double(rate), ms, (total / double(rate)) / (ms / 1000.0), queryMs, peak,
                 (unsigned long long)sd.missingSamples());
    for (const auto& w : sd.takeWarnings()) std::fprintf(stderr, "[warning] %s\n", w.c_str());
    if (trace) std::fclose(trace);
    return 0;
}
