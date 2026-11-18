// concha_query: runs Strudel code in Concha's pattern engine, without the plug-in.
//
//   concha_query [--cycles N] [--from C] [--cps X] [--show] [file | -]
//       prints the events of N cycles (default 1) from cycle C, as the scheduler receives them;
//       --show prints hap.show(true) instead (the format of Strudel's tests)
//   concha_query --bench [file | -]
//       evaluation time, then the cost of the scheduler's queries (1/16 cycle at a time)
//   concha_query --suite <cases.json> <results.json> [filter]
//       parity suite of js/test/make-cases.mjs, one fresh engine per case; compare the results
//       with: node js/test/check.mjs <cases.json> --json <results.json>

#include "pattern/PatternEngine.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

using concha::PatternEngine;
using concha::PatternEvent;

namespace {

std::string readAll(const char* path)
{
    if (!path || std::strcmp(path, "-") == 0) return {std::istreambuf_iterator<char>(std::cin), {}};
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot read %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void printLog(PatternEngine& engine)
{
    for (const auto& m : engine.takeLog()) std::fprintf(stderr, "[%s] %s\n", m.type.c_str(), m.message.c_str());
}

std::string formatNumber(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

void printEvent(const PatternEvent& e)
{
    std::string line = formatNumber(e.begin) + " -> " + formatNumber(e.end) + "  " + e.kind + " |";
    for (const auto& [k, v] : e.params) {
        line += ' ' + k + ':';
        line += v.type == concha::ParamValue::Type::Number ? formatNumber(v.number) : v.string;
    }
    if (!e.locations.empty()) {
        line += "  @";
        for (const auto& [a, b] : e.locations) line += ' ' + std::to_string(a) + '-' + std::to_string(b);
    }
    std::puts(line.c_str());
}

int runSuite(const char* casesPath, const char* resultsPath, const char* filter)
{
    const std::string cases = readAll(casesPath);
    std::string countJson, err;
    int count = 0;
    {
        PatternEngine probe;
        if (!probe.ok()) {
            std::fprintf(stderr, "engine: %s\n", probe.initError().c_str());
            return 1;
        }
        if (!probe.evalJson("globalThis.__cases = " + cases + "; Object.keys(__cases).length", countJson, &err)) {
            std::fprintf(stderr, "cases: %s\n", err.c_str());
            return 1;
        }
        count = std::atoi(countJson.c_str());
    }
    const std::string filterJs = filter ? std::string("'") + filter + "'" : "undefined";
    std::string results = "{";
    int done = 0;
    double initMs = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) {
        PatternEngine engine; // fresh runtime per case, like a freshly opened page
        if (!engine.ok()) {
            std::fprintf(stderr, "engine: %s\n", engine.initError().c_str());
            return 1;
        }
        initMs += engine.initMilliseconds();
        std::string json;
        const std::string expr = "globalThis.__cases = " + cases + "; const __k = Object.keys(__cases)[" +
                                 std::to_string(i) + "]; concha.runSuite({ [__k]: __cases[__k] }, " + filterJs + ")";
        if (!engine.evalJson(expr, json, &err)) {
            std::fprintf(stderr, "case %d: %s\n", i, err.c_str());
            continue;
        }
        engine.takeLog();
        if (json.size() > 2) {
            if (done++) results += ',';
            results.append(json, 1, json.size() - 2); // strip { }
        }
        if ((i + 1) % 50 == 0) std::fprintf(stderr, "%d/%d\n", i + 1, count);
    }
    results += '}';
    std::ofstream(resultsPath, std::ios::binary) << results;
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "%d cases in %.1f s (engine start: %.0f ms on average)\n", done, total, done ? initMs / count : 0.0);
    return 0;
}

int bench(PatternEngine& engine, const std::string& code)
{
    const auto r = engine.evaluate(code);
    printLog(engine);
    if (!r.ok) {
        std::fprintf(stderr, "error: %s\n", r.error.c_str());
        return 1;
    }
    std::printf("engine start %.1f ms, evaluate %.1f ms\n", engine.initMilliseconds(), r.milliseconds);
    std::vector<PatternEvent> events;
    const int slices = 16 * 64; // 64 cycles in 1/16 steps
    size_t total = 0;
    double worst = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < slices; ++i) {
        const auto s0 = std::chrono::steady_clock::now();
        std::string err;
        if (!engine.query(i / 16.0, (i + 1) / 16.0, 0.5, events, &err)) {
            std::fprintf(stderr, "query: %s\n", err.c_str());
            return 1;
        }
        worst = std::max(worst, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count());
        total += events.size();
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("64 cycles: %zu events, %.2f ms per cycle on average, worst 1/16-cycle query %.2f ms\n", total, ms / 64.0, worst);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    double cycles = 1.0, from = 0.0, cps = 0.5;
    bool show = false, doBench = false;
    const char* file = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--suite") {
            if (i + 2 >= argc) {
                std::fprintf(stderr, "usage: %s --suite <cases.json> <results.json> [filter]\n", argv[0]);
                return 2;
            }
            return runSuite(argv[i + 1], argv[i + 2], i + 3 < argc ? argv[i + 3] : nullptr);
        }
        if (a == "--cycles" && i + 1 < argc) cycles = std::atof(argv[++i]);
        else if (a == "--from" && i + 1 < argc) from = std::atof(argv[++i]);
        else if (a == "--cps" && i + 1 < argc) cps = std::atof(argv[++i]);
        else if (a == "--show") show = true;
        else if (a == "--bench") doBench = true;
        else if (a == "-h" || a == "--help") {
            std::puts("usage: concha_query [--cycles N] [--from C] [--cps X] [--show] [--bench] [file | -]\n"
                      "       concha_query --suite <cases.json> <results.json> [filter]");
            return 0;
        } else file = argv[i];
    }

    const std::string code = readAll(file);
    PatternEngine engine;
    if (!engine.ok()) {
        std::fprintf(stderr, "engine: %s\n", engine.initError().c_str());
        return 1;
    }
    if (doBench) return bench(engine, code);

    const auto r = engine.evaluate(code);
    printLog(engine);
    if (!r.ok) {
        std::fprintf(stderr, "error: %s\n", r.error.c_str());
        return 1;
    }
    if (show) {
        for (const auto& h : engine.showHaps(from, from + cycles)) std::puts(h.c_str());
    } else {
        std::vector<PatternEvent> events;
        std::string err;
        if (!engine.query(from, from + cycles, cps, events, &err)) {
            std::fprintf(stderr, "query: %s\n", err.c_str());
            return 1;
        }
        for (const auto& e : events) printEvent(e);
    }
    printLog(engine);
    return 0;
}
