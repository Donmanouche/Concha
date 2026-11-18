#include "sc/ScDefs.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <algorithm>
#include <utility>

namespace concha::sc {

const std::vector<std::pair<std::string_view, std::string_view>>& embeddedSynthDefs(); // generated
std::string_view embeddedSynthDefIndex();

namespace {
// index.json: [{"name": "...", "controls": {"out": 0.0, ...}}, ...] (tools/sc/compile_synthdefs.scd)
std::vector<ScDef> parseIndex(std::string_view j)
{
    std::vector<ScDef> out;
    size_t i = 0;
    auto str = [&]() {
        std::string s;
        i = j.find('"', i);
        if (i == std::string_view::npos) return s;
        size_t e = j.find('"', i + 1);
        s = std::string(j.substr(i + 1, e - i - 1));
        i = e + 1;
        return s;
    };
    while ((i = j.find("\"name\"", i)) != std::string_view::npos) {
        i += 6;
        ScDef d;
        d.name = str();
        size_t c = j.find('{', j.find("\"controls\"", i));
        size_t end = j.find('}', c);
        i = c + 1;
        while (true) {
            size_t q = j.find('"', i);
            if (q == std::string_view::npos || q > end) break;
            i = q;
            d.controls.push_back(str());
            size_t colon = j.find(':', i);
            d.defaults.push_back(float(std::strtod(std::string(j.substr(colon + 1, 32)).c_str(), nullptr)));
            i = colon + 1;
        }
        i = end + 1;
        out.push_back(std::move(d));
    }
    return out;
}

// StrudelDirt's own plumbing and effect modules (named <module><numChannels>), and the synths
// superdough already has under the same name (superdough plays those)
bool isInstrument(const std::string& n)
{
    static const char* internal[] = {"debug", "in", "in1", "inr", "gain2", "squiz2", "waveloss2", "dj-filter2"};
    for (const char* x : internal)
        if (n == x) return false;
    if (n.rfind("concha_", 0) == 0 || n.rfind("dirt_", 0) == 0 || n.rfind("strudel_", 0) == 0 || n.rfind("spectral-", 0) == 0) return false;
    static const char* superdough[] = {"sine", "triangle", "sawtooth", "square", "pulse", "white", "pink", "brown"};
    for (const char* x : superdough)
        if (n == x) return false;
    return true;
}
} // namespace

const std::vector<ScDef>& scDefs()
{
    static const std::vector<ScDef> defs = [] {
        auto list = parseIndex(embeddedSynthDefIndex());
        const auto& bytes = embeddedSynthDefs();
        for (auto& d : list) {
            d.instrument = isInstrument(d.name);
            for (const auto& [name, data] : bytes)
                if (name == d.name) d.bytes = data;
        }
        return list;
    }();
    return defs;
}

namespace {
// the user's SynthDefs: fixed slots, filled before the count that publishes them
constexpr int kMaxUserDefs = 4096;
struct UserDefs {
    std::array<ScDef, kMaxUserDefs> defs;
    std::atomic<int> count{0};
    std::mutex mutex; // writers
    std::map<std::string, std::pair<int64_t, uintmax_t>> files; // path -> (mtime, size) read
};
UserDefs& userDefs()
{
    static UserDefs u;
    return u;
}
} // namespace

int scDefCount()
{
    return int(scDefs().size()) + userDefs().count.load(std::memory_order_acquire);
}

const ScDef& scDef(int index)
{
    const auto& b = scDefs();
    if (index < int(b.size())) return b[size_t(index)];
    return userDefs().defs[size_t(index - int(b.size()))];
}

int scDefIndex(std::string_view name, bool builtinOnly)
{
    const auto& defs = scDefs();
    if (!builtinOnly) {
        auto& u = userDefs();
        for (int i = u.count.load(std::memory_order_acquire) - 1; i >= 0; --i)
            if (u.defs[size_t(i)].name == name) return int(defs.size()) + i;
    }
    for (size_t i = 0; i < defs.size(); ++i)
        if (defs[i].name == name) return int(i);
    return -1;
}

namespace {
struct Reader {
    std::string_view b;
    size_t i = 0;
    bool bad = false;
    bool need(size_t n)
    {
        if (i + n > b.size()) bad = true;
        return !bad;
    }
    uint32_t u(int bytes)
    {
        if (!need(size_t(bytes))) return 0;
        uint32_t v = 0;
        for (int k = 0; k < bytes; ++k) v = (v << 8) | uint8_t(b[i++]);
        return v;
    }
    int32_t i32() { return int32_t(u(4)); }
    float f32()
    {
        const uint32_t v = u(4);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    std::string pstr()
    {
        const size_t n = u(1);
        if (!need(n)) return {};
        std::string s(b.substr(i, n));
        i += n;
        return s;
    }
    void skip(size_t n)
    {
        if (need(n)) i += n;
    }
};
} // namespace

bool parseSynthDefFile(std::string_view bytes, std::vector<ScDef>& out, std::string& error)
{
    Reader r{bytes};
    if (bytes.substr(0, 4) != "SCgf") {
        error = "not a SynthDef file (no SCgf header)";
        return false;
    }
    r.skip(4);
    const int version = r.i32();
    if (version != 1 && version != 2) {
        error = "unknown SynthDef file version " + std::to_string(version);
        return false;
    }
    const int w = version == 2 ? 4 : 2; // counts and input indices
    const int numDefs = int(r.u(2));
    for (int d = 0; d < numDefs && !r.bad; ++d) {
        ScDef def;
        def.name = r.pstr();
        const uint32_t numConstants = r.u(w);
        r.skip(size_t(numConstants) * 4);
        const uint32_t numParams = r.u(w);
        if (numParams > 100000) r.bad = true;
        if (r.bad) break;
        def.defaults.resize(numParams);
        for (auto& v : def.defaults) v = r.f32();
        def.controls.assign(numParams, std::string());
        const uint32_t numNames = r.u(w);
        std::vector<std::pair<uint32_t, std::string>> names;
        for (uint32_t k = 0; k < numNames && !r.bad; ++k) {
            std::string n = r.pstr();
            const uint32_t at = r.u(w);
            names.push_back({at, std::move(n)});
        }
        for (const auto& [at, n] : names)
            if (at < numParams) {
                def.controls[at] = n;
                // an array control: the slots up to the next name
                uint32_t next = numParams;
                for (const auto& [a2, n2] : names)
                    if (a2 > at && a2 < next) next = a2;
                for (uint32_t k = at + 1; k < next; ++k) def.controls[k] = n + "[" + std::to_string(k - at) + "]";
            }
        const uint32_t numUGens = r.u(w);
        for (uint32_t k = 0; k < numUGens && !r.bad; ++k) {
            r.pstr();       // class
            r.skip(1);      // rate
            const uint32_t ins = r.u(w), outs = r.u(w);
            r.skip(2);      // special index
            r.skip(size_t(ins) * size_t(2 * w));
            r.skip(outs);   // output rates
        }
        const uint32_t numVariants = r.u(2);
        for (uint32_t k = 0; k < numVariants && !r.bad; ++k) {
            r.pstr();
            r.skip(size_t(numParams) * 4);
        }
        if (!r.bad) out.push_back(std::move(def));
    }
    if (r.bad) {
        error = "truncated or damaged SynthDef file";
        return false;
    }
    return true;
}

std::vector<std::string> loadUserSynthDefFolders(const std::vector<std::string>& paths)
{
    namespace fs = std::filesystem;
    std::vector<std::string> messages;
    auto& u = userDefs();
    std::lock_guard<std::mutex> lock(u.mutex);
    for (const auto& root : paths) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            messages.push_back("SynthDef folder not found: " + root);
            continue;
        }
        std::vector<fs::path> files;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec) && it->path().extension() == ".scsyndef") files.push_back(it->path());
        std::sort(files.begin(), files.end());
        int added = 0;
        for (const auto& f : files) {
            const auto size = fs::file_size(f, ec);
            const auto mtime = int64_t(fs::last_write_time(f, ec).time_since_epoch().count());
            auto& seen = u.files[f.string()];
            if (seen == std::make_pair(mtime, size)) continue;
            seen = {mtime, size};
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            auto data = std::make_shared<const std::string>(ss.str());
            std::vector<ScDef> defs;
            std::string error;
            if (!parseSynthDefFile(*data, defs, error)) {
                messages.push_back(f.string() + ": " + error);
                continue;
            }
            // a file of one SynthDef is sent as is; several get one /d_recv for the file
            for (auto& d : defs) {
                const int n = u.count.load(std::memory_order_relaxed);
                if (n >= kMaxUserDefs) {
                    messages.push_back("too many SynthDefs (" + std::to_string(kMaxUserDefs) + "): " + d.name + " skipped");
                    break;
                }
                d.instrument = true;
                d.owned = data;
                d.bytes = *data;
                d.source = f.string();
                u.defs[size_t(n)] = std::move(d);
                u.count.store(n + 1, std::memory_order_release);
                ++added;
            }
        }
        if (added > 0) messages.push_back("loaded " + std::to_string(added) + " SynthDef" + (added > 1 ? "s" : "") + " from " + root);
    }
    return messages;
}

int scControlIndex(const ScDef& def, std::string_view control)
{
    for (size_t i = 0; i < def.controls.size(); ++i)
        if (def.controls[i] == control) return int(i);
    return -1;
}

} // namespace concha::sc
