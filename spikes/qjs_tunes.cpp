// Runs the Strudel tunes bundle (js/spike/tunes-entry.mjs) inside an embedded QuickJS-ng and
// writes each tune's haps as JSON, for js/spike/compare.mjs.
// usage: spike_qjs_tunes <tunes.js> <results.json> [tune]

extern "C" {
#include "quickjs.h"
}

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

JSValue consoleLog(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    for (int i = 0; i < argc; ++i) {
        if (const char* s = JS_ToCString(ctx, argv[i])) {
            std::fprintf(stderr, "%s%s", i ? " " : "", s);
            JS_FreeCString(ctx, s);
        }
    }
    std::fputc('\n', stderr);
    return JS_UNDEFINED;
}

void printException(JSContext* ctx)
{
    JSValue e = JS_GetException(ctx);
    const char* s = JS_ToCString(ctx, e);
    JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
    const char* st = JS_IsUndefined(stack) ? nullptr : JS_ToCString(ctx, stack);
    std::fprintf(stderr, "exception: %s\n%s\n", s ? s : "?", st ? st : "");
    if (st) JS_FreeCString(ctx, st);
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, e);
}

std::string readFile(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <tunes.js> <results.json> [tune]\n", argv[0]);
        return 2;
    }
    const std::string src = readFile(argv[1]);

    const double t0 = nowMs();
    JSRuntime* rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 8 * 1024 * 1024);
    JSContext* ctx = JS_NewContext(rt);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    for (const char* name : {"log", "warn", "error", "info", "debug"})
        JS_SetPropertyStr(ctx, console, name, JS_NewCFunction(ctx, consoleLog, name, 1));
    JS_SetPropertyStr(ctx, global, "console", console);

    JSValue r = JS_Eval(ctx, src.data(), src.size(), argv[1], JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) { printException(ctx); return 1; }
    JS_FreeValue(ctx, r);
    const double t1 = nowMs();

    std::string call = argc > 3 ? "runTunes(" + std::string("'") + argv[3] + "')" : "runTunes()";
    JSValue promise = JS_Eval(ctx, call.data(), call.size(), "<call>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(promise)) { printException(ctx); return 1; }
    for (;;) {
        JSContext* jobCtx = nullptr;
        const int ret = JS_ExecutePendingJob(rt, &jobCtx);
        if (ret < 0) { printException(jobCtx); return 1; }
        if (ret == 0) break;
    }
    if (JS_PromiseState(ctx, promise) != JS_PROMISE_FULFILLED) {
        std::fprintf(stderr, "runTunes did not finish (state %d)\n", JS_PromiseState(ctx, promise));
        return 1;
    }
    JSValue result = JS_PromiseResult(ctx, promise);
    JSValue json = JS_JSONStringify(ctx, result, JS_UNDEFINED, JS_UNDEFINED);
    size_t len = 0;
    const char* s = JS_ToCStringLen(ctx, &len, json);
    std::ofstream(argv[2], std::ios::binary).write(s, static_cast<std::streamsize>(len));
    const double t2 = nowMs();
    std::fprintf(stderr, "bundle load %.0f ms, tunes %.0f ms, heap %.1f MiB\n", t1 - t0, t2 - t1,
                 [&] { JSMemoryUsage m; JS_ComputeMemoryUsage(rt, &m); return m.memory_used_size / 1048576.0; }());

    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, json);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, promise);
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
