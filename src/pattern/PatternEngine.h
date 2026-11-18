#pragma once

// Strudel's pattern engine, running in an embedded QuickJS-ng (bundle: js/src/concha.mjs).
// One PatternEngine per plug-in instance, used from a single thread (the pattern thread, never
// the audio thread). Not thread-safe.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace concha {

struct ParamValue {
    enum class Type { Number, String } type = Type::Number;
    double number = 0.0;
    std::string string; // strings, and objects / arrays as JSON
};

// One scheduled hap (an onset inside the queried window).
struct PatternEvent {
    std::string kind;   // "sound" (default output) or the output method: "midi", "osc"...
    double begin = 0.0; // whole, in cycles
    double end = 0.0;
    double duration = 0.0; // hap.duration (clip included), in cycles
    std::vector<std::pair<std::string, ParamValue>> params; // hap.value
    std::vector<std::pair<int, int>> locations;             // code ranges [start, end)

    const ParamValue* find(std::string_view key) const;
};

struct EvalResult {
    bool ok = false;
    std::string error;
    std::vector<std::pair<int, int>> miniLocations; // mini-notation ranges, for highlighting
    double milliseconds = 0.0;
};

struct LogMessage {
    std::string type; // "info", "warning", "error"...
    std::string message;
};

// What the JS runtime may ask Concha for (js/src/sounds.mjs). Called on the thread that runs
// evaluate(), blocking is fine there.
struct PatternHost {
    virtual ~PatternHost() = default;
    virtual bool fetchText(const std::string& url, std::string& text, std::string& error) = 0;
    // Sample map JSON of a named library folder, empty if there is none by that name.
    virtual std::string libraryMap(const std::string& name, std::string& error) = 0;
    // {"version": n, "names": [...]}: folders loaded before every evaluation.
    virtual std::string libraryAutoload() = 0;
    // A buffer rendered in JS (zzfx, bytebeat) for the sampler; returns its sample id (0: failed).
    virtual uint32_t publishBuffer(const float* data, size_t frames, double sampleRate) { return 0; }
};

class PatternEngine {
public:
    struct Options {
        size_t memoryLimitBytes = size_t(512) << 20;
        std::chrono::milliseconds evaluateTimeout{5000}; // stops `while(true){}` in user code
        std::chrono::milliseconds queryTimeout{1000};
        std::string bundleSource; // empty: the bundle compiled into the binary
        PatternHost* host = nullptr; // natives for samples(); none: samples('...') does nothing
    };

    PatternEngine();
    explicit PatternEngine(Options options);
    ~PatternEngine();
    PatternEngine(const PatternEngine&) = delete;
    PatternEngine& operator=(const PatternEngine&) = delete;

    bool ok() const;
    const std::string& initError() const;
    double initMilliseconds() const;

    EvalResult evaluate(std::string_view code);

    // Haps with an onset in [begin, end) (cycles), in order. cps: cycles per second (the DAW's).
    // Returns false on error (message in *error); `out` is cleared first.
    bool query(double begin, double end, double cps, std::vector<PatternEvent>& out, std::string* error = nullptr);

    // hap.show(true) of every hap in [begin, end), as Strudel's tests print them.
    std::vector<std::string> showHaps(double begin, double end);

    // Runs `expression` (a promise or a value), returns its JSON. For tools and tests.
    bool evalJson(std::string_view expression, std::string& json, std::string* error = nullptr);

    void setSlider(std::string_view id, double value);
    // The audio engine's sample rate, seen by JS as globalThis.__conchaSampleRate (any thread).
    void setSampleRate(double sampleRate);
    std::vector<LogMessage> takeLog();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace concha
