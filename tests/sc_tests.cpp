// The embedded scsynth (src/sc): StrudelDirt's SynthDefs, instruments started at their sample.
#include "check.h"
#include "sc/ScDefs.h"
#include "sc/ScFx.h"
#include "sc/ScHost.h"
#include "sc/ScOrbitFx.h"

#include <chrono>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <unistd.h>

using namespace concha;

TEST("SynthDefs: StrudelDirt's, with their controls, instruments told apart")
{
    const auto& defs = sc::scDefs();
    CHECK(defs.size() >= 120);
    const int piano = sc::scDefIndex("superpiano");
    CHECK(piano >= 0 && defs[size_t(piano)].instrument && !defs[size_t(piano)].bytes.empty());
    CHECK(piano >= 0 && sc::scControlIndex(defs[size_t(piano)], "freq") >= 0);
    const int gate = sc::scDefIndex("dirt_gate2");
    CHECK(gate >= 0 && !defs[size_t(gate)].instrument);
    CHECK(sc::scDefIndex("sine") >= 0 && !defs[size_t(sc::scDefIndex("sine"))].instrument); // superdough's
}

TEST("SynthDef files: read like the index, a user's folder adds instruments")
{
    const auto& defs = sc::scDefs();
    const auto& hex = defs[size_t(sc::scDefIndex("superhex"))];
    std::vector<sc::ScDef> parsed;
    std::string error;
    CHECK(sc::parseSynthDefFile(hex.bytes, parsed, error));
    CHECK(parsed.size() == 1 && parsed[0].name == "superhex");
    CHECK(parsed.size() == 1 && parsed[0].controls == hex.controls);
    CHECK(!sc::parseSynthDefFile(std::string_view("SCgf\0\0\0\2\0\1\x05" "ab", 13), parsed, error) &&
          error.find("truncated") != std::string::npos);

    // a user's folder: superhex's file renamed "userhexy" (a name of the same length)
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("concha_sc_test_" + std::to_string(::getpid()));
    fs::create_directories(dir / "sub");
    std::string bytes(hex.bytes);
    bytes.replace(bytes.find("superhex"), 8, "userhexy");
    std::ofstream(dir / "sub" / "userhexy.scsyndef", std::ios::binary) << bytes;
    sc::ScHost host(48000); // created before: gets the new SynthDef with loadNewDefs
    const int before = sc::scDefCount();
    const auto messages = sc::loadUserSynthDefFolders({dir.string()});
    CHECK(messages.size() == 1 && messages[0].rfind("loaded 1 SynthDef", 0) == 0);
    CHECK(sc::scDefCount() == before + 1);
    CHECK(sc::loadUserSynthDefFolders({dir.string()}).empty()); // unchanged: not read again
    const int def = sc::scDefIndex("userhexy");
    CHECK(def >= int(defs.size()) && sc::scDef(def).instrument && sc::scDef(def).source.find("userhexy") != std::string::npos);
    host.loadNewDefs();
    for (int i = 0; i < 20; ++i) { // /d_recv: scsynth's NRT thread, then the World's blocks
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        host.renderTo(4096 * (i + 1) / 20 * 1);
    }
    const auto& d = sc::scDef(def);
    const sd::SynthArg args[] = {{uint8_t(sc::scControlIndex(d, "freq")), 220.f}, {uint8_t(sc::scControlIndex(d, "sustain")), 0.5f}};
    int64_t read = 0;
    const int slot = host.start(def, args, 2, 8192, read);
    CHECK(slot >= 0);
    host.renderTo(16384);
    float peak = 0;
    for (int64_t i = read; i < 16384; ++i) peak = std::max(peak, std::fabs(host.sample(slot, 0, i)));
    CHECK(peak > 0.01f);
    host.stop(slot);
    fs::remove_all(dir);
    CHECK(sc::loadUserSynthDefFolders({dir.string()}).size() == 1); // "not found"
}

// the first sample of slot `slot` above a threshold, reading from `from`
static int64_t firstSound(const sc::ScHost& h, int slot, int64_t from, int64_t to)
{
    for (int64_t i = from; i < to; ++i)
        if (std::fabs(h.sample(slot, 0, i)) > 1e-7f) return i;
    return -1;
}

TEST("ScHost: an instrument starts at its sample, read from its block's start")
{
    sc::ScHost host(48000);
    CHECK(host.ok());
    const int def = sc::scDefIndex("superhex");
    const auto& d = sc::scDefs()[size_t(def)];
    const sd::SynthArg args[] = {{uint8_t(sc::scControlIndex(d, "freq")), 220.f}, {uint8_t(sc::scControlIndex(d, "sustain")), 0.5f}};
    // two notes 3 samples apart within a block: the same sound, 3 samples apart once aligned
    int64_t readA = 0, readB = 0;
    const int a = host.start(def, args, 2, 4096 + 10, readA);
    const int b = host.start(def, args, 2, 8192 + 13, readB);
    CHECK(a >= 0 && b >= 0);
    CHECK(readA == 4096 && readB == 8192); // Out.ar: from the start of their 64-sample block
    host.renderTo(16384);
    const int64_t fa = firstSound(host, a, readA, 16384), fb = firstSound(host, b, readB, 16384);
    CHECK(fa - readA == fb - readB); // the same delay from the synth's start
    float diff = 0;
    for (int i = 0; i < 2000; ++i) diff = std::max(diff, std::fabs(host.sample(a, 0, readA + i) - host.sample(b, 0, readB + i)));
    CHECK(diff < 1e-5f);
    host.stop(a);
    host.stop(b);
}

TEST("ScFx: a chain gives its input back as written; a module changes it")
{
    sc::ScFx fx(48000);
    CHECK(fx.ok());
    const int64_t at = 4096 + 21;
    const int plain = fx.start(nullptr, 0, at, 1.0); // concha_fx_in2 -> concha_fx_out2 only
    sd::FxModule squiz;
    squiz.def = int16_t(sc::scDefIndex("squiz2"));
    squiz.args[0] = {uint8_t(sc::scControlIndex(sc::scDefs()[size_t(squiz.def)], "pitchratio")), 2.f};
    squiz.numArgs = 1;
    const int squized = fx.start(&squiz, 1, at, 1.0);
    CHECK(plain >= 0 && squized >= 0);
    for (int64_t i = at; i < at + 4000; ++i) {
        const float x = float(std::sin(2 * 3.14159265 * 220 * double(i - at) / 48000.0));
        fx.write(plain, i, x, -x);
        fx.write(squized, i, x, -x);
    }
    fx.processTo(at + 4000);
    float err = 0, diff = 0;
    for (int64_t i = at; i < at + 3900; ++i) {
        const float x = float(std::sin(2 * 3.14159265 * 220 * double(i - at) / 48000.0));
        err = std::max(err, std::fabs(fx.output(plain, 0, i) - x) + std::fabs(fx.output(plain, 1, i) + x));
        diff = std::max(diff, std::fabs(fx.output(squized, 0, i) - x));
    }
    CHECK(err < 1e-6f); // the same samples, at the same absolute positions (kLatency is the reader's)
    CHECK(diff > 0.1f);
    fx.stop(plain);
    fx.stop(squized);
}

int main() { return check::run(); }

TEST("ScOrbitFx: leslie on one orbit, its sound back on that orbit only")
{
    sc::ScOrbitFx fx(48000);
    CHECK(fx.ok());
    const char* names[] = {"leslie", "lrate"};
    const float values[] = {1.f, 6.7f};
    fx.set(3, sd::OrbitFxServer::Leslie, names, values, 2, 0);
    for (int64_t i = 0; i < 9600; ++i) {
        const float x = 0.5f * std::sin(float(i) * 2.f * 3.14159265f * 220.f / 48000.f);
        fx.write(3, i, x, x);
    }
    fx.processTo(9600);
    float on = 0, other = 0;
    for (int64_t i = 0; i < 9600; ++i) {
        on = std::max(on, std::fabs(fx.output(3, 0, i)));
        other = std::max(other, std::fabs(fx.output(2, 0, i)));
    }
    CHECK(on > 0.1f && other == 0.f);
    fx.reset();
}
