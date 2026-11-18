#include "superdough/Kabel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>

namespace concha::sd::kabel {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kISR = 1.0 / 48000.0; // ugens.js ISR: fixed, whatever the sample rate

// ---------------------------------------------------------------------------------------------
// minimal JSON (the object js/src/kabel.mjs writes)
struct Json {
    enum Kind { Null, Num, Str, Arr, Obj } kind = Null;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json* get(const char* k) const
    {
        for (const auto& [key, v] : obj)
            if (key == k) return &v;
        return nullptr;
    }
};

struct JsonReader {
    const std::string& s;
    size_t i = 0;
    bool ok = true;
    void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
    std::string string()
    {
        std::string out;
        ++i;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\' && i < s.size()) {
                const char e = s[i++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        const unsigned cp = unsigned(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
                        i += 4;
                        if (cp < 0x80) out += char(cp);
                        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
                        else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
                        break;
                    }
                    default: out += e;
                }
            } else {
                out += c;
            }
        }
        ++i;
        return out;
    }
    Json value()
    {
        ws();
        Json j;
        if (i >= s.size()) { ok = false; return j; }
        const char c = s[i];
        if (c == '{') {
            j.kind = Json::Obj;
            ++i;
            for (;;) {
                ws();
                if (i < s.size() && s[i] == '}') { ++i; break; }
                if (i >= s.size() || s[i] != '"') { ok = false; break; }
                std::string k = string();
                ws();
                if (i < s.size() && s[i] == ':') ++i;
                j.obj.emplace_back(std::move(k), value());
                ws();
                if (i < s.size() && s[i] == ',') ++i;
            }
        } else if (c == '[') {
            j.kind = Json::Arr;
            ++i;
            for (;;) {
                ws();
                if (i < s.size() && s[i] == ']') { ++i; break; }
                if (i >= s.size()) { ok = false; break; }
                j.arr.push_back(value());
                ws();
                if (i < s.size() && s[i] == ',') ++i;
            }
        } else if (c == '"') {
            j.kind = Json::Str;
            j.str = string();
        } else if (c == 'n') {
            i += 4;
        } else {
            j.kind = Json::Num;
            char* end = nullptr;
            j.num = std::strtod(s.c_str() + i, &end);
            if (end == s.c_str() + i) { ok = false; ++i; }
            else i = size_t(end - s.c_str());
        }
        return j;
    }
};

// ---------------------------------------------------------------------------------------------
// bytecode
enum Op : uint8_t {
    OpConst, OpLoadR, OpStoreR, OpLoadO, OpStoreO, OpLoadS, OpStoreS, OpTime, OpInput,
    OpNeg, OpNot, OpPos, OpAdd, OpSub, OpMul, OpDiv, OpMod, OpPow, OpLt, OpGt, OpLe, OpGe,
    OpSEq, OpSNe, OpEq, OpNe, OpJmp, OpJmpFalse, OpAndJmp, OpOrJmp, OpMath, OpCall, OpPair, OpIndex
};

enum MathFn : uint8_t { MSin, MCos, MTan, MAsin, MAcos, MAtan, MExp, MLog, MAbs, MRound, MFloor, MCeil, MSign, MMin, MMax, MPow, MSqrt, MTanh, MLog10, MLog2, MTrunc, MAtan2, MHypot };

const std::map<std::string, MathFn>& mathFns()
{
    static const std::map<std::string, MathFn> m = {
        {"sin", MSin}, {"cos", MCos}, {"tan", MTan}, {"asin", MAsin}, {"acos", MAcos}, {"atan", MAtan},
        {"exp", MExp}, {"log", MLog}, {"abs", MAbs}, {"round", MRound}, {"floor", MFloor}, {"ceil", MCeil},
        {"sign", MSign}, {"min", MMin}, {"max", MMax}, {"pow", MPow}, {"sqrt", MSqrt}, {"tanh", MTanh},
        {"log10", MLog10}, {"log2", MLog2}, {"trunc", MTrunc}, {"atan2", MAtan2}, {"hypot", MHypot}};
    return m;
}

const std::map<std::string, Ugen>& ugenNames()
{
    static const std::map<std::string, Ugen> m = {
        {"ADSRNode", Ugen::ADSR}, {"Clock", Ugen::Clock}, {"ClockDiv", Ugen::ClockDiv}, {"ClockOut", Ugen::ClockOut},
        {"Delay", Ugen::Delay}, {"Distort", Ugen::Distort}, {"Hold", Ugen::Hold}, {"Output", Ugen::Output},
        {"LcgNoise", Ugen::LcgNoise}, {"NoiseOsc", Ugen::Noise}, {"DustOsc", Ugen::Dust}, {"BrownNoiseOsc", Ugen::Brown},
        {"PinkNoise", Ugen::Pink}, {"ImpulseOsc", Ugen::Impulse}, {"PulseOsc", Ugen::Pulse}, {"ZawOsc", Ugen::Zaw},
        {"SawOsc", Ugen::Saw}, {"SineOsc", Ugen::Sine}, {"SidechainCompressor", Ugen::Sidechain}, {"TriOsc", Ugen::Tri},
        {"Scope", Ugen::Scope}, {"Lag", Ugen::Lag}, {"Slew", Ugen::Slew}, {"Slide", Ugen::Slide}, {"Filter", Ugen::Filter},
        {"BPF", Ugen::BPF}, {"Fold", Ugen::Fold}, {"AudioIn", Ugen::AudioIn}, {"MidiGate", Ugen::MidiGate},
        {"MidiFreq", Ugen::MidiFreq}, {"MidiVel", Ugen::MidiVel}, {"Signal", Ugen::Signal}, {"CC", Ugen::CC},
        {"MidiCC", Ugen::MidiCC}, {"Sequence", Ugen::Sequence}, {"Pick", Ugen::Pick}, {"Remap", Ugen::Remap},
        {"Clip", Ugen::Clip}, {"Trig", Ugen::Trig}, {"BiquadFilter", Ugen::Biquad}};
    return m;
}

// ---- tokenizer + recursive-descent compiler for the generated update code ----
struct Token {
    enum Kind { End, Num, Str, Ident, Punct } kind = End;
    std::string text;
    double num = 0;
};

class Compiler {
public:
    Compiler(const std::string& src, Program& p) : src_(src), p_(p) { next(); }

    bool run(std::string& error)
    {
        while (tok_.kind != Token::End) {
            if (isPunct(";")) { next(); continue; }
            if (!statement()) {
                error = error_.empty() ? "unsupported code near \"" + tok_.text + "\"" : error_;
                return false;
            }
        }
        return true;
    }

private:
    void skipSpace()
    {
        for (;;) {
            while (pos_ < src_.size() && std::isspace((unsigned char)src_[pos_])) ++pos_;
            if (src_.compare(pos_, 2, "/*") == 0) {
                const size_t e = src_.find("*/", pos_ + 2);
                pos_ = e == std::string::npos ? src_.size() : e + 2;
            } else if (src_.compare(pos_, 2, "//") == 0) {
                const size_t e = src_.find('\n', pos_);
                pos_ = e == std::string::npos ? src_.size() : e + 1;
            } else {
                return;
            }
        }
    }
    void next()
    {
        skipSpace();
        tok_ = Token{};
        if (pos_ >= src_.size()) return;
        const char c = src_[pos_];
        if (std::isdigit((unsigned char)c) || (c == '.' && pos_ + 1 < src_.size() && std::isdigit((unsigned char)src_[pos_ + 1]))) {
            char* end = nullptr;
            tok_.kind = Token::Num;
            tok_.num = std::strtod(src_.c_str() + pos_, &end);
            tok_.text = src_.substr(pos_, size_t(end - (src_.c_str() + pos_)));
            pos_ = size_t(end - src_.c_str());
            return;
        }
        if (std::isalpha((unsigned char)c) || c == '_' || c == '$') {
            size_t e = pos_;
            while (e < src_.size() && (std::isalnum((unsigned char)src_[e]) || src_[e] == '_' || src_[e] == '$')) ++e;
            tok_.kind = Token::Ident;
            tok_.text = src_.substr(pos_, e - pos_);
            pos_ = e;
            return;
        }
        if (c == '"' || c == '\'') {
            size_t e = pos_ + 1;
            std::string sv;
            while (e < src_.size() && src_[e] != c) {
                if (src_[e] == '\\' && e + 1 < src_.size()) ++e;
                sv += src_[e++];
            }
            tok_.kind = Token::Str;
            tok_.text = sv;
            pos_ = e + 1;
            return;
        }
        static const char* puncts[] = {"===", "!==", "**", "==", "!=", "<=", ">=", "&&", "||", "+", "-", "*", "/", "%",
                                       "<", ">", "!", "?", ":", "(", ")", "[", "]", ",", ".", ";", "="};
        for (const char* p : puncts) {
            const size_t n = std::strlen(p);
            if (src_.compare(pos_, n, p) == 0) {
                tok_.kind = Token::Punct;
                tok_.text = p;
                pos_ += n;
                return;
            }
        }
        tok_.kind = Token::Punct;
        tok_.text = std::string(1, c);
        ++pos_;
    }
    bool isPunct(const char* p) const { return tok_.kind == Token::Punct && tok_.text == p; }
    bool expect(const char* p)
    {
        if (!isPunct(p)) return false;
        next();
        return true;
    }
    void emit(uint8_t op, int32_t arg = 0, uint8_t argc = 0)
    {
        p_.code.push_back(Instr{op, argc, arg});
        depth_ += stackEffect(op, argc);
        p_.maxStack = std::max(p_.maxStack, depth_);
    }
    static int stackEffect(uint8_t op, int argc)
    {
        switch (op) {
            case OpConst: case OpLoadR: case OpLoadO: case OpLoadS: case OpTime: case OpInput: return 1;
            case OpStoreR: case OpStoreO: case OpStoreS: case OpJmpFalse: return -1;
            case OpNeg: case OpNot: case OpPos: case OpJmp: case OpIndex: return 0;
            case OpAndJmp: case OpOrJmp: return -1; // the jump keeps it, the fall-through pops it
            case OpMath: case OpCall: return 1 - argc;
            case OpPair: return -1;
            default: return -1; // binary
        }
    }
    int constant(Value v)
    {
        p_.consts.push_back(v);
        return int(p_.consts.size() - 1);
    }
    // r[N] / o[N] / s[N] index
    bool bracketIndex(int& index)
    {
        if (!expect("[")) return false;
        if (tok_.kind != Token::Num) return false;
        index = int(tok_.num);
        next();
        return expect("]");
    }

    bool statement()
    {
        if (tok_.kind != Token::Ident) return false;
        const std::string target = tok_.text;
        if (target != "r" && target != "o" && target != "s") {
            error_ = "unsupported statement \"" + target + "\" (raw / bytebeat code cannot run here)";
            return false;
        }
        next();
        int index;
        if (!bracketIndex(index) || !expect("=")) return false;
        if (!expression()) return false;
        if (target == "r") {
            if (index < 0 || index >= KabelState::kMaxRegisters) { error_ = "too many nodes"; return false; }
            emit(OpStoreR, index);
        } else if (index < 0 || index >= 16) {
            error_ = "output channel out of range";
            return false;
        } else {
            emit(target == "o" ? OpStoreO : OpStoreS, index);
        }
        return true;
    }
    bool expression() { return ternary(); }
    bool ternary()
    {
        if (!logicalOr()) return false;
        if (!isPunct("?")) return true;
        next();
        const size_t jf = p_.code.size();
        emit(OpJmpFalse);
        if (!ternary()) return false;
        const size_t j = p_.code.size();
        emit(OpJmp);
        --depth_; // the branches leave one value between them
        p_.code[jf].arg = int32_t(p_.code.size());
        if (!expect(":")) return false;
        if (!ternary()) return false;
        p_.code[j].arg = int32_t(p_.code.size());
        return true;
    }
    bool logicalOr()
    {
        if (!logicalAnd()) return false;
        while (isPunct("||")) {
            next();
            const size_t j = p_.code.size();
            emit(OpOrJmp);
            if (!logicalAnd()) return false;
            p_.code[j].arg = int32_t(p_.code.size());
        }
        return true;
    }
    bool logicalAnd()
    {
        if (!equality()) return false;
        while (isPunct("&&")) {
            next();
            const size_t j = p_.code.size();
            emit(OpAndJmp);
            if (!equality()) return false;
            p_.code[j].arg = int32_t(p_.code.size());
        }
        return true;
    }
    bool equality()
    {
        if (!relational()) return false;
        for (;;) {
            uint8_t op;
            if (isPunct("===")) op = OpSEq;
            else if (isPunct("!==")) op = OpSNe;
            else if (isPunct("==")) op = OpEq;
            else if (isPunct("!=")) op = OpNe;
            else return true;
            next();
            if (!relational()) return false;
            emit(op);
        }
    }
    bool relational()
    {
        if (!additive()) return false;
        for (;;) {
            uint8_t op;
            if (isPunct("<")) op = OpLt;
            else if (isPunct(">")) op = OpGt;
            else if (isPunct("<=")) op = OpLe;
            else if (isPunct(">=")) op = OpGe;
            else return true;
            next();
            if (!additive()) return false;
            emit(op);
        }
    }
    bool additive()
    {
        if (!multiplicative()) return false;
        for (;;) {
            uint8_t op;
            if (isPunct("+")) op = OpAdd;
            else if (isPunct("-")) op = OpSub;
            else return true;
            next();
            if (!multiplicative()) return false;
            emit(op);
        }
    }
    bool multiplicative()
    {
        if (!exponent()) return false;
        for (;;) {
            uint8_t op;
            if (isPunct("*")) op = OpMul;
            else if (isPunct("/")) op = OpDiv;
            else if (isPunct("%")) op = OpMod;
            else return true;
            next();
            if (!exponent()) return false;
            emit(op);
        }
    }
    bool exponent()
    {
        if (!unary()) return false;
        if (isPunct("**")) { // right associative
            next();
            if (!exponent()) return false;
            emit(OpPow);
        }
        return true;
    }
    bool unary()
    {
        if (isPunct("-")) { next(); if (!unary()) return false; emit(OpNeg); return true; }
        if (isPunct("+")) { next(); if (!unary()) return false; emit(OpPos); return true; }
        if (isPunct("!")) { next(); if (!unary()) return false; emit(OpNot); return true; }
        return postfix();
    }
    bool postfix()
    {
        if (!primary()) return false;
        while (isPunct("[")) { // pair[0] / pair[1]
            next();
            if (tok_.kind != Token::Num) return false;
            const int k = int(tok_.num);
            next();
            if (!expect("]")) return false;
            emit(OpIndex, k);
        }
        return true;
    }
    bool arguments(int& argc)
    {
        argc = 0;
        if (!expect("(")) return false;
        if (isPunct(")")) { next(); return true; }
        for (;;) {
            if (!expression()) return false;
            ++argc;
            if (isPunct(",")) { next(); continue; }
            return expect(")");
        }
    }
    bool primary()
    {
        if (tok_.kind == Token::Num) {
            emit(OpConst, constant(Value{tok_.num, 0, Value::Number}));
            next();
            return true;
        }
        if (tok_.kind == Token::Str) {
            char* end = nullptr;
            const std::string& t = tok_.text;
            double num = t.empty() ? 0.0 : std::strtod(t.c_str(), &end);
            if (!t.empty() && (end == t.c_str() || *end != 0)) num = kNaN; // Number("abc")
            emit(OpConst, constant(Value{num, t.empty() ? 0.0 : 1.0, Value::String}));
            next();
            return true;
        }
        if (isPunct("(")) {
            next();
            if (!expression()) return false;
            return expect(")");
        }
        if (isPunct("[")) { // [a, b]: argmin / argmax pairs
            next();
            if (!expression() || !expect(",") || !expression() || !expect("]")) return false;
            emit(OpPair);
            return true;
        }
        if (tok_.kind != Token::Ident) return false;
        const std::string id = tok_.text;
        next();
        if (id == "true" || id == "false") { emit(OpConst, constant(Value{id == "true" ? 1.0 : 0.0, 0, Value::Bool})); return true; }
        if (id == "undefined") { emit(OpConst, constant(Value{kNaN, 0, Value::Undefined})); return true; }
        if (id == "Infinity") { emit(OpConst, constant(Value{std::numeric_limits<double>::infinity(), 0, Value::Number})); return true; }
        if (id == "NaN") { emit(OpConst, constant(Value{kNaN, 0, Value::Number})); return true; }
        if (id == "time") { emit(OpTime); return true; }
        if (id == "input") { emit(OpInput); return true; }
        if (id == "r" || id == "o" || id == "s") {
            int index;
            if (!bracketIndex(index)) return false;
            if (id == "r" && (index < 0 || index >= KabelState::kMaxRegisters)) { error_ = "too many nodes"; return false; }
            if (id != "r" && (index < 0 || index >= 16)) return false;
            emit(id == "r" ? OpLoadR : id == "o" ? OpLoadO : OpLoadS, index);
            return true;
        }
        if (id == "Math") {
            if (!expect(".") || tok_.kind != Token::Ident) return false;
            const std::string fn = tok_.text;
            next();
            if (fn == "PI") { emit(OpConst, constant(Value{kPi, 0, Value::Number})); return true; }
            const auto it = mathFns().find(fn);
            if (it == mathFns().end()) { error_ = "Math." + fn + " is not supported"; return false; }
            int argc;
            if (!arguments(argc)) return false;
            emit(OpMath, it->second, uint8_t(argc));
            return true;
        }
        if (id == "nodes") {
            int index;
            if (!bracketIndex(index) || !expect(".") || tok_.kind != Token::Ident || tok_.text != "update") return false;
            next();
            int argc;
            if (!arguments(argc)) return false;
            if (index < 0 || index >= int(p_.ugens.size())) { error_ = "unknown ugen"; return false; }
            emit(OpCall, index, uint8_t(argc));
            return true;
        }
        error_ = "unsupported name \"" + id + "\"";
        return false;
    }

    const std::string& src_;
    Program& p_;
    size_t pos_ = 0;
    Token tok_;
    int depth_ = 0;
    std::string error_;
};

// ---- JavaScript value semantics ----
inline double num(const Value& v) { return v.kind == Value::Pair || v.kind == Value::Undefined ? kNaN : v.a; }
inline bool truthy(const Value& v)
{
    switch (v.kind) {
        case Value::Number: return v.a != 0 && !std::isnan(v.a);
        case Value::Bool: return v.a != 0;
        case Value::Pair: return true;
        case Value::String: return v.b != 0; // non-empty
        default: return false;
    }
}
inline Value number(double x) { return Value{x, 0, Value::Number}; }
inline Value boolean(bool x) { return Value{x ? 1.0 : 0.0, 0, Value::Bool}; }
inline bool strictEqual(const Value& x, const Value& y)
{
    if (x.kind != y.kind) return false;
    if (x.kind == Value::Undefined) return true;
    if (x.kind == Value::Pair) return false; // different arrays
    return x.a == y.a && (x.kind != Value::String || x.b == y.b);
}
inline bool looseEqual(const Value& x, const Value& y)
{
    if (x.kind == y.kind) return strictEqual(x, y);
    if (x.kind == Value::Undefined || y.kind == Value::Undefined) return false;
    return num(x) == num(y);
}
inline double jsRound(double x) { return std::floor(x + 0.5); }
inline double jsSign(double x) { return std::isnan(x) ? x : (x > 0 ? 1.0 : (x < 0 ? -1.0 : x)); }
inline double lerpUtil(double x, double y0, double y1) // utils.js lerp
{
    if (x <= 0) return y0;
    if (x >= 1) return y1;
    return y0 + x * (y1 - y0);
}
inline double invLerp(double x, double y0, double y1)
{
    if (x <= y0) return 0;
    if (x >= y1) return 1;
    if (y1 == y0) return 0;
    return (x - y0) / (y1 - y0);
}
inline double lerpSynth(double x, double y0, double y1) // synth.js lerp
{
    if (x >= 1) return y1;
    return y0 + x * (y1 - y0);
}
inline double polyBlep(double phase, double dt)
{
    if (phase < dt) {
        phase /= dt;
        return phase + phase - phase * phase - 1;
    }
    if (phase > 1 - dt) {
        phase = (phase - 1) / dt;
        return phase * phase + phase + phase + 1;
    }
    return 0;
}
inline double fmodJs(double a, double b) { return std::fmod(a, b); }

std::atomic<uint32_t> gNoiseChannels{0}; // ugens.js NOISE_CHANNELS: never reset

} // namespace

// ---------------------------------------------------------------------------------------------
KabelStore& KabelStore::instance()
{
    static KabelStore s;
    return s;
}

uint32_t KabelStore::getOrBuild(const std::string& json, double sampleRate, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [k, id] : keys_)
        if (k == json) {
            DelayPool::instance().reserve(get(id) ? get(id)->delays * 8 : 0, sampleRate);
            return id;
        }
    if (owned_.size() + 1 >= kMax) {
        error = "too many different K() graphs";
        return 0;
    }
    JsonReader reader{json};
    const Json root = reader.value();
    const Json* src = root.get("src");
    const Json* ugens = root.get("ugens");
    const Json* registers = root.get("registers");
    if (!reader.ok || !src || src->kind != Json::Str || !ugens || ugens->kind != Json::Arr) {
        error = "invalid program";
        return 0;
    }
    auto p = std::make_unique<Program>();
    p->registers = registers ? int(registers->num) : 0;
    if (ugens->arr.size() > size_t(KabelState::kMaxUgens)) {
        error = "the graph has too many nodes";
        return 0;
    }
    for (const Json& u : ugens->arr) {
        UgenDesc d;
        const Json* type = u.get("type");
        const Json* inputs = u.get("inputs");
        const std::string t = type ? type->str : "";
        const auto it = ugenNames().find(t);
        if (it == ugenNames().end()) {
            error = "ugen " + t + " is not supported";
            return 0;
        }
        d.type = it->second;
        auto input = [&](size_t k, double fallback) {
            if (!inputs || k >= inputs->arr.size()) return fallback;
            const std::string& s = inputs->arr[k].str;
            char* end = nullptr;
            const double v = std::strtod(s.c_str(), &end);
            return end != s.c_str() && *end == 0 ? v : kNaN;
        };
        if (d.type == Ugen::CC) {
            d.ccInit = input(1, 0.0);
            d.gate = inputs && !inputs->arr.empty() && inputs->arr[0].str.find("strudel-gate") != std::string::npos;
        } else if (d.type == Ugen::MidiCC) {
            d.ccInit = input(2, -1.0);
        }
        if (d.type == Ugen::Delay) ++p->delays;
        p->ugens.push_back(d);
    }
    Compiler compiler(src->str, *p);
    if (!compiler.run(error)) return 0;
    if (p->maxStack > KabelState::kMaxStack) {
        error = "the expression is too deep";
        return 0;
    }
    DelayPool::instance().reserve(p->delays * 8, sampleRate);
    const uint32_t id = uint32_t(owned_.size() + 1);
    slots_[id].store(p.get(), std::memory_order_release);
    owned_.push_back(std::move(p));
    keys_.emplace_back(json, id);
    return id;
}

// ---------------------------------------------------------------------------------------------
DelayPool& DelayPool::instance()
{
    static DelayPool p;
    return p;
}

void DelayPool::reserve(int count, double sampleRate)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t frames = int64_t(10 * sampleRate);
    count = std::min(count, kMax);
    // grow, and replace buffers too short for this rate (only when free)
    for (int i = 0; i < count_; ++i) {
        Slot& s = buffers_[size_t(i)];
        if (s.length.load() >= frames) continue;
        bool expected = false;
        if (!s.busy.compare_exchange_strong(expected, true)) continue;
        owned_.push_back(std::make_unique<float[]>(size_t(frames)));
        std::fill_n(owned_.back().get(), frames, 0.f);
        s.length.store(0);
        s.data.store(owned_.back().get());
        s.length.store(frames);
        s.busy.store(false);
    }
    while (count_ < count) {
        owned_.push_back(std::make_unique<float[]>(size_t(frames)));
        std::fill_n(owned_.back().get(), frames, 0.f);
        Slot& s = buffers_[size_t(count_)];
        s.data.store(owned_.back().get());
        s.length.store(frames, std::memory_order_release);
        ++count_;
    }
}

int DelayPool::acquire(int64_t frames)
{
    for (int i = 0; i < kMax; ++i) {
        Slot& s = buffers_[size_t(i)];
        if (s.length.load(std::memory_order_acquire) < frames) continue;
        bool expected = false;
        if (s.busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            if (s.length.load(std::memory_order_acquire) >= frames) return i;
            s.busy.store(false);
        }
    }
    return -1;
}

void DelayPool::release(int index, int64_t written)
{
    if (index < 0) return;
    Slot& s = buffers_[size_t(index)];
    const int64_t len = s.length.load();
    float* d = s.data.load();
    if (written >= len) std::fill_n(d, len, 0.f);
    else std::fill_n(d, std::min(len, written + 1), 0.f); // writes start at index 1
    s.busy.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------------------------
double KabelState::random()
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return double(rng_) / 4294967296.0;
}

void KabelState::start(const Program* program, double sampleRate, uint32_t seed)
{
    stop();
    program_ = program;
    sampleRate_ = sampleRate;
    time_ = 0;
    gateValue_ = 1;
    rng_ = seed ? seed : 1;
    failed_ = false;
    const int nr = std::min(program->registers, kMaxRegisters);
    for (int i = 0; i < nr; ++i) r_[size_t(i)] = Value{};
    for (auto& v : s_) v = Value{};
    const int64_t delayFrames = int64_t(10 * sampleRate);
    for (size_t k = 0; k < program->ugens.size(); ++k) {
        UgenState& u = u_[k];
        u = UgenState{};
        const UgenDesc& d = program->ugens[k];
        switch (d.type) {
            case Ugen::ClockDiv: u.sgn = true; u.sgn2 = true; break; // inSgn, outSgn
            case Ugen::Impulse: u.phase = 1; break;
            case Ugen::Saw: u.phase = random(); break;
            case Ugen::Noise: u.value = random() * 2 - 1; break;
            case Ugen::Sequence: u.sgn = true; break; // clockSgn
            case Ugen::Lag: u.s = 0; break;
            case Ugen::CC: case Ugen::MidiCC: u.value = d.gate ? 1.0 : d.ccInit; break;
            case Ugen::LcgNoise: {
                const uint32_t ch = gNoiseChannels.fetch_add(1);
                u.lcgStart = uint32_t(340u * (ch + 1u));
                u.lcg = u.lcgStart;
                break;
            }
            case Ugen::Delay: {
                u.delay = DelayPool::instance().acquire(delayFrames);
                if (u.delay < 0) DelayPool::instance().missed();
                u.length = u.delay >= 0 ? DelayPool::instance().length(u.delay) : 0;
                if (u.length > delayFrames) u.length = delayFrames; // the buffer kabelsalat would have
                break;
            }
            case Ugen::Scope: u.length = int64_t(sampleRate / 30); break;
            case Ugen::Biquad: u.b[4] = 1; break; // b0 = 1
            default: break;
        }
    }
}

void KabelState::stop()
{
    if (!program_) return;
    for (size_t k = 0; k < program_->ugens.size(); ++k) {
        UgenState& u = u_[k];
        if (program_->ugens[k].type == Ugen::Delay && u.delay >= 0) {
            DelayPool::instance().release(u.delay, u.written);
            u.delay = -1;
        }
    }
    program_ = nullptr;
}

double KabelState::update(int index, const Value* args, int argc)
{
    UgenState& u = u_[size_t(index)];
    const UgenDesc& d = program_->ugens[size_t(index)];
    auto arg = [&](int k) { return k < argc ? num(args[k]) : kNaN; };
    const double sampleTime = 1.0 / sampleRate_;
    switch (d.type) {
        case Ugen::ADSR: { // synth.js ADSREnv.eval(curTime, gate, attack, decay, susVal, release)
            const double curTime = arg(0), gate = arg(1), attack = arg(2), decay = arg(3), sus = arg(4), release = arg(5);
            switch (u.state) {
                case 0: // off
                    if (gate > 0) { u.state = 1; u.startTime = curTime; u.startVal = 0; }
                    return 0;
                case 1: { // attack
                    const double t = curTime - u.startTime;
                    if (t > attack) { u.state = 2; u.startTime = curTime; return 1; }
                    return lerpSynth(t / attack, u.startVal, 1);
                }
                case 2: { // decay
                    const double t = curTime - u.startTime;
                    const double cur = lerpSynth(t / decay, 1, sus);
                    if (gate <= 0) { u.state = 4; u.startTime = curTime; u.startVal = cur; return cur; }
                    if (t > decay) { u.state = 3; u.startTime = curTime; return sus; }
                    return cur;
                }
                case 3: // sustain
                    if (gate <= 0) { u.state = 4; u.startTime = curTime; u.startVal = sus; }
                    return sus;
                default: { // release
                    const double t = curTime - u.startTime;
                    if (t > release) { u.state = 0; return 0; }
                    const double cur = lerpSynth(t / release, u.startVal, 0);
                    if (gate > 0) { u.state = 1; u.startTime = curTime; u.startVal = cur; }
                    return cur;
                }
            }
        }
        case Ugen::Clock: {
            const double freq = (24 * arg(0)) / 60;
            u.phase += sampleTime * freq;
            return fmodJs(u.phase, 1) < 0.5 ? 1 : -1;
        }
        case Ugen::ClockDiv: {
            const bool cur = arg(0) > 0;
            if (u.sgn != cur) {
                ++u.count;
                if (u.count >= arg(1)) { u.count = 0; u.sgn2 = !u.sgn2; }
            }
            u.sgn = cur;
            return u.sgn2 ? 1 : -1;
        }
        case Ugen::ClockOut: {
            const bool cur = arg(1) > 0;
            if (cur && u.sgn != cur) failed_ = true; // this.send is undefined in the worklet: throws
            u.sgn = cur;
            return 0;
        }
        case Ugen::Delay: { // synth.Delay write + read
            if (u.delay < 0 || u.length <= 0) return 0;
            float* buf = DelayPool::instance().data(u.delay);
            u.writeIdx = (u.writeIdx + 1) % u.length;
            buf[u.writeIdx] = float(arg(0));
            u.written = std::max(u.written, u.writeIdx);
            const double ns = std::floor(sampleRate_ * arg(1));
            if (std::isnan(ns)) return kNaN; // buffer[NaN]: undefined
            const int64_t n = int64_t(std::min(ns, double(u.length - 1)));
            u.readIdx = u.writeIdx - n;
            if (u.readIdx < 0) u.readIdx += u.length;
            if (u.readIdx < 0 || u.readIdx >= u.length) return kNaN; // out of the typed array: undefined
            return buf[u.readIdx];
        }
        case Ugen::Distort: {
            double amount = std::min(std::max(arg(1), 0.0), 1.0);
            amount -= 0.01;
            const double k = (2 * amount) / (1 - amount);
            const double x = arg(0);
            return ((1 + k) * x) / (1 + k * std::fabs(x));
        }
        case Ugen::Hold: {
            const double trig = arg(1);
            if (!u.sgn && trig > 0) u.value = arg(0);
            u.sgn = trig > 0;
            return u.value;
        }
        case Ugen::Output: u.value = arg(0); return u.value;
        case Ugen::LcgNoise: {
            const Value next = argc > 0 ? args[0] : Value{kNaN, 0, Value::Undefined};
            if (!truthy(next)) return u.value;
            if (argc > 1 && truthy(args[1])) u.lcg = u.lcgStart;
            u.lcg = u.lcg * 1664525u + 1013904223u;
            const double y = double(u.lcg & 0x00ffffffu) * (1.0 / double(1 << 24));
            u.value = y * 2 - 1;
            return u.value;
        }
        case Ugen::Noise: {
            if (!(argc > 0 && truthy(args[0]))) return u.value;
            u.value = random() * 2 - 1;
            return u.value;
        }
        case Ugen::Dust: return random() < arg(0) * kISR ? random() : 0;
        case Ugen::Brown: {
            const double white = random() * 2 - 1;
            u.s = (u.s + 0.02 * white) / 1.02;
            return u.s;
        }
        case Ugen::Pink: {
            double* b = u.b;
            const double white = random() * 2 - 1;
            b[0] = 0.99886 * b[0] + white * 0.0555179;
            b[1] = 0.99332 * b[1] + white * 0.0750759;
            b[2] = 0.969 * b[2] + white * 0.153852;
            b[3] = 0.8665 * b[3] + white * 0.3104856;
            b[4] = 0.55 * b[4] + white * 0.5329522;
            b[5] = -0.7616 * b[5] - white * 0.016898;
            const double pink = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + white * 0.5362;
            b[6] = white * 0.115926;
            return pink * 0.11;
        }
        case Ugen::Impulse: {
            u.phase += sampleTime * arg(0);
            const double v = u.phase >= 1 ? 1 : 0;
            u.phase = fmodJs(u.phase, 1);
            return v;
        }
        case Ugen::Pulse: {
            u.phase += sampleTime * arg(0);
            return fmodJs(u.phase, 1) < arg(1) ? 1 : -1;
        }
        case Ugen::Zaw: {
            u.phase += sampleTime * arg(0);
            return fmodJs(u.phase, 1) * 2 - 1;
        }
        case Ugen::Saw: {
            const double dt = arg(0) / sampleRate_;
            const double p = polyBlep(u.phase, dt);
            const double s = 2 * u.phase - 1 - p;
            u.phase += dt;
            if (u.phase > 1) u.phase -= 1;
            return s;
        }
        case Ugen::Sine: {
            const double sync = arg(1);
            if (!u.sgn && sync > 0) u.phase = 0;
            u.sgn = sync > 0;
            const double cyclePos = fmodJs(u.phase + arg(2), 1);
            u.phase += sampleTime * arg(0);
            return std::sin(cyclePos * 2 * kPi);
        }
        case Ugen::Sidechain: {
            const double inDb = 20 * std::log10(std::fabs(arg(0)));
            double red = 0;
            if (inDb > arg(1)) red = (inDb - arg(1)) * (1 - 1 / arg(2));
            return std::pow(10, -red / 20);
        }
        case Ugen::Tri: {
            u.phase += sampleTime * arg(0);
            const double c = fmodJs(u.phase, 1);
            const double n = c < 0.5 ? 2 * c : 1 - 2 * (c - 0.5);
            return n * 2 - 1;
        }
        case Ugen::Scope: { // sends a buffer every 1/30 s: this.send is undefined, it throws then
            const double input = arg(0);
            if (++u.writeIdx % std::max<int64_t>(1, u.length) == 0) failed_ = true;
            return input;
        }
        case Ugen::Lag: {
            double rate = arg(1) * 4410;
            if (rate < 1) rate = 1;
            u.s += (1 / rate) * (arg(0) - u.s);
            return u.s;
        }
        case Ugen::Slew: {
            const double upStep = arg(1) * kISR, downStep = arg(2) * kISR;
            double delta = arg(0) - u.last;
            if (delta > upStep) delta = upStep;
            else if (delta < -downStep) delta = -downStep;
            u.last += delta;
            return u.last;
        }
        case Ugen::Slide: {
            double rate = arg(1) * 1000;
            if (rate < 1) rate = 1;
            u.s += (1 / rate) * (arg(0) - u.s);
            return u.s;
        }
        case Ugen::Filter: case Ugen::BPF: { // synth.js TwoPoleFilter
            const double s = arg(0);
            if (std::isnan(s)) { failed_ = true; return 0; } // assert(!isNaN(s)) throws
            const double cutoff = std::min(arg(1), 1.0), reso = std::max(arg(2), 0.0);
            const double c = std::pow(0.5, (1 - cutoff) / 0.125);
            const double r = std::pow(0.5, (reso + 0.125) / 0.125);
            const double mrc = 1 - r * c;
            double v0 = u.b[0], v1 = u.b[1];
            v0 = mrc * v0 - c * v1 + c * s;
            v1 = mrc * v1 + c * v0;
            u.b[0] = v0;
            u.b[1] = v1;
            return d.type == Ugen::Filter ? v1 : v0;
        }
        case Ugen::Fold: {
            double rate = arg(1);
            if (rate < 0) rate = 0;
            rate = rate + 1;
            const double x = arg(0) * rate;
            return 4 * (std::fabs(0.25 * x + 0.25 - jsRound(0.25 * x + 0.25)) - 0.25);
        }
        case Ugen::AudioIn: return arg(0);
        case Ugen::MidiGate: return 0; // no MIDI reaches the worklet
        case Ugen::MidiFreq: case Ugen::MidiVel: return 0;
        case Ugen::Signal: {
            const double input = arg(0);
            if (!u.sgn && input > 0) failed_ = true; // this.send is undefined: throws
            u.sgn = input > 0;
            return u.value;
        }
        case Ugen::CC: return d.gate ? gateValue_ : u.value;
        case Ugen::MidiCC: return u.value;
        case Ugen::Sequence: {
            const int n = argc - 1;
            const double clock = arg(0);
            if (!u.sgn && clock > 0) {
                u.step = n > 0 ? (u.step + 1) % n : -1;
                u.sgn = clock > 0;
                return 0;
            }
            u.sgn = clock > 0;
            return u.step >= 0 && u.step < n ? num(args[1 + u.step]) : kNaN;
        }
        case Ugen::Pick: {
            const int n = argc - 1;
            if (n <= 0) return kNaN;
            const double i = fmodJs(arg(0), n) + n;
            const double k = fmodJs(std::floor(i), n);
            return k >= 0 && k < n ? num(args[1 + int(k)]) : kNaN;
        }
        case Ugen::Remap: return lerpUtil(invLerp(arg(0), arg(1), arg(2)), arg(3), arg(4));
        case Ugen::Clip: return std::min(std::max(arg(0), arg(1)), arg(2));
        case Ugen::Trig: {
            const double input = arg(0);
            if (!u.flag && input > 0) { u.flag = true; return 1; }
            if (u.flag && input <= 0) u.flag = false;
            return 0;
        }
        case Ugen::Biquad: { // ugens.js BiquadFilter (defaults input 0, type 0, freq 500, Q 1, gain 1)
            auto def = [&](int k, double v) { return k < argc && args[k].kind != Value::Undefined ? num(args[k]) : v; };
            const double input = def(0, 0), type = def(1, 0), freq = def(2, 500), gain = def(4, 1);
            double Q = def(3, 1);
            const double omega = (2 * kPi * freq) / sampleRate_;
            const double sw = std::sin(omega), cw = std::cos(omega);
            Q = std::pow(10, Q / 20);
            const double alpha = sw / (2.0 * Q);
            double* c = u.b; // x1 x2 y1 y2 in b[0..3]
            double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
            if (type == 0) { b1 = 1 - cw; b0 = b1 / 2; b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; }
            else if (type == 1) { b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; }
            else if (type == 2) { b0 = sw / 2; b1 = 0; b2 = -b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; }
            else if (type == 3) { b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; }
            else if (type == 4) { b0 = 1 - alpha; b1 = -2 * cw; b2 = 1 + alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; }
            else if (type == 5) {
                const double A = std::pow(10, gain / 40);
                b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
            } else if (type == 6 || type == 7) {
                const double A = std::pow(10, gain / 40), s2 = 2 * std::sqrt(A) * alpha;
                const double am = (A - 1) * cw, ap = (A + 1) * cw;
                if (type == 6) {
                    b0 = A * (A + 1 - am + s2); b1 = 2 * A * (A - 1 - ap); b2 = A * (A + 1 - am - s2);
                    a0 = A + 1 + am + s2; a1 = -2 * (A - 1 + ap); a2 = A + 1 + am - s2;
                } else {
                    b0 = A * (A + 1 + am + s2); b1 = -2 * A * (A - 1 + ap); b2 = A * (A + 1 + am - s2);
                    a0 = A + 1 - am + s2; a1 = 2 * (A - 1 - ap); a2 = A + 1 - am - s2;
                }
            } else { // another type keeps the previous coefficients
                b0 = c[4]; b1 = c[5]; b2 = c[6]; a1 = u.phase; a2 = u.value; a0 = 1;
            }
            b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
            c[4] = b0; c[5] = b1; c[6] = b2; u.phase = a1; u.value = a2;
            const double out = b0 * input + b1 * c[0] + b2 * c[1] - a1 * c[2] - a2 * c[3];
            c[1] = c[0];
            c[0] = input;
            c[3] = c[2];
            c[2] = out;
            return out;
        }
        default: return 0;
    }
}

void KabelState::sample(double input, float& left, float& right)
{
    if (!program_ || failed_) {
        left = right = 0.f;
        return;
    }
    for (auto& v : o_) v = Value{}; // o.fill(0)
    const Program& p = *program_;
    const Instr* code = p.code.data();
    const int n = int(p.code.size());
    Value* st = stack_.data();
    int sp = 0;
    for (int pc = 0; pc < n; ++pc) {
        const Instr& in = code[pc];
        switch (in.op) {
            case OpConst: st[sp++] = p.consts[size_t(in.arg)]; break;
            case OpLoadR: st[sp++] = r_[size_t(in.arg)]; break;
            case OpStoreR: r_[size_t(in.arg)] = st[--sp]; break;
            case OpLoadO: st[sp++] = o_[size_t(in.arg)]; break;
            case OpStoreO: o_[size_t(in.arg)] = st[--sp]; break;
            case OpLoadS: st[sp++] = s_[size_t(in.arg)]; break;
            case OpStoreS: s_[size_t(in.arg)] = st[--sp]; break;
            case OpTime: st[sp++] = number(time_); break;
            case OpInput: st[sp++] = number(input); break;
            case OpNeg: st[sp - 1] = number(-num(st[sp - 1])); break;
            case OpPos: st[sp - 1] = number(num(st[sp - 1])); break;
            case OpNot: st[sp - 1] = boolean(!truthy(st[sp - 1])); break;
            case OpJmp: pc = in.arg - 1; break;
            case OpJmpFalse: if (!truthy(st[--sp])) pc = in.arg - 1; break;
            case OpAndJmp: if (!truthy(st[sp - 1])) pc = in.arg - 1; else --sp; break;
            case OpOrJmp: if (truthy(st[sp - 1])) pc = in.arg - 1; else --sp; break;
            case OpPair: {
                const double b = num(st[--sp]);
                st[sp - 1] = Value{num(st[sp - 1]), b, Value::Pair};
                break;
            }
            case OpIndex: {
                Value& v = st[sp - 1];
                if (v.kind == Value::Pair) v = number(in.arg == 0 ? v.a : in.arg == 1 ? v.b : kNaN);
                else v = Value{kNaN, 0, Value::Undefined};
                break;
            }
            case OpMath: {
                const int argc = in.argc;
                Value* a = st + sp - argc;
                auto x = [&](int k) { return k < argc ? num(a[k]) : kNaN; };
                double res;
                switch (in.arg) {
                    case MSin: res = std::sin(x(0)); break;
                    case MCos: res = std::cos(x(0)); break;
                    case MTan: res = std::tan(x(0)); break;
                    case MAsin: res = std::asin(x(0)); break;
                    case MAcos: res = std::acos(x(0)); break;
                    case MAtan: res = std::atan(x(0)); break;
                    case MExp: res = std::exp(x(0)); break;
                    case MLog: res = std::log(x(0)); break;
                    case MAbs: res = std::fabs(x(0)); break;
                    case MRound: res = jsRound(x(0)); break;
                    case MFloor: res = std::floor(x(0)); break;
                    case MCeil: res = std::ceil(x(0)); break;
                    case MSign: res = jsSign(x(0)); break;
                    case MSqrt: res = std::sqrt(x(0)); break;
                    case MTanh: res = std::tanh(x(0)); break;
                    case MLog10: res = std::log10(x(0)); break;
                    case MLog2: res = std::log2(x(0)); break;
                    case MTrunc: res = std::trunc(x(0)); break;
                    case MPow: res = std::pow(x(0), x(1)); break;
                    case MAtan2: res = std::atan2(x(0), x(1)); break;
                    case MHypot: res = std::hypot(x(0), x(1)); break;
                    case MMin: case MMax: { // NaN wins, as in JavaScript
                        res = in.arg == MMin ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
                        for (int k = 0; k < argc; ++k) {
                            const double v = x(k);
                            if (std::isnan(v)) { res = v; break; }
                            res = in.arg == MMin ? std::min(res, v) : std::max(res, v);
                        }
                        break;
                    }
                    default: res = kNaN;
                }
                sp -= argc;
                st[sp++] = number(res);
                break;
            }
            case OpCall: {
                const int argc = in.argc;
                const double res = update(in.arg, st + sp - argc, argc);
                sp -= argc;
                st[sp++] = number(res);
                break;
            }
            default: { // binary
                const Value b = st[--sp];
                Value& a = st[sp - 1];
                switch (in.op) {
                    case OpAdd: a = number(num(a) + num(b)); break;
                    case OpSub: a = number(num(a) - num(b)); break;
                    case OpMul: a = number(num(a) * num(b)); break;
                    case OpDiv: a = number(num(a) / num(b)); break;
                    case OpMod: a = number(fmodJs(num(a), num(b))); break;
                    case OpPow: a = number(std::pow(num(a), num(b))); break;
                    case OpLt: a = boolean(num(a) < num(b)); break;
                    case OpGt: a = boolean(num(a) > num(b)); break;
                    case OpLe: a = boolean(num(a) <= num(b)); break;
                    case OpGe: a = boolean(num(a) >= num(b)); break;
                    case OpSEq: a = boolean(strictEqual(a, b)); break;
                    case OpSNe: a = boolean(!strictEqual(a, b)); break;
                    case OpEq: a = boolean(looseEqual(a, b)); break;
                    case OpNe: a = boolean(!looseEqual(a, b)); break;
                    default: break;
                }
            }
        }
        if (failed_) break;
    }
    if (failed_) { // the worklet threw: silent from now on
        left = right = 0.f;
        return;
    }
    left = float(num(o_[0]));
    right = float(num(o_[1]));
    time_ += 1.0 / sampleRate_;
}

} // namespace concha::sd::kabel
