#include "sched/SoundConvert.h"
#include "sc/ScDefs.h"
#include "superdough/Analyser.h"
#include "superdough/FxChain.h"
#include "superdough/Kabel.h"

#include "library/SampleLibrary.h"
#include "superdough/Nodes.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace concha {

namespace {
using sd::SoundParams;

struct NumberField {
    const char* name;
    float SoundParams::*member;
};
#define CONCHA_SD_FIELD(n) {#n, &SoundParams::n},
const NumberField kFields[] = {CONCHA_SD_NUMBER_PARAMS(CONCHA_SD_FIELD)};
#undef CONCHA_SD_FIELD

// Keys read elsewhere (strings, the resolution by sounds.mjs) or without effect on the sound.
const char* const kHandled[] = {"s", "bank", "note", "freq", "duration", "channels", "ftype", "fmwave", "fmenv",
                                "unit", "fft", "uniform", "color", "markcss", "id", "cps", "label", "activeLabel",
                                "i", "value", "lock", "vowel", "distorttype", "tremoloshape", "lpshape", "hpshape",
                                "bpshape", "duckorbit", "duckonset", "duckattack", "duckdepth", "partials", "phases",
                                "warpmode", "wtshape", "warpshape", "lfo", "env", "bmod", "ir", "FX",
                                "psrate", "psdisp", "tilt", "plat", "tremolorate", "waveloss", "squiz", "fshift", "fshiftphase",
                                "fshiftnote", "triode", "krush", "kcutoff", "octer", "octersub", "octersubsub", "ring", "ringf",
                                "ringdf", "distort2", "tsdelay", "xsdelay", "freeze", "comb", "smear", "scram", "binshift", "hbrick",
                                "lbrick", "real", "imag", "enhance", "workletSrc", "workletInputs"};

// A tiny reader for the modulator objects ({"0":{"control":"cutoff","rate":2},...}): flat key/value
// pairs per inner object, in order. Values are numbers or strings.
struct JsonEntry {
    std::string key;
    std::vector<std::pair<std::string, std::string>> fields; // value as text (strings unquoted)
};
std::vector<JsonEntry> modulatorEntries(const std::string& json)
{
    std::vector<JsonEntry> out;
    size_t i = 0;
    auto skip = [&] { while (i < json.size() && std::isspace((unsigned char)json[i])) ++i; };
    auto readString = [&]() {
        std::string sv;
        ++i; // "
        while (i < json.size() && json[i] != '"') {
            if (json[i] == '\\' && i + 1 < json.size()) ++i;
            sv += json[i++];
        }
        ++i;
        return sv;
    };
    skip();
    if (i >= json.size() || json[i] != '{') return out;
    ++i;
    while (i < json.size()) {
        skip();
        if (json[i] == '}') break;
        if (json[i] == ',') { ++i; continue; }
        if (json[i] != '"') break;
        JsonEntry e;
        e.key = readString();
        skip();
        if (json[i] == ':') ++i;
        skip();
        if (json[i] != '{') { // not an object: skip the value
            while (i < json.size() && json[i] != ',' && json[i] != '}') ++i;
            continue;
        }
        ++i;
        while (i < json.size()) {
            skip();
            if (json[i] == '}') { ++i; break; }
            if (json[i] == ',') { ++i; continue; }
            if (json[i] != '"') { ++i; continue; }
            std::string k = readString();
            skip();
            if (json[i] == ':') ++i;
            skip();
            std::string v;
            if (json[i] == '"') v = readString();
            else if (json[i] == '{' || json[i] == '[') { // nested: skip
                int depth = 0;
                do {
                    if (json[i] == '{' || json[i] == '[') ++depth;
                    if (json[i] == '}' || json[i] == ']') --depth;
                    ++i;
                } while (i < json.size() && depth > 0);
            } else {
                while (i < json.size() && json[i] != ',' && json[i] != '}') v += json[i++];
            }
            e.fields.emplace_back(k, v);
        }
        if (e.key != "__ids") out.push_back(std::move(e));
    }
    return out;
}

int fmOperatorKey(const std::string& key, const char* prefix);

// FX: a JSON array of objects -> one parameter list per object (nested values kept as JSON text)
std::vector<std::vector<std::pair<std::string, ParamValue>>> objectArray(const std::string& json)
{
    std::vector<std::vector<std::pair<std::string, ParamValue>>> out;
    size_t i = 0;
    auto skip = [&] { while (i < json.size() && std::isspace((unsigned char)json[i])) ++i; };
    auto readString = [&]() {
        std::string sv;
        ++i;
        while (i < json.size() && json[i] != '"') {
            if (json[i] == '\\' && i + 1 < json.size()) {
                ++i;
                const char c = json[i];
                sv += c == 'n' ? '\n' : c == 't' ? '\t' : c;
                ++i;
                continue;
            }
            sv += json[i++];
        }
        ++i;
        return sv;
    };
    auto rawValue = [&]() { // a nested object / array, as text
        const size_t b = i;
        int depth = 0;
        bool inString = false;
        do {
            const char c = json[i];
            if (inString) {
                if (c == '\\') ++i;
                else if (c == '"') inString = false;
            } else if (c == '"') inString = true;
            else if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') --depth;
            ++i;
        } while (i < json.size() && depth > 0);
        return json.substr(b, i - b);
    };
    skip();
    if (i >= json.size() || json[i] != '[') return out;
    ++i;
    for (;;) {
        skip();
        if (i >= json.size() || json[i] == ']') break;
        if (json[i] == ',') { ++i; continue; }
        if (json[i] != '{') { // not an object: skip it
            while (i < json.size() && json[i] != ',' && json[i] != ']') ++i;
            out.emplace_back();
            continue;
        }
        ++i;
        std::vector<std::pair<std::string, ParamValue>> obj;
        for (;;) {
            skip();
            if (i >= json.size()) break;
            if (json[i] == '}') { ++i; break; }
            if (json[i] == ',') { ++i; continue; }
            if (json[i] != '"') { ++i; continue; }
            std::string key = readString();
            skip();
            if (i < json.size() && json[i] == ':') ++i;
            skip();
            ParamValue v;
            if (json[i] == '"') {
                v.type = ParamValue::Type::String;
                v.string = readString();
            } else if (json[i] == '{' || json[i] == '[') {
                v.type = ParamValue::Type::String;
                v.string = rawValue();
            } else {
                std::string t;
                while (i < json.size() && json[i] != ',' && json[i] != '}') t += json[i++];
                if (t == "true" || t == "false") v.number = t == "true" ? 1 : 0;
                else if (t == "null") continue;
                else v.number = std::strtod(t.c_str(), nullptr);
            }
            obj.emplace_back(std::move(key), std::move(v));
        }
        out.push_back(std::move(obj));
    }
    return out;
}

// superdoughdata.mjs CONTROL_TARGETS, for the targets superdough C++ can modulate
sd::ModTarget modTarget(const std::string& control, const std::string& subControl)
{
    using T = sd::ModTarget;
    static const std::pair<const char*, T> table[] = {
        {"s", T::SourceFrequency}, {"freq", T::SourceFrequency}, {"note", T::SourceFrequency},
        {"gain", T::Gain}, {"postgain", T::Post}, {"pan", T::Pan},
        {"cutoff", T::LpfFrequency}, {"resonance", T::LpfQ}, {"hcutoff", T::HpfFrequency}, {"hresonance", T::HpfQ},
        {"bandf", T::BpfFrequency}, {"bandq", T::BpfQ}, {"tremolo", T::TremoloRate}, {"tremolosync", T::TremoloRate},
        {"tremolodepth", T::TremoloGain}, {"tremoloskew", T::TremoloSkew},
        {"phasercenter", T::PhaserFrequency}, {"phaserdepth", T::PhaserQ},
        {"phaserrate", T::PhaserLfoFreq}, {"phasersweep", T::PhaserLfoDepth},
        {"delay", T::DelaySend}, {"room", T::RoomSend}, {"vib", T::VibRate}, {"vibmod", T::VibDepth},
        {"pw", T::PulseWidth}, {"pwrate", T::PwLfoFreq}, {"pwsweep", T::PwLfoDepth},
        {"wt", T::WtPosition}, {"warp", T::WtWarp},
        {"wtrate", T::WtLfoFreq}, {"wtsync", T::WtLfoFreq}, {"wtdepth", T::WtLfoDepth}, {"wtskew", T::WtLfoSkew},
        {"warprate", T::WarpLfoFreq}, {"warpsync", T::WarpLfoFreq}, {"warpdepth", T::WarpLfoDepth}, {"warpskew", T::WarpLfoSkew},
        {"stretch", T::Stretch},
        {"lpdepth", T::LpfLfoDepth}, {"lpdepthfrequency", T::LpfLfoDepth}, {"lpdc", T::LpfLfoDc}, {"lpskew", T::LpfLfoSkew},
        {"hpdepth", T::HpfLfoDepth}, {"hpdepthfrequency", T::HpfLfoDepth}, {"hpdc", T::HpfLfoDc}, {"hpskew", T::HpfLfoSkew},
        {"bpdepth", T::BpfLfoDepth}, {"bpdepthfrequency", T::BpfLfoDepth}, {"bpdc", T::BpfLfoDc}, {"bpskew", T::BpfLfoSkew},
        {"vowel", T::VowelFreq}, {"coarse", T::Coarse}, {"crush", T::Crush}, {"shape", T::Shape}, {"shapevol", T::ShapeVol},
        {"distort", T::Distort}, {"distorttype", T::Distort}, {"distortvol", T::DistortVol},
        {"compressor", T::CompThreshold}, {"compressorRatio", T::CompRatio}, {"compressorKnee", T::CompKnee},
        {"compressorAttack", T::CompAttack}, {"compressorRelease", T::CompRelease},
        {"delaytime", T::DelayTime}, {"delaysync", T::DelayTime}, {"delayfeedback", T::DelayFeedback}, {"djf", T::Djf},
        {"detune", T::FreqSpread}, {"spread", T::PanSpread},
    };
    if (int op = fmOperatorKey(control, "fmi"); op > 0) return T(int(T::FmIndex) + op - 1);
    if (int op = fmOperatorKey(control, "fmh"); op > 0) return T(int(T::FmFreq) + op - 1);
    const std::string base = control.substr(0, control.find('_'));
    if (base == "bmod") return T::None; // connectBusModulator registers no node: nothing can target a bmod
    // getControlData: CONTROL_TARGETS[`${control}_${subControl}`] ?? CONTROL_TARGETS[control]
    if (base == "lfo") {
        const std::string& sc = subControl;
        if (sc == "depth" || sc == "depthabs") return T::ModDepth;
        if (sc == "dcoffset") return T::ModDcoffset;
        if (sc == "skew") return T::ModSkew;
        if (sc == "curve") return T::ModCurve;
        return T::ModFrequency; // rate, sync, and anything else
    }
    if (base == "env") {
        const std::string& sc = subControl;
        if (sc == "attack") return T::ModAttack;
        if (sc == "decay") return T::ModDecay;
        if (sc == "sustain") return T::ModSustain;
        if (sc == "release") return T::ModRelease;
        return T::ModDepth;
    }
    for (const auto& [n, t] : table)
        if (control == n) return t;
    return T::None;
}

// "[0.5,1]" or a number -> list
std::vector<double> numbers(const ParamValue& v)
{
    std::vector<double> out;
    if (v.type == ParamValue::Type::Number) {
        out.push_back(v.number);
        return out;
    }
    const char* p = v.string.c_str();
    while (*p) {
        if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.') {
            char* endp = nullptr;
            const double d = std::strtod(p, &endp);
            if (endp == p) {
                ++p;
                continue;
            }
            out.push_back(d);
            p = endp;
        } else ++p;
    }
    return out;
}

// vowel.mjs vowelFormant keys (and their aliases)
int vowelIndex(const std::string& v)
{
    static const char* names[] = {"a", "e", "i", "o", "u", "ae", "aa", "oe", "ue", "y", "uh", "un", "en", "an", "on"};
    for (int i = 0; i < 15; ++i)
        if (v == names[i]) return i;
    if (v == "\u00e6" || v == "æ") return 5;
    if (v == "ø" || v == "ö") return 7;
    if (v == "ɑ" || v == "å") return 6;
    if (v == "ü") return 8;
    if (v == "ı") return 9;
    return -2; // unknown: superdough throws
}

// helpers.mjs getModulationShapeInput
int8_t modulationShape(const ParamValue& v)
{
    if (v.type == ParamValue::Type::Number) {
        const double m = std::fmod(v.number, 5.0);
        return int8_t(m < 0 ? m + 5 : m);
    }
    static const std::pair<const char*, int> names[] = {{"tri", 0}, {"triangle", 0}, {"sine", 1}, {"ramp", 2}, {"saw", 3}, {"square", 4}};
    for (const auto& [n, i] : names)
        if (v.string == n) return int8_t(i);
    return 0;
}

// helpers.mjs distortionAlgorithms order
int8_t distortionIndex(const ParamValue& v)
{
    if (v.type == ParamValue::Type::Number) return int8_t(std::lround(v.number) % 9);
    static const char* names[] = {"scurve", "soft", "hard", "cubic", "diode", "asym", "fold", "sinefold", "chebyshev"};
    for (int i = 0; i < 9; ++i)
        if (v.string == names[i]) return int8_t(i);
    return 0; // superdough logs and defaults to scurve
}

// synth.mjs waveformN(partials, phases, type): coefficients, then createPeriodicWave (normalised)
uint32_t customWave(const std::string& type, const std::vector<double>& partials, const std::vector<double>& phases,
                    double sampleRate)
{
    const size_t len = partials.size();
    std::vector<double> real(len + 1, 0.0), imag(len + 1, 0.0);
    std::string key = type + ":" + std::to_string(sampleRate);
    for (size_t n = 0; n < len; ++n) {
        const double k = double(n + 1), mag = partials[n];
        double re = 0, im = 0;
        if (type == "sawtooth") im = -1 / k;
        else if (type == "square") im = (n + 1) % 2 == 0 ? 0 : 1 / k;
        else if (type == "triangle") re = (n + 1) % 2 == 0 ? 0 : 1 / (k * k);
        else im = 1; // user
        double R = re * mag, I = im * mag;
        const double ph = n < phases.size() ? phases[n] : 0.0;
        if (ph != 0) {
            const double c = std::cos(2 * 3.14159265358979323846 * ph), s = std::sin(2 * 3.14159265358979323846 * ph);
            const double R0 = R, I0 = I;
            R = c * R0 - s * I0;
            I = s * R0 + c * I0;
        }
        real[n + 1] = R;
        imag[n + 1] = I;
        key += "," + std::to_string(mag) + "@" + std::to_string(ph);
    }
    return sd::WaveStore::instance().getOrBuild(key, sampleRate, real, imag);
}

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// "fmwave" -> 1, "fmwave3" -> 3 (prefix + an operator 1..8), else 0
int fmOperatorKey(const std::string& key, const char* prefix)
{
    const size_t n = std::strlen(prefix);
    if (key.compare(0, n, prefix) != 0) return 0;
    if (key.size() == n) return 1;
    if (key.size() == n + 1 && key[n] >= '1' && key[n] <= '8') return key[n] - '0';
    return 0;
}

double numberOf(const ParamValue& v)
{
    if (v.type == ParamValue::Type::Number) return v.number;
    char* endp = nullptr;
    const double d = std::strtod(v.string.c_str(), &endp);
    return endp != v.string.c_str() ? d : std::numeric_limits<double>::quiet_NaN();
}

// applyFM's matrix controls beyond operator 1: fmiN (N -> N-1), fmiIJ (I -> J, unless I == J + 1,
// which applyFM reads from fmiI), fmhN, fmattackN, fmdecayN, fmsustainN, fmreleaseN.
bool fmMatrixKey(const std::string& key, const ParamValue& v, sd::SoundParams& out)
{
    const double x = numberOf(v);
    if (key.size() == 5 && key.compare(0, 3, "fmi") == 0 && std::isdigit((unsigned char)key[3]) && std::isdigit((unsigned char)key[4])) {
        const int i = key[3] - '0', j = key[4] - '0';
        if (i >= 1 && i <= 8 && j <= 8 && i != j + 1) out.fmMatrix[i][j] = std::isnan(x) ? 0.f : float(x);
        return true;
    }
    if (int op = fmOperatorKey(key, "fmi"); op > 1) {
        out.fmMatrix[op][op - 1] = std::isnan(x) ? 0.f : float(x);
        return true;
    }
    struct F { const char* prefix; float sd::FmOperator::* member; };
    static const F fields[] = {{"fmh", &sd::FmOperator::h}, {"fmattack", &sd::FmOperator::attack}, {"fmdecay", &sd::FmOperator::decay},
                               {"fmsustain", &sd::FmOperator::sustain}, {"fmrelease", &sd::FmOperator::release}};
    for (const auto& f : fields)
        if (int op = fmOperatorKey(key, f.prefix); op > 1) {
            out.fmOps[op].*(f.member) = float(x);
            return true;
        }
    return false;
}

uint32_t hashString(const std::string& s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return h;
}

// .channels(3) or .channels("3:4") -> [3,4]: first channel, 0-based
int firstChannel(const ParamValue& v)
{
    double c = v.number;
    if (v.type == ParamValue::Type::String) {
        const char* p = v.string.c_str();
        while (*p && !(*p >= '0' && *p <= '9')) ++p;
        c = std::atof(p);
    }
    return c >= 1 ? int(c) - 1 : -1;
}
} // namespace

void registerSuperDirtSynths(PatternEngine& engine)
{
    std::string list = "concha.registerSuperDirtSynths([";
    bool first = true;
    for (int i = 0; i < sc::scDefCount(); ++i)
        if (const sc::ScDef& d = sc::scDef(i); d.instrument) {
            list += (first ? "\"" : ",\"") + d.name + "\"";
            first = false;
        }
    std::string json;
    engine.evalJson(list + "])", json);
    // their controls Strudel doesn't have become pattern functions (a user's SynthDef's `harm`)
    std::string controls = "concha.registerSynthControls([";
    first = true;
    for (int i = 0; i < sc::scDefCount(); ++i)
        if (const sc::ScDef& d = sc::scDef(i); d.instrument)
            for (const auto& c : d.controls) {
                if (c.empty() || c == "out" || c.find('[') != std::string::npos) continue;
                controls += (first ? "\"" : ",\"") + c + "\"";
                first = false;
            }
    engine.evalJson(controls + "])", json);
}

namespace {
// a control of the SynthDef that plays the event (s("mybeep").harm(2)): the synth takes it
bool isSynthControl(const PatternEvent& e, const std::string& key)
{
    const ParamValue* type = e.find("__type");
    const ParamValue* s = e.find("__s");
    if (!type || type->string != "superdirt" || !s) return false;
    if (key == "amp" || key == "hold" || key == "curve") return true; // StrudelDirt's envelope module
    const int def = sc::scDefIndex(s->string);
    return def >= 0 && sc::scControlIndex(sc::scDef(def), key) >= 0;
}
} // namespace

bool SoundConverter::convertControls(const PatternEvent& e, sd::SoundParams& out, const Report& report)
{
    for (const auto& [key, v] : e.params) {
        bool known = false;
        for (const auto& f : kFields)
            if (key == f.name) {
                known = true;
                if (v.type == ParamValue::Type::Number) out.*(f.member) = float(v.number);
                else {
                    char* endp = nullptr;
                    const double d = std::strtod(v.string.c_str(), &endp); // "0.5" strings from mini-notation
                    if (endp != v.string.c_str()) out.*(f.member) = float(d);
                }
                break;
            }
        if (known || startsWith(key, "__")) continue;
        for (const char* h : kHandled)
            if (key == h) known = true;
        if (key == "ftype") {
            if (v.type == ParamValue::Type::Number) {
                const int i = int(std::floor(std::fmod(std::fmod(v.number, 3.0) + 3.0, 3.0)));
                out.ftype = sd::FilterModel(i);
            } else out.ftype = v.string == "24db" ? sd::FilterModel::Db24 : v.string == "ladder" ? sd::FilterModel::Ladder : sd::FilterModel::Db12;
        } else if (int op = fmOperatorKey(key, "fmwave"); op > 0) {
            const std::string w = v.string;
            out.fmOps[op].wave = w == "square" ? 1 : w == "sawtooth" || w == "saw" ? 2 : w == "triangle" || w == "tri" ? 3
                               : w == "white" ? 4 : w == "pink" ? 5 : w == "brown" ? 6 : w == "crackle" ? 7 : 0;
            if (op == 1) out.fmWave = out.fmOps[1].wave;
        } else if (int op2 = fmOperatorKey(key, "fmenv"); op2 > 0) {
            out.fmOps[op2].envLinear = v.string == "lin" || v.string == "linear";
            if (op2 == 1) out.fmEnvLinear = out.fmOps[1].envLinear;
        } else if (fmMatrixKey(key, v, out)) {
            // fmiN, fmhN, fmattackN... (N = 2..8) and fmiIJ
        } else if (key == "unit") {
            out.unitCycles = v.string == "c";
        } else if (key == "channels") {
            out.channel = int8_t(firstChannel(v));
        } else if (key == "vowel") {
            const int vi = vowelIndex(v.string);
            if (vi == -2) {
                report("vowel:" + v.string, "[superdough] vowel: unknown vowel " + v.string);
                return false;
            }
            out.vowel = int8_t(vi);
        } else if (key == "lfo" || key == "env" || key == "bmod") {
            // collected after the loop (the order matters: superdough connects every lfo, then every env)
        } else if (key == "warpmode") { // wavetable.mjs: Warpmode[name.toUpperCase()] ?? NONE
            static const char* modes[] = {"NONE", "ASYM", "MIRROR", "BENDP", "BENDM", "BENDMP", "SYNC", "QUANT", "FOLD",
                                          "PWM", "ORBIT", "SPIN", "CHAOS", "PRIMES", "BINARY", "BROWNIAN", "RECIPROCAL",
                                          "WORMHOLE", "LOGISTIC", "SIGMOID", "FRACTAL", "FLIP"};
            if (v.type == ParamValue::Type::Number) out.warpmode = int8_t(v.number);
            else {
                std::string up = v.string;
                for (auto& c : up) c = char(std::toupper((unsigned char)c));
                out.warpmode = 0;
                for (int k = 0; k < 22; ++k)
                    if (up == modes[k]) out.warpmode = int8_t(k);
            }
        } else if (key == "wtshape" || key == "warpshape") {
            (key == "wtshape" ? out.wtshape : out.warpshape) = modulationShape(v);
        } else if (key == "analyze") { // any id: numbers and the widgets' strings share the slots
            std::string id;
            if (v.type == ParamValue::Type::Number) { // as JS's String(n): "1", "0.5"
                if (v.number == std::floor(v.number)) id = std::to_string(int64_t(v.number));
                else {
                    char b[32];
                    std::snprintf(b, sizeof b, "%.15g", v.number);
                    id = b;
                }
                if (v.number == 0) id.clear(); // analyze(0): none, as superdough's falsy check
            } else
                id = v.string;
            out.analyze = id.empty() ? 0 : int8_t(sd::analyserSlot(id));
        } else if (key == "distorttype") {
            out.distorttype = distortionIndex(v);
        } else if (key == "tremoloshape" || key == "lpshape" || key == "hpshape" || key == "bpshape") {
            const int8_t sh = modulationShape(v);
            (key == "tremoloshape" ? out.tremoloshape : key == "lpshape" ? out.lpshape : key == "hpshape" ? out.hpshape : out.bpshape) = sh;
        } else if (key == "duckorbit" || key == "duckonset" || key == "duckattack" || key == "duckdepth") {
            const auto list = numbers(v);
            float* dst = key == "duckorbit" ? out.duckorbit : key == "duckonset" ? out.duckonset : key == "duckattack" ? out.duckattack : out.duckdepth;
            int8_t& count = key == "duckorbit" ? out.numDuck : key == "duckonset" ? out.numDuckOnset : key == "duckattack" ? out.numDuckAttack : out.numDuckDepth;
            count = int8_t(std::min<size_t>(4, list.size()));
            for (int k = 0; k < count; ++k) dst[k] = float(list[size_t(k)]);
        } else if (!known && !isSynthControl(e, key)) {
            report("param:" + key, "[superdough C++] control \"" + key + "\" is not implemented yet: ignored");
        }
    }

    // FM operator 1 and fmi (1 -> carrier) from their own fields
    out.fmOps[1].h = out.fmh;
    out.fmOps[1].attack = out.fmattack;
    out.fmOps[1].decay = out.fmdecay;
    out.fmOps[1].sustain = out.fmsustain;
    out.fmOps[1].release = out.fmrelease;
    out.fmMatrix[1][0] = std::isnan(out.fmi) ? 0.f : out.fmi;
    // K() and ir: resolved by sounds.mjs
    if (const ParamValue* k = e.find("__kabel"); k && k->type == ParamValue::Type::String) {
        std::string error;
        out.kabelId = sd::kabel::KabelStore::instance().getOrBuild(k->string, sampleRate, error);
        if (!out.kabelId) report("kabel:" + error, "[kabelsalat] " + error + ": K() ignored");
    }
    if (const ParamValue* irUrl = e.find("__irurl"); irUrl && irUrl->type == ParamValue::Type::String && library)
        out.irSampleId = library->requestSample(irUrl->string);
    return true;
}

void SoundConverter::parseModulators(const PatternEvent& e, int fxi, sd::SoundParams& out, std::vector<std::string>& modIds,
                                     const Report& report)
{
    for (const char* kind : {"lfo", "env", "bmod"}) {
        const ParamValue* m = e.find(kind);
        if (!m || m->type != ParamValue::Type::String) continue;
        for (const auto& entry : modulatorEntries(m->string)) {
            if (out.numModulators >= sd::SoundParams::kMaxModulators) {
                report("mods:max", "[superdough C++] more than 8 modulators on one sound: the others are ignored");
                break;
            }
            sd::Modulator md;
            md.fxi = int8_t(fxi); // params.fxi ??= key
            md.kind = kind[0] == 'l' ? 1 : kind[0] == 'e' ? 2 : 3;
            std::string control = kind, subControl;
            auto num = [](const std::string& v) { return float(std::strtod(v.c_str(), nullptr)); };
            for (const auto& [k, v] : entry.fields) {
                if (k == "control") control = v;
                else if (k == "subControl") subControl = v;
                else if (k == "rate") md.rate = num(v);
                else if (k == "sync") md.sync = num(v);
                else if (k == "depth") md.depth = num(v);
                else if (k == "depthabs") md.depthabs = num(v);
                else if (k == "dcoffset" || (k == "dc" && md.kind == 3)) md.dcoffset = num(v);
                else if (k == "bus") md.bus = num(v);
                else if (k == "skew") md.skew = num(v);
                else if (k == "curve") md.curve = num(v);
                else if (k == "retrig") md.retrig = num(v);
                else if (k == "shape") {
                    ParamValue pv;
                    if (!v.empty() && (std::isdigit((unsigned char)v[0]) || v[0] == '-')) pv.number = num(v);
                    else { pv.type = ParamValue::Type::String; pv.string = v; }
                    md.shape = modulationShape(pv);
                } else if (k == "attack") md.attack = num(v);
                else if (k == "decay") md.decay = num(v);
                else if (k == "sustain") md.sustain = num(v);
                else if (k == "release") md.release = num(v);
                else if (k == "acurve") md.acurve = num(v);
                else if (k == "dcurve") md.dcurve = num(v);
                else if (k == "rcurve") md.rcurve = num(v);
                else if (k == "fxi") md.fxi = v == "main" ? int8_t(-1) : int8_t(std::clamp(int(std::strtol(v.c_str(), nullptr, 10)), -1, 7));
            }
            md.target = modTarget(control, subControl);
            if (md.target >= sd::ModTarget::ModFrequency) { // another modulator, created before this one
                for (size_t j = 0; j < modIds.size(); ++j)
                    if (modIds[j] == control) md.targetIndex = int8_t(j);
                if (md.targetIndex < 0) md.target = sd::ModTarget::None;
            }
            if (md.target == sd::ModTarget::None) {
                report("modtarget:" + control + "_" + subControl,
                       "[superdough C++] modulating \"" + control + (subControl.empty() ? "" : "_" + subControl) +
                           "\" is not implemented yet: ignored");
                continue; // like superdough: not created, so other modulators cannot target it
            }
            out.modulators[out.numModulators++] = md;
            modIds.push_back(std::string(kind) + "_" + entry.key);
        }
    }
}

// Concha's live controls: "param:kind:a:b:c0:slope;..." (concha.mjs liveBindings) -> modulators of
// kind 4, which move the parameter of the playing voice with the MIDI CC or macro.
void SoundConverter::parseLive(const PatternEvent& e, sd::SoundParams& out, const Report& report)
{
    const ParamValue* live = e.find("__live");
    if (!live || live->type != ParamValue::Type::String) return;
    auto number = [&](const char* key, double fallback) {
        const ParamValue* v = e.find(key);
        return v && v->type == ParamValue::Type::Number ? v->number : fallback;
    };
    size_t at = 0;
    const std::string& text = live->string;
    while (at < text.size()) {
        size_t end = text.find(';', at);
        if (end == std::string::npos) end = text.size();
        const std::string entry = text.substr(at, end - at);
        at = end + 1;
        std::vector<std::string> f;
        for (size_t a = 0;;) {
            const size_t b = entry.find(':', a);
            f.push_back(entry.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (f.size() != 6) continue;
        const std::string& param = f[0];
        sd::ModTarget target = modTarget(param, "");
        double factor = 1; // the parameter's units -> the node's (superdough's own conversions)
        if (param == "pan") factor = 2;                        // panner.pan = 2 * pan - 1
        else if (param == "gain") factor = number("velocity", 1); // gain node = gain * velocity
        else if (param == "velocity") target = sd::ModTarget::Gain, factor = number("gain", 0.8);
        if (target == sd::ModTarget::None || target == sd::ModTarget::SourceFrequency || target >= sd::ModTarget::ModFrequency)
            continue; // pitch and other modulators: not live (a new note follows the control)
        if (out.numModulators >= sd::SoundParams::kMaxModulators) {
            report("live:max", "[live] more than 8 modulators and live controls on one sound: the others are ignored");
            break;
        }
        sd::Modulator md;
        md.kind = 4;
        md.target = target;
        md.fxi = -1;
        md.liveKind = int8_t(std::strtol(f[1].c_str(), nullptr, 10));
        md.liveA = int16_t(std::strtol(f[2].c_str(), nullptr, 10));
        md.liveB = int8_t(std::strtol(f[3].c_str(), nullptr, 10));
        md.liveC0 = float(std::strtod(f[4].c_str(), nullptr));
        md.liveSlope = float(std::strtod(f[5].c_str(), nullptr) * factor);
        if (!std::isfinite(md.liveSlope) || md.liveSlope == 0) continue;
        out.modulators[out.numModulators++] = md;
    }
}

bool SoundConverter::convert(const PatternEvent& e, sd::SoundParams& out, std::vector<std::string>& log)
{
    out = SoundParams{};
    const Report report = [&](const std::string& key, const std::string& message) {
        if (reported.insert(key).second) log.push_back(message);
    };
    if (!convertControls(e, out, report)) return false;
    std::vector<std::string> modIds; // "lfo_<id>" / "env_<id>" / "bmod_<id>" of each modulator slot
    // FX chain stages (each runs superdough's whole effect block before the value's own effects)
    if (const ParamValue* fx = e.find("FX"); fx && fx->type == ParamValue::Type::String) {
        sd::FxChain chain;
        for (auto& stageParams : objectArray(fx->string)) {
            if (chain.count >= sd::FxChain::kMaxStages) {
                report("fx:max", "[superdough C++] more than 4 FX stages: the others are ignored");
                break;
            }
            PatternEvent stage;
            stage.params = std::move(stageParams);
            sd::SoundParams& sp = chain.stages[size_t(chain.count)];
            sp = sd::SoundParams{};
            if (!convertControls(stage, sp, report)) return false;
            parseModulators(stage, chain.count, out, modIds, report); // they target this stage by default
            ++chain.count;
        }
        if (chain.count > 0) {
            out.fxChainId = sd::FxChainStore::instance().publish(chain);
            if (!out.fxChainId) report("fx:full", "[superdough C++] too many different FX chains at once: FX ignored");
        }
    }

    // StrudelDirt's effect modules superdough lacks (default-effects-extra.scd, core-modules.scd), in
    // StrudelDirt's module order: they run at the end of the voice, in the effects' scsynth World
    {
        struct Module {
            const char* def;
            std::initializer_list<const char*> tests;
            std::initializer_list<std::pair<const char*, const char*>> args; // control <- event key
        };
        static const Module modules[] = {
            {"dirt_pitch_shift2", {"psrate"}, {{"psrate", "psrate"}, {"psdisp", "psdisp"}, {"sustain", "#sustain"}}},
            {"dirt_grain_envelope2", {"tilt"}, {{"sustain", "#sustain"}, {"tilt", "tilt"}, {"plat", "plat"}, {"curve", "curve"}}},
            {"dirt_tremolo2", {"tremolorate"}, {{"tremolorate", "tremolorate"}, {"tremolodepth", "tremolodepth"}}},
            {"waveloss2", {"waveloss"}, {{"drop", "waveloss"}}},
            {"squiz2", {"squiz"}, {{"pitchratio", "squiz"}}},
            {"dirt_fshift2", {"fshift"}, {{"fshift", "fshift"}, {"fshiftphase", "fshiftphase"}, {"fshiftnote", "fshiftnote"}, {"freq", "#freq"}}},
            {"dirt_triode2", {"triode"}, {{"triode", "triode"}}},
            {"dirt_krush2", {"krush"}, {{"krush", "krush"}, {"kcutoff", "kcutoff"}}},
            {"dirt_octer2", {"octer", "octersub", "octersubsub"}, {{"octer", "octer"}, {"octersub", "octersub"}, {"octersubsub", "octersubsub"}}},
            {"dirt_ring2", {"ring"}, {{"ring", "ring"}, {"ringf", "ringf"}, {"ringdf", "ringdf"}}},
            {"dirt_distort2", {"distort2"}, {{"distort", "distort2"}}},
            {"spectral-delay2", {"tsdelay", "xsdelay"}, {{"xsdelay", "xsdelay"}, {"tsdelay", "tsdelay"}}},
            {"spectral-freeze2", {"freeze"}, {{"freeze", "freeze"}}},
            {"spectral-comb2", {"comb"}, {{"comb", "comb"}}},
            {"spectral-smear2", {"smear"}, {{"smear", "smear"}}},
            {"spectral-scram2", {"scram"}, {{"scram", "scram"}}},
            {"spectral-binshift2", {"binshift"}, {{"binshift", "binshift"}}},
            {"spectral-hbrick2", {"hbrick"}, {{"hbrick", "hbrick"}}},
            {"spectral-lbrick2", {"lbrick"}, {{"lbrick", "lbrick"}}},
            {"spectral-conformer2", {"real", "imag"}, {{"real", "real"}, {"imag", "imag"}}},
            {"spectral-enhance2", {"enhance"}, {{"enhance", "enhance"}}},
        };
        auto number = [&](const char* k, double& v) {
            const ParamValue* p = e.find(k);
            if (!p) return false;
            if (p->type == ParamValue::Type::Number) v = p->number;
            else v = std::strtod(p->string.c_str(), nullptr);
            return true;
        };
        // DirtEvent's ~freq: midinote.midicps, midinote = (note ? n ? 36) + octave * 12
        double freq = 0, x = 0;
        if (const ParamValue* f = e.find("__freq")) freq = f->number;
        else if (number("freq", x)) freq = x;
        else {
            double note = 36, octave = 0;
            if (!number("note", note) && number("n", x)) note = x;
            number("octave", octave);
            freq = 440.0 * std::pow(2.0, (note + octave * 12 - 69) / 12.0);
        }
        for (const auto& m : modules) {
            bool on = false;
            for (const char* t : m.tests) on = on || e.find(t) != nullptr;
            if (!on) continue;
            const int def = sc::scDefIndex(m.def, true);
            if (def < 0 || out.numFxModules >= std::size(out.fxModules)) continue;
            const sc::ScDef& d = sc::scDefs()[size_t(def)];
            sd::FxModule& fm = out.fxModules[out.numFxModules++];
            fm = sd::FxModule{};
            fm.def = int16_t(def);
            for (const auto& [control, key] : m.args) {
                const int c = sc::scControlIndex(d, control);
                if (c < 0 || fm.numArgs >= std::size(fm.args)) continue;
                double v = 0;
                if (std::string_view(key) == "#sustain") fm.sustainArg = int8_t(fm.numArgs);
                else if (std::string_view(key) == "#freq") v = freq;
                else if (!number(key, v)) v = 0; // sendSynth's [key: ~key] with ~key nil: OSC sends 0
                fm.args[fm.numArgs++] = sd::SynthArg{uint8_t(c), float(v)};
            }
        }
    }

    // the value's own modulators, after the stages' (FX.forEach in superdough: stages, then main)
    parseModulators(e, -1, out, modIds, report);
    parseLive(e, out, report);

    const ParamValue* type = e.find("__type");
    const ParamValue* name = e.find("__s");
    const std::string t = type ? type->string : "synth", s = name ? name->string : "triangle";
    out.soundHash = hashString(s);
    if (t == "synth") {
        out.type = sd::SoundType::Synth;
        const ParamValue* f = e.find("__freq");
        out.frequency = f ? float(f->number) : 65.406f;
        if (s == "triangle" || s == "tri") out.wave = sd::SynthWave::Triangle;
        else if (s == "square" || s == "sqr") out.wave = sd::SynthWave::Square;
        else if (s == "sawtooth" || s == "saw") out.wave = sd::SynthWave::Sawtooth;
        else if (s == "sine" || s == "sin") out.wave = sd::SynthWave::Sine;
        else if (s == "one") out.wave = sd::SynthWave::One;
        else if (s == "user") out.wave = sd::SynthWave::User;
        else if (s == "supersaw") out.wave = sd::SynthWave::Supersaw;
        else if (s == "pulse") out.wave = sd::SynthWave::Pulse;
        else if (s == "bus") out.wave = sd::SynthWave::Bus; // reads bus n (value.n ?? 0)
        else if (s == "sbd") {
            out.wave = sd::SynthWave::Sbd;
            // getFrequencyFromValue(value, 29): sbd's default note is 29
            if (!e.find("note") && !e.find("freq")) out.frequency = float(440.0 * std::pow(2.0, (29 - 69) / 12.0));
        } else {
            report("sound:" + s, "[superdough C++] sound \"" + s + "\" is not implemented yet (phase 4b)");
            return false;
        }
        // getOscillator: partials = value.partials ?? value.n -> waveformN (not for sine)
        const bool basic = out.wave == sd::SynthWave::Triangle || out.wave == sd::SynthWave::Square ||
                           out.wave == sd::SynthWave::Sawtooth || out.wave == sd::SynthWave::User;
        if (basic) {
            const ParamValue* partials = e.find("partials");
            if (!partials) partials = e.find("n");
            std::vector<double> list;
            if (partials) {
                if (partials->type == ParamValue::Type::Number) {
                    const double n = partials->number;
                    if (n > 0) list.assign(size_t(std::min(n, 1024.0)), 1.0); // new Float32Array(n).fill(1)
                } else list = numbers(*partials);
            }
            if (!list.empty()) {
                const ParamValue* ph = e.find("phases");
                const std::string type = out.wave == sd::SynthWave::Triangle ? "triangle"
                                         : out.wave == sd::SynthWave::Square ? "square"
                                         : out.wave == sd::SynthWave::Sawtooth ? "sawtooth" : "user";
                out.customWave = customWave(type, list, ph ? numbers(*ph) : std::vector<double>{}, sampleRate);
            } else if (out.wave == sd::SynthWave::User) {
                report("user:nopartials", "[superdough] Synth 'user' was selected, but partials not specified. Defaulting to triangle. Use pat.partials to setup custom waveform");
                out.wave = sd::SynthWave::Triangle;
            }
        }
        return true;
    }
    if (t == "noise") {
        out.type = sd::SoundType::Noise;
        out.noiseType = s == "pink" ? 1 : s == "brown" ? 2 : s == "crackle" ? 3 : 0;
        return true;
    }
    if (t == "sample") {
        out.type = sd::SoundType::Sample;
        const ParamValue* url = e.find("__url");
        const ParamValue* tr = e.find("__transpose");
        out.transpose = tr ? float(tr->number) : 0.f;
        if (!url || !library) return false;
        out.sampleId = library->requestSample(url->string);
        return out.sampleId != 0;
    }
    if (t == "soundfont") { // a General MIDI preset's zone (js/src/sounds.mjs, lib::Soundfont)
        out.type = sd::SoundType::Soundfont;
        const ParamValue* url = e.find("__url");
        const ParamValue* midi = e.find("__midi");
        out.sfMidi = midi ? float(midi->number) : 48.f;
        if (!url || !library) return false;
        out.sampleId = library->requestSample(url->string);
        return out.sampleId != 0;
    }
    if (t == "wavetable") {
        out.type = sd::SoundType::Wavetable;
        const ParamValue* url = e.find("__url");
        const ParamValue* fl = e.find("__frameLen");
        const ParamValue* f = e.find("__freq");
        out.frameLen = uint16_t(fl ? std::max(16.0, std::min(32768.0, fl->number)) : 2048);
        out.frequency = f ? float(f->number) : 65.406f;
        if (!url || !library) return false;
        out.sampleId = library->requestSample(url->string);
        return out.sampleId != 0;
    }
    if (t == "superdirt") { // a StrudelDirt instrument (DirtEvent: the event's keys that are controls)
        out.type = sd::SoundType::ScSynth;
        const int def = sc::scDefIndex(s);
        if (def < 0) return false;
        const sc::ScDef& d = sc::scDef(def);
        out.scDef = int16_t(def);
        auto control = [&](const char* n) { return int8_t(sc::scControlIndex(d, n)); };
        auto add = [&](int index, float value) {
            if (index < 0 || out.numScArgs >= std::size(out.scArgs)) return;
            for (int k = 0; k < out.numScArgs; ++k)
                if (out.scArgs[k].control == index) {
                    out.scArgs[k].value = value;
                    return;
                }
            out.scArgs[out.numScArgs++] = sd::SynthArg{uint8_t(index), value};
        };
        // the hap's numbers whose names are controls (velocity, detune, voice, lfo, rate...)
        static const char* const engine[] = {"out", "pan", "gain", "postgain", "orbit", "sustain", "delta", "cps", "cycle",
                                              "totalDuration", "speed", "endSpeed"};
        for (const auto& [key, v] : e.params) {
            if (key.empty() || key[0] == '_' || v.type != ParamValue::Type::Number) continue;
            bool skip = false;
            for (const char* x : engine)
                if (key == x) skip = true;
            if (!skip) add(sc::scControlIndex(d, key), float(v.number));
        }
        const ParamValue* f = e.find("__freq");
        const ParamValue* mn = e.find("__midinote");
        if (f) add(control("freq"), float(f->number));
        if (mn) add(control("midinote"), float(mn->number));
        if (const ParamValue* n = e.find("n"); n && n->type == ParamValue::Type::Number && !e.find("z1"))
            add(control("z1"), float(n->number)); // ~z1 = ~z1 ? ~n
        add(control("pan"), 0.f); // the voice pans (superdough), the synth stays centred
        // set when the voice starts (they depend on the tempo)
        out.scSustain = control("sustain");
        out.scDelta = control("delta");
        out.scCps = control("cps");
        out.scCycle = control("cycle");
        out.scTotal = control("totalDuration");
        out.scRelease = control("release");
        out.scSpeed = control("speed");
        out.scEndSpeed = control("endSpeed");
        auto num = [&](const char* k, float def) {
            const ParamValue* p = e.find(k);
            return p && p->type == ParamValue::Type::Number ? float(p->number) : def;
        };
        out.scSpeedValue = num("speed", 1.f);
        out.scAccelerate = num("accelerate", std::numeric_limits<float>::quiet_NaN());
        out.scSustainValue = num("sustain", std::numeric_limits<float>::quiet_NaN());
        out.scClip = num("clip", num("legato", 1.f));
        out.scReleaseValue = num("release", 0.01f);
        out.scAttack = num("attack", std::numeric_limits<float>::quiet_NaN());
        out.scDecay = num("decay", std::numeric_limits<float>::quiet_NaN());
        out.scHold = num("hold", std::numeric_limits<float>::quiet_NaN());
        out.scCurve = num("curve", -2.f);
        out.scAmp = num("amp", 1.f);
        return true;
    }
    if (t == "zzfx" || t == "bytebeat") { // rendered in JS (js/src/sounds.mjs), played as a buffer
        out.type = sd::SoundType::Buffer;
        out.bufferMode = t == "bytebeat" ? 1 : 0;
        const ParamValue* id = e.find("__buffer");
        out.sampleId = id ? uint32_t(id->number) : 0;
        return out.sampleId != 0;
    }
    report("type:" + t, "[superdough C++] " + t + " sounds are not implemented yet (" + s + ")");
    return false;
}

} // namespace concha
