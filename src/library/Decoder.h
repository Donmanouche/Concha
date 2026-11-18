#pragma once

// Audio file decoding for the sampler: WAV / AIFF (dr_wav), FLAC (dr_flac), MP3 (dr_mp3),
// Ogg Vorbis (stb_vorbis). Detected from the file's first bytes, not its name.

#include "superdough/SampleStore.h"

#include <memory>
#include <string>
#include <vector>

namespace concha::lib {

std::unique_ptr<sd::SampleData> decodeAudio(const std::vector<unsigned char>& bytes, std::string& error);
bool readFile(const std::string& path, std::vector<unsigned char>& out);

} // namespace concha::lib
