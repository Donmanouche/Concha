#include "library/Decoder.h"

#include <cstdio>
#include <cstring>

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace concha::lib {

namespace {
std::unique_ptr<sd::SampleData> fromInterleaved(const float* data, uint64_t frames, unsigned channels, double rate)
{
    auto out = std::make_unique<sd::SampleData>();
    out->channels = int(channels);
    out->frames = int64_t(frames);
    out->sampleRate = rate;
    out->planar.resize(size_t(frames) * channels);
    for (uint64_t f = 0; f < frames; ++f)
        for (unsigned c = 0; c < channels; ++c) out->planar[size_t(c * frames + f)] = data[f * channels + c];
    return out;
}
} // namespace

bool readFile(const std::string& path, std::vector<unsigned char>& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? size_t(n) : 0);
    const size_t got = n > 0 ? std::fread(out.data(), 1, size_t(n), f) : 0;
    std::fclose(f);
    return got == out.size();
}

std::unique_ptr<sd::SampleData> decodeAudio(const std::vector<unsigned char>& b, std::string& error)
{
    if (b.size() < 12) {
        error = "file too short";
        return nullptr;
    }
    const auto starts = [&](const char* m, size_t at = 0) { return std::memcmp(b.data() + at, m, std::strlen(m)) == 0; };
    if (starts("RIFF") || starts("RIFX") || starts("RF64") || starts("FORM") || starts("riff")) {
        unsigned ch = 0, rate = 0;
        drwav_uint64 frames = 0;
        float* d = drwav_open_memory_and_read_pcm_frames_f32(b.data(), b.size(), &ch, &rate, &frames, nullptr);
        if (!d) { error = "unreadable WAV/AIFF"; return nullptr; }
        auto out = fromInterleaved(d, frames, ch, rate);
        drwav_free(d, nullptr);
        return out;
    }
    if (starts("fLaC")) {
        unsigned ch = 0, rate = 0;
        drflac_uint64 frames = 0;
        float* d = drflac_open_memory_and_read_pcm_frames_f32(b.data(), b.size(), &ch, &rate, &frames, nullptr);
        if (!d) { error = "unreadable FLAC"; return nullptr; }
        auto out = fromInterleaved(d, frames, ch, rate);
        drflac_free(d, nullptr);
        return out;
    }
    if (starts("OggS")) {
        int ch = 0, rate = 0;
        short* d = nullptr;
        const int frames = stb_vorbis_decode_memory(b.data(), int(b.size()), &ch, &rate, &d);
        if (frames <= 0 || !d) { error = "unreadable Ogg Vorbis"; return nullptr; }
        std::vector<float> f(size_t(frames) * size_t(ch));
        for (size_t i = 0; i < f.size(); ++i) f[i] = d[i] / 32768.f;
        std::free(d);
        return fromInterleaved(f.data(), uint64_t(frames), unsigned(ch), rate);
    }
    // MP3: ID3 tag or frame sync
    if (starts("ID3") || (b[0] == 0xff && (b[1] & 0xe0) == 0xe0)) {
        drmp3_config cfg{};
        drmp3_uint64 frames = 0;
        float* d = drmp3_open_memory_and_read_pcm_frames_f32(b.data(), b.size(), &cfg, &frames, nullptr);
        if (!d) { error = "unreadable MP3"; return nullptr; }
        auto out = fromInterleaved(d, frames, cfg.channels, cfg.sampleRate);
        drmp3_free(d, nullptr);
        return out;
    }
    error = "unknown audio format";
    return nullptr;
}

} // namespace concha::lib

#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
