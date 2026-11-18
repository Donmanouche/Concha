// Loads every .scsyndef of a folder into the embedded scsynth (UGens compiled in) and plays each
// one for a moment: scsynth prints "UGen ... not installed" / "FAILURE" for anything missing.
// usage: spike_sc_check_defs <synthdef dir>
#include "sc/ConchaSc.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <chrono>
#include <thread>
#include <vector>

namespace {
struct Osc {
    std::string b;
    void pad() { while (b.size() % 4) b.push_back('\0'); }
    void str(const std::string& s) { b += s; b.push_back('\0'); pad(); }
    void i32(int32_t v) { for (int k = 3; k >= 0; --k) b.push_back(char((uint32_t(v) >> (k * 8)) & 0xff)); }
    void f32(float f) { int32_t v; std::memcpy(&v, &f, 4); i32(v); }
    void blob(const std::string& d) { i32(int32_t(d.size())); b += d; pad(); }
};
std::string readFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    ConchaScOptions opt{48000, 512, 32, 0, "/nonexistent", 0};
    ConchaScWorld* w = concha_sc_new(&opt);
    std::vector<std::string> names;
    for (const auto& e : std::filesystem::directory_iterator(argv[1]))
        if (e.path().extension() == ".scsyndef") {
            Osc m;
            m.str("/d_recv");
            m.str(",b");
            m.blob(readFile(e.path()));
            concha_sc_send(w, m.b.data(), int(m.b.size()));
            names.push_back(e.path().stem().string());
        }
    std::vector<float> buf(32 * 512);
    std::vector<float*> outs(32);
    for (int c = 0; c < 32; ++c) outs[size_t(c)] = buf.data() + c * 512;
    int64_t t = 0;
    auto run = [&](int blocks) {
        for (int i = 0; i < blocks; ++i) {
            concha_sc_process(w, nullptr, 0, outs.data(), 32, 512, t);
            t += concha_sc_seconds_to_osc(512.0 / 48000.0);
        }
    };
    run(20); // the defs are received on the NRT thread, in real time
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    run(4);
    int node = 1000;
    for (const auto& n : names) { // a synth of each, on a private bus, freed by /n_free
        Osc m;
        m.str("/s_new");
        m.str(",siiisfsf");
        m.str(n);
        m.i32(node++);
        m.i32(0);
        m.i32(0);
        m.str("out");
        m.f32(100.f);
        m.str("sustain");
        m.f32(0.1f);
        concha_sc_send(w, m.b.data(), int(m.b.size()));
    }
    for (int i = 0; i < 40; ++i) { // in real time: the synths' async parts too
        run(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("checked %zu SynthDefs\n", names.size());
    concha_sc_free(w);
    return 0;
}
