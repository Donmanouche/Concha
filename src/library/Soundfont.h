#pragma once

// WebAudioFont presets, as Strudel's soundfonts package loads them (packages/soundfonts/fontloader.mjs):
// a JS file `var _tone_xxx = {zones: [{keyRangeLow, keyRangeHigh, originalPitch, coarseTune,
// fineTune, sampleRate, loopStart, loopEnd, file: '<base64 mp3>' | sample: '<base64 int16>'}, ...]}`.

#include "superdough/SampleStore.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace concha::lib {

struct SoundfontZone {
    int keyRangeLow = 0, keyRangeHigh = 127;
    double originalPitch = 6000, coarseTune = 0, fineTune = 0, sampleRate = 44100;
    double loopStart = 0, loopEnd = 0; // frames at sampleRate
    std::string file;                  // base64 of a compressed file (mp3...)
    std::string sample;                // base64 of int16 little-endian PCM
};

// The zones of a preset file; empty with an error when it cannot be read.
std::vector<SoundfontZone> parseSoundfont(std::string_view text, std::string& error);
// findZone: the first zone with keyRangeLow <= pitch <= keyRangeHigh + 1 (fontloader.mjs); -1 if none.
int findSoundfontZone(const std::vector<SoundfontZone>& zones, double pitch);
// The zone's audio, with its loop (seconds) and base detune (cents) for the voice.
std::unique_ptr<sd::SampleData> decodeSoundfontZone(const SoundfontZone& zone, std::string& error);
std::vector<unsigned char> decodeBase64(std::string_view text);

} // namespace concha::lib
