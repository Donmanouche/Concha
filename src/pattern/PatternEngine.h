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
    bool visual = false; // .pianoroll() & co.: a hap of an oscilloscope channel (js/src/visuals.mjs)

    const ParamValue* find(std::string_view key) const;
};

// What a canvas of Strudel's visuals drew in one frame (js/src/canvas.mjs): opcodes and arguments,
// and the strings they refer to. Replayed by the window (plugin/ui/Canvas2D).
struct CanvasFrame {
    std::string id;
    bool widget = false; // an inline widget (._pianoroll()...): see EvalResult::widgets
    int width = 0, height = 0;
    std::vector<double> nums;
    std::vector<std::string> strs;
};

struct EvalResult {
    bool ok = false;
    std::string error;
    std::vector<std::pair<int, int>> miniLocations; // mini-notation ranges, for highlighting
    struct Slider {
        int from = 0, to = 0;    // the value's range in the code; its id is "slider_<from>"
        std::string value;       // as written
        double min = 0, max = 1, step = 0; // step 0: (max - min) / 1000
    };
    std::vector<Slider> sliders; // slider(value, min, max, step) calls, for the editor's widgets
    struct Widget {              // ._pianoroll() ... calls: the canvas they draw on (CanvasFrame::id)
        std::string type, canvas;
        int from = 0, to = 0;
    };
    std::vector<Widget> widgets;
    std::string shader; // shader(`...`): Concha's GLSL background, empty for none
    struct Visuals {    // what the code's visual functions asked for
        bool channels = false;           // .pianoroll() & co.: the main screen
        std::vector<std::string> scope, fft; // the analysers of .scope() / .spectrum()
        bool custom = false;             // its own .draw() / .onPaint(): Strudel's canvases
    } visuals;
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
    // Live values read at query time (js/src/midiin.mjs): kind 0 = MIDI CC `a` on channel `b`
    // (0 = any), 0..1; kind 1 = macro `a` (1..16), 0..1. Must be fast and never block.
    virtual double control(int kind, int a, int b) { return 0.0; }
    // `.analyze(id)`'s audio (js/src/analysers.mjs): kind 1 = time domain (fftSize / 2 values, as
    // getFloatTimeDomainData into frequencyBinCount), 2 = frequency (dB). Returns the fftSize, 0
    // before any sound. Pattern thread.
    virtual int analyser(const std::string& id, int kind, std::vector<float>& out) { return 0; }
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
    // Visuals: runs the animation frames of the code at `cycle` (draw.mjs's getTime()); false on error.
    bool frame(double cycle, double ms, std::vector<CanvasFrame>& out, std::string* error = nullptr);
    bool hasFrames();
    void setViewport(int width, int height, double pixelRatio);
    // keysOnly: only the notes just played on the MIDI input (pushed with evalJson("concha.pushKey(...)")).
    bool query(double begin, double end, double cps, std::vector<PatternEvent>& out, std::string* error = nullptr,
               bool keysOnly = false);

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
