#include "pattern/PatternEngine.h"

#include "pattern/EmbeddedBundle.h"

extern "C" {
#include "quickjs.h"
}

#include <atomic>
#include <cstdio>
#include <cstring>

namespace concha {

const ParamValue* PatternEvent::find(std::string_view key) const
{
    for (const auto& [k, v] : params)
        if (k == key) return &v;
    return nullptr;
}

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// RAII for a JSValue owned by a context.
struct Value {
    JSContext* ctx;
    JSValue v;
    Value(JSContext* c, JSValue val) : ctx(c), v(val) {}
    ~Value() { JS_FreeValue(ctx, v); }
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
    Value get(const char* name) const { return {ctx, JS_GetPropertyStr(ctx, v, name)}; }
    Value at(uint32_t i) const { return {ctx, JS_GetPropertyUint32(ctx, v, i)}; }
    bool isException() const { return JS_IsException(v); }
    double number(double fallback = 0.0) const
    {
        double d = fallback;
        if (JS_IsNumber(v) || JS_IsBool(v)) JS_ToFloat64(ctx, &d, v);
        return d;
    }
    std::string string() const
    {
        size_t len = 0;
        const char* s = JS_ToCStringLen(ctx, &len, v);
        std::string out = s ? std::string(s, len) : std::string();
        if (s) JS_FreeCString(ctx, s);
        return out;
    }
    uint32_t length() const
    {
        Value l = get("length");
        return static_cast<uint32_t>(l.number(0.0));
    }
};

} // namespace

struct PatternEngine::Impl {
    Options options;
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    JSValue concha = JS_UNDEFINED;
    JSValue queryFn = JS_UNDEFINED;
    std::string initError;
    double initMs = 0.0;
    Clock::time_point deadline = Clock::time_point::max();
    std::vector<LogMessage> log;

    static JSValue consoleLog(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic)
{
    auto* impl = static_cast<Impl*>(JS_GetContextOpaque(ctx));
    std::string msg;
    for (int i = 0; i < argc; ++i) {
        size_t len = 0;
        if (const char* s = JS_ToCStringLen(ctx, &len, argv[i])) {
            std::string part(s, len);
            JS_FreeCString(ctx, s);
            // Strudel's logger prints "%c<message>" and also dispatches it as an event, which
            // concha.log() returns: skip the console copy.
            if (i == 0 && part.rfind("%c", 0) == 0) return JS_UNDEFINED;
            if (!msg.empty()) msg += ' ';
            msg += part;
        }
    }
    static const char* types[] = {"info", "info", "debug", "warning", "error"};
    if (impl) impl->log.push_back({types[magic], std::move(msg)});
    return JS_UNDEFINED;
}

    // __concha_fetch(url), __concha_library(name), __concha_library_autoload()
    static JSValue native(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic)
    {
        auto* impl = static_cast<Impl*>(JS_GetContextOpaque(ctx));
        PatternHost* host = impl ? impl->options.host : nullptr;
        if (!host) return JS_ThrowInternalError(ctx, "no host");
        std::string arg;
        if (argc > 0) {
            size_t len = 0;
            if (const char* s = JS_ToCStringLen(ctx, &len, argv[0])) {
                arg.assign(s, len);
                JS_FreeCString(ctx, s);
            }
        }
        // the call may block (downloads): do not count it in the evaluation timeout
        const auto deadline = impl->deadline;
        const auto callStart = Clock::now();
        impl->deadline = Clock::time_point::max();
        if (magic == 3) { // __concha_buffer(Float32Array, sampleRate)
            size_t offset = 0, length = 0, bpe = 0;
            JSValue ab = argc > 0 ? JS_GetTypedArrayBuffer(ctx, argv[0], &offset, &length, &bpe) : JS_EXCEPTION;
            if (JS_IsException(ab) || bpe != 4) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                return JS_ThrowTypeError(ctx, "__concha_buffer expects a Float32Array");
            }
            size_t size = 0;
            uint8_t* bytes = JS_GetArrayBuffer(ctx, &size, ab);
            double rate = 48000;
            if (argc > 1) JS_ToFloat64(ctx, &rate, argv[1]);
            const uint32_t id = bytes ? host->publishBuffer(reinterpret_cast<const float*>(bytes + offset), length / 4, rate) : 0;
            JS_FreeValue(ctx, ab);
            return JS_NewUint32(ctx, id);
        }
        std::string out, error;
        bool ok = true;
        if (magic == 0) ok = host->fetchText(arg, out, error);
        else if (magic == 1) {
            out = host->libraryMap(arg, error);
            if (out.empty() && !error.empty()) ok = false;
        } else out = host->libraryAutoload();
        if (deadline != Clock::time_point::max()) impl->deadline = deadline + (Clock::now() - callStart); // time left kept
        if (!ok) return JS_ThrowInternalError(ctx, "%s", error.c_str());
        if (magic == 1 && out.empty()) return JS_UNDEFINED;
        return JS_NewStringLen(ctx, out.data(), out.size());
    }

    static int interrupt(JSContext*, void* opaque)
    {
        auto* self = static_cast<Impl*>(opaque);
        return Clock::now() > self->deadline ? 1 : 0;
    }

    std::atomic<double> sampleRate{48000.0};
    double sampleRateSeen = 0.0;
    void applySampleRate()
    {
        const double sr = sampleRate.load();
        if (sr == sampleRateSeen) return;
        sampleRateSeen = sr;
        Value g(ctx, JS_GetGlobalObject(ctx));
        JS_SetPropertyStr(ctx, g.v, "__conchaSampleRate", JS_NewFloat64(ctx, sr));
    }

    // Also re-anchors QuickJS's stack-overflow check: the engine may be used from another thread
    // than the one that created it (PatternWorker swaps its two engines between threads).
    void arm(std::chrono::milliseconds timeout)
    {
        JS_UpdateStackTop(rt);
        deadline = Clock::now() + timeout;
        if (ctx && JS_IsObject(concha)) applySampleRate();
    }
    void disarm() { deadline = Clock::time_point::max(); }

    std::string exceptionText()
    {
        Value e(ctx, JS_GetException(ctx));
        std::string msg = e.string();
        if (JS_IsError(e.v)) {
            Value stack = e.get("stack");
            if (JS_IsString(stack.v)) {
                const std::string s = stack.string();
                if (!s.empty()) msg += "\n" + s;
            }
        }
        if (msg.find("interrupted") != std::string::npos)
            msg = "timeout: the code ran for too long (endless loop?)\n" + msg;
        return msg;
    }

    // Runs queued promise jobs until `promise` settles (or no job is left).
    bool settle(JSValue promise, JSValue& result, std::string& error)
    {
        for (;;) {
            const JSPromiseStateEnum st = JS_PromiseState(ctx, promise);
            if (st == JS_PROMISE_FULFILLED) {
                result = JS_PromiseResult(ctx, promise);
                return true;
            }
            if (st == JS_PROMISE_REJECTED) {
                Value reason(ctx, JS_PromiseResult(ctx, promise));
                error = reason.string();
                Value stack = reason.get("stack");
                if (JS_IsString(stack.v)) error += "\n" + stack.string();
                return false;
            }
            if (st != JS_PROMISE_PENDING) { // not a promise: a plain value
                result = JS_DupValue(ctx, promise);
                return true;
            }
            JSContext* jobCtx = nullptr;
            const int r = JS_ExecutePendingJob(rt, &jobCtx);
            if (r < 0) {
                error = exceptionText();
                return false;
            }
            if (r == 0) {
                error = "the code waits for something that never happens (pending promise)";
                return false;
            }
        }
    }

    void drainLog()
    {
        Value fn(ctx, JS_GetPropertyStr(ctx, concha, "log"));
        Value arr(ctx, JS_Call(ctx, fn.v, concha, 0, nullptr));
        if (arr.isException()) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return;
        }
        for (uint32_t i = 0, n = arr.length(); i < n; ++i) {
            Value m = arr.at(i);
            log.push_back({m.get("type").string(), m.get("message").string()});
        }
    }

    bool init()
    {
        const auto t0 = Clock::now();
        rt = JS_NewRuntime();
        if (!rt) return fail("JS_NewRuntime failed");
        JS_SetMemoryLimit(rt, options.memoryLimitBytes);
        // Below the 8 MB of a Linux thread stack (the pattern threads). Windows threads have 1 MB by
        // default: the port will create them with an explicit stack size.
        JS_SetMaxStackSize(rt, size_t(6) << 20);
        JS_SetInterruptHandler(rt, &Impl::interrupt, this);
        ctx = JS_NewContext(rt);
        if (!ctx) return fail("JS_NewContext failed");
        JS_SetContextOpaque(ctx, this);

        Value global(ctx, JS_GetGlobalObject(ctx));
        JSValue console = JS_NewObject(ctx);
        const char* names[] = {"log", "info", "debug", "warn", "error"};
        for (int i = 0; i < 5; ++i)
            JS_SetPropertyStr(ctx, console, names[i], JS_NewCFunctionMagic(ctx, &Impl::consoleLog, names[i], 1, JS_CFUNC_generic_magic, i));
        JS_SetPropertyStr(ctx, global.v, "console", console);
        if (options.host) {
            const char* natives[] = {"__concha_fetch", "__concha_library", "__concha_library_autoload", "__concha_buffer"};
            for (int i = 0; i < 4; ++i)
                JS_SetPropertyStr(ctx, global.v, natives[i], JS_NewCFunctionMagic(ctx, &Impl::native, natives[i], 1, JS_CFUNC_generic_magic, i));
        }

        const std::string_view src = options.bundleSource.empty() ? embeddedBundle() : std::string_view(options.bundleSource);
        if (src.empty()) return fail("the Strudel bundle is empty");
        arm(std::chrono::milliseconds(20000));
        // QuickJS wants a NUL-terminated buffer.
        const std::string code(src);
        Value r(ctx, JS_Eval(ctx, code.c_str(), code.size(), "concha-strudel.js", JS_EVAL_TYPE_GLOBAL));
        if (r.isException()) return fail("bundle: " + exceptionText());

        concha = JS_GetPropertyStr(ctx, global.v, "concha");
        if (!JS_IsObject(concha)) return fail("bundle: globalThis.concha is missing");
        queryFn = JS_GetPropertyStr(ctx, concha, "query");
        Value ready(ctx, JS_GetPropertyStr(ctx, concha, "ready"));
        JSValue done = JS_UNDEFINED;
        std::string err;
        if (!settle(ready.v, done, err)) return fail("bundle setup: " + err);
        JS_FreeValue(ctx, done);
        disarm();
        drainLog();
        initMs = msSince(t0);
        return true;
    }

    bool fail(std::string message)
    {
        disarm();
        initError = std::move(message);
        return false;
    }

    ~Impl()
    {
        if (ctx) {
            JS_FreeValue(ctx, queryFn);
            JS_FreeValue(ctx, concha);
            JS_FreeContext(ctx);
        }
        if (rt) JS_FreeRuntime(rt);
    }

    void readParams(JSValueConst obj, std::vector<std::pair<std::string, ParamValue>>& params)
    {
        JSPropertyEnum* props = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &count, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return;
        }
        params.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const char* key = JS_AtomToCString(ctx, props[i].atom);
            Value v(ctx, JS_GetProperty(ctx, obj, props[i].atom));
            ParamValue pv;
            bool keep = true;
            if (JS_IsNumber(v.v) || JS_IsBool(v.v)) {
                pv.number = v.number();
            } else if (JS_IsString(v.v)) {
                pv.type = ParamValue::Type::String;
                pv.string = v.string();
            } else if (JS_IsNull(v.v) || JS_IsUndefined(v.v) || JS_IsFunction(ctx, v.v)) {
                keep = false;
            } else if (JS_IsObject(v.v)) {
                // Fractions (n/d) become numbers; arrays and other objects their JSON.
                Value n = v.get("n"), d = v.get("d"), s = v.get("s");
                if (!JS_IsUndefined(n.v) && !JS_IsUndefined(d.v) && !JS_IsArray(v.v)) {
                    Value num(ctx, JS_Call(ctx, Value(ctx, JS_GetPropertyStr(ctx, v.v, "valueOf")).v, v.v, 0, nullptr));
                    pv.number = num.number();
                } else {
                    pv.type = ParamValue::Type::String;
                    Value json(ctx, JS_JSONStringify(ctx, v.v, JS_UNDEFINED, JS_UNDEFINED));
                    if (json.isException()) {
                        JS_FreeValue(ctx, JS_GetException(ctx));
                        keep = false;
                    } else {
                        pv.string = json.string();
                    }
                }
            } else {
                keep = false;
            }
            if (keep && key) params.emplace_back(key, std::move(pv));
            if (key) JS_FreeCString(ctx, key);
        }
        JS_FreePropertyEnum(ctx, props, count);
    }
};

PatternEngine::PatternEngine() : PatternEngine(Options{}) {}

PatternEngine::PatternEngine(Options options) : impl_(std::make_unique<Impl>())
{
    impl_->options = std::move(options);
    impl_->init();
}

PatternEngine::~PatternEngine() = default;

bool PatternEngine::ok() const { return impl_->initError.empty() && impl_->ctx != nullptr; }
const std::string& PatternEngine::initError() const { return impl_->initError; }
double PatternEngine::initMilliseconds() const { return impl_->initMs; }

EvalResult PatternEngine::evaluate(std::string_view code)
{
    EvalResult res;
    if (!ok()) {
        res.error = impl_->initError;
        return res;
    }
    auto& I = *impl_;
    JSContext* ctx = I.ctx;
    const auto t0 = Clock::now();
    I.arm(I.options.evaluateTimeout);

    Value fn(ctx, JS_GetPropertyStr(ctx, I.concha, "evaluate"));
    JSValue arg = JS_NewStringLen(ctx, code.data(), code.size());
    Value promise(ctx, JS_Call(ctx, fn.v, I.concha, 1, &arg));
    JS_FreeValue(ctx, arg);
    if (promise.isException()) {
        res.error = I.exceptionText();
    } else {
        JSValue out = JS_UNDEFINED;
        std::string err;
        if (!I.settle(promise.v, out, err)) {
            res.error = err;
        } else {
            Value r(ctx, out);
            res.ok = JS_ToBool(ctx, r.get("ok").v) == 1;
            if (!res.ok) res.error = r.get("error").string();
            Value locs = r.get("miniLocations");
            for (uint32_t i = 0, n = JS_IsArray(locs.v) ? locs.length() : 0; i < n; ++i) {
                Value l = locs.at(i);
                res.miniLocations.emplace_back(int(l.at(0).number()), int(l.at(1).number()));
            }
            Value sl = r.get("sliders");
            for (uint32_t i = 0, n = JS_IsArray(sl.v) ? sl.length() : 0; i < n; ++i) {
                Value s = sl.at(i);
                EvalResult::Slider x;
                x.from = int(s.get("from").number());
                x.to = int(s.get("to").number());
                x.value = s.get("value").string();
                const double mn = s.get("min").number(), mx = s.get("max").number(), st = s.get("step").number();
                x.min = std::isnan(mn) ? 0.0 : mn;
                x.max = std::isnan(mx) ? 1.0 : mx;
                x.step = std::isnan(st) ? 0.0 : st;
                res.sliders.push_back(std::move(x));
            }
        }
    }
    I.disarm();
    I.drainLog();
    res.milliseconds = msSince(t0);
    return res;
}

bool PatternEngine::query(double begin, double end, double cps, std::vector<PatternEvent>& out, std::string* error)
{
    out.clear();
    if (!ok()) {
        if (error) *error = impl_->initError;
        return false;
    }
    auto& I = *impl_;
    JSContext* ctx = I.ctx;
    I.arm(I.options.queryTimeout);
    JSValue args[3] = {JS_NewFloat64(ctx, begin), JS_NewFloat64(ctx, end), JS_NewFloat64(ctx, cps)};
    Value events(ctx, JS_Call(ctx, I.queryFn, I.concha, 3, args));
    I.disarm();
    if (events.isException()) {
        const std::string msg = I.exceptionText();
        if (error) *error = msg;
        I.log.push_back({"error", "[query] " + msg});
        return false;
    }
    const uint32_t n = events.length();
    out.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        Value e = events.at(i);
        PatternEvent& pe = out[i];
        pe.kind = e.get("kind").string();
        pe.begin = e.get("begin").number();
        pe.end = e.get("end").number();
        pe.duration = e.get("dur").number(pe.end - pe.begin);
        Value value = e.get("value");
        if (JS_IsObject(value.v)) I.readParams(value.v, pe.params);
        Value locs = e.get("locs");
        const uint32_t nl = locs.length();
        pe.locations.reserve(nl);
        for (uint32_t k = 0; k < nl; ++k) {
            Value l = locs.at(k);
            pe.locations.emplace_back(int(l.at(0).number()), int(l.at(1).number()));
        }
    }
    I.drainLog();
    return true;
}

std::vector<std::string> PatternEngine::showHaps(double begin, double end)
{
    std::vector<std::string> out;
    if (!ok()) return out;
    auto& I = *impl_;
    JSContext* ctx = I.ctx;
    I.arm(I.options.queryTimeout);
    Value fn(ctx, JS_GetPropertyStr(ctx, I.concha, "showHaps"));
    JSValue args[2] = {JS_NewFloat64(ctx, begin), JS_NewFloat64(ctx, end)};
    Value arr(ctx, JS_Call(ctx, fn.v, I.concha, 2, args));
    I.disarm();
    if (arr.isException()) {
        I.log.push_back({"error", "[showHaps] " + I.exceptionText()});
        return out;
    }
    for (uint32_t i = 0, n = arr.length(); i < n; ++i) out.push_back(arr.at(i).string());
    return out;
}

bool PatternEngine::evalJson(std::string_view expression, std::string& json, std::string* error)
{
    if (!ok()) {
        if (error) *error = impl_->initError;
        return false;
    }
    auto& I = *impl_;
    JSContext* ctx = I.ctx;
    I.arm(std::chrono::milliseconds(600000));
    const std::string code(expression);
    Value v(ctx, JS_Eval(ctx, code.c_str(), code.size(), "<evalJson>", JS_EVAL_TYPE_GLOBAL));
    bool ok = false;
    std::string err;
    if (v.isException()) {
        err = I.exceptionText();
    } else {
        JSValue result = JS_UNDEFINED;
        if (I.settle(v.v, result, err)) {
            Value r(ctx, result);
            Value s(ctx, JS_JSONStringify(ctx, r.v, JS_UNDEFINED, JS_UNDEFINED));
            if (s.isException()) err = I.exceptionText();
            else {
                json = s.string();
                ok = true;
            }
        }
    }
    I.disarm();
    if (!ok && error) *error = err;
    return ok;
}

void PatternEngine::setSampleRate(double sampleRate)
{
    if (sampleRate > 0) impl_->sampleRate.store(sampleRate);
}

void PatternEngine::setSlider(std::string_view id, double value)
{
    if (!ok()) return;
    auto& I = *impl_;
    JS_UpdateStackTop(I.rt);
    Value fn(I.ctx, JS_GetPropertyStr(I.ctx, I.concha, "setSlider"));
    JSValue args[2] = {JS_NewStringLen(I.ctx, id.data(), id.size()), JS_NewFloat64(I.ctx, value)};
    Value r(I.ctx, JS_Call(I.ctx, fn.v, I.concha, 2, args));
    JS_FreeValue(I.ctx, args[0]);
}

std::vector<LogMessage> PatternEngine::takeLog()
{
    std::vector<LogMessage> out;
    out.swap(impl_->log);
    return out;
}

} // namespace concha
