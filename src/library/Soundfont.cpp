#include "library/Soundfont.h"

#include "library/Decoder.h"

#include <cctype>
#include <cstdlib>

namespace concha::lib {

std::vector<unsigned char> decodeBase64(std::string_view text)
{
    static int8_t table[256];
    static const bool init = [] {
        for (auto& t : table) t = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[uint8_t(a[i])] = int8_t(i);
        return true;
    }();
    (void)init;
    std::vector<unsigned char> out;
    out.reserve(text.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        const int v = table[uint8_t(c)];
        if (v < 0) continue; // padding, line breaks
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t((acc >> bits) & 0xff));
        }
    }
    return out;
}

// The zones array of `{zones: [{key: value, ...}, ...]}` (a JS object literal: unquoted keys,
// numbers, '...' or "..." strings). Unknown keys are skipped.
std::vector<SoundfontZone> parseSoundfont(std::string_view s, std::string& error)
{
    std::vector<SoundfontZone> zones;
    size_t i = s.find("zones");
    if (i == std::string_view::npos || (i = s.find('[', i)) == std::string_view::npos) {
        error = "not a soundfont preset (no zones)";
        return zones;
    }
    ++i;
    auto skipSpace = [&] {
        while (i < s.size() && std::isspace(uint8_t(s[i]))) ++i;
    };
    while (i < s.size()) {
        skipSpace();
        if (i >= s.size() || s[i] == ']') break;
        if (s[i] == ',') {
            ++i;
            continue;
        }
        if (s[i] != '{') {
            error = "unexpected character in the zones";
            return {};
        }
        ++i;
        SoundfontZone z;
        while (i < s.size()) {
            skipSpace();
            if (s[i] == ',') {
                ++i;
                continue;
            }
            if (s[i] == '}') {
                ++i;
                break;
            }
            // key
            size_t k0 = i;
            if (s[i] == '"' || s[i] == '\'') {
                const char q = s[i];
                k0 = ++i;
                while (i < s.size() && s[i] != q) ++i;
            } else
                while (i < s.size() && (std::isalnum(uint8_t(s[i])) || s[i] == '_' || s[i] == '$')) ++i;
            const std::string_view key = s.substr(k0, i - k0);
            if (i < s.size() && (s[i] == '"' || s[i] == '\'')) ++i;
            skipSpace();
            if (i >= s.size() || s[i] != ':') {
                error = "unexpected character in a zone";
                return {};
            }
            ++i;
            skipSpace();
            // value: string, number, or anything else up to the next , or }
            std::string str;
            double num = 0;
            bool isStr = false;
            if (s[i] == '"' || s[i] == '\'') {
                const char q = s[i++];
                const size_t v0 = i;
                while (i < s.size() && s[i] != q) i += s[i] == '\\' ? 2 : 1;
                str.assign(s.substr(v0, i - v0));
                ++i;
                isStr = true;
            } else {
                const size_t v0 = i;
                int depth = 0;
                while (i < s.size() && (depth > 0 || (s[i] != ',' && s[i] != '}'))) {
                    if (s[i] == '[' || s[i] == '{') ++depth;
                    if (s[i] == ']' || s[i] == '}') --depth;
                    ++i;
                }
                num = std::strtod(std::string(s.substr(v0, i - v0)).c_str(), nullptr);
            }
            if (key == "keyRangeLow") z.keyRangeLow = int(num);
            else if (key == "keyRangeHigh") z.keyRangeHigh = int(num);
            else if (key == "originalPitch") z.originalPitch = num;
            else if (key == "coarseTune") z.coarseTune = num;
            else if (key == "fineTune") z.fineTune = num;
            else if (key == "sampleRate") z.sampleRate = num;
            else if (key == "loopStart") z.loopStart = num;
            else if (key == "loopEnd") z.loopEnd = num;
            else if (key == "file" && isStr) z.file = std::move(str);
            else if (key == "sample" && isStr) z.sample = std::move(str);
        }
        zones.push_back(std::move(z));
    }
    if (zones.empty()) error = "the soundfont preset has no zone";
    return zones;
}

int findSoundfontZone(const std::vector<SoundfontZone>& zones, double pitch)
{
    for (size_t i = 0; i < zones.size(); ++i)
        if (zones[i].keyRangeLow <= pitch && zones[i].keyRangeHigh + 1 >= pitch) return int(i);
    return -1;
}

std::unique_ptr<sd::SampleData> decodeSoundfontZone(const SoundfontZone& z, std::string& error)
{
    std::unique_ptr<sd::SampleData> d;
    if (!z.file.empty()) {
        d = decodeAudio(decodeBase64(z.file), error);
    } else if (!z.sample.empty()) { // getBuffer's zone.sample: int16 / 65536, at the zone's rate
        const auto bytes = decodeBase64(z.sample);
        d = std::make_unique<sd::SampleData>();
        d->channels = 1;
        d->frames = int64_t(bytes.size() / 2);
        d->sampleRate = z.sampleRate > 0 ? z.sampleRate : 44100;
        d->planar.resize(size_t(d->frames));
        for (int64_t k = 0; k < d->frames; ++k) {
            const int16_t v = int16_t(bytes[size_t(2 * k)] | (bytes[size_t(2 * k + 1)] << 8));
            d->planar[size_t(k)] = float(v) / 65536.f;
        }
    } else
        error = "the zone has no audio";
    if (!d) return d;
    // fontloader: loop when loopStart > 1 && loopStart < loopEnd, in seconds at the zone's rate
    const double rate = z.sampleRate > 0 ? z.sampleRate : 44100;
    if (z.loopStart > 1 && z.loopStart < z.loopEnd) {
        d->loopStart = z.loopStart / rate;
        d->loopEnd = z.loopEnd / rate;
    }
    d->baseDetune = z.originalPitch - 100.0 * z.coarseTune - z.fineTune;
    return d;
}

} // namespace concha::lib
