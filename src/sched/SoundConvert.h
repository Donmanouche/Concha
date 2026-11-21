#pragma once

// PatternEvent (a "sound" hap value, resolved by js/src/sounds.mjs) -> superdough SoundParams.

#include "pattern/PatternEngine.h"
#include "superdough/SoundParams.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace concha {

namespace lib {
class SampleLibrary;
}

// Declares StrudelDirt's instruments (src/sc/ScDefs) to an engine's sound registry.
void registerSuperDirtSynths(PatternEngine& engine);

struct SoundConverter {
    lib::SampleLibrary* library = nullptr; // sample ids; null: samples are not played
    double sampleRate = 48000.0;           // for custom waves (partials)
    std::set<std::string> reported;        // unsupported controls / sounds already logged

    // false when the event cannot play (skipped). `log` gets new warnings.
    bool convert(const PatternEvent& e, sd::SoundParams& out, std::vector<std::string>& log);

    using Report = std::function<void(const std::string& key, const std::string& message)>;
    // The controls of a hap value (or of one FX stage) into `out`; false: the sound cannot play.
    bool convertControls(const PatternEvent& e, sd::SoundParams& out, const Report& report);
    // Concha's live controls (__live): parameters that follow a MIDI CC or a macro while they play.
    void parseLive(const PatternEvent& e, sd::SoundParams& out, const Report& report);
    // lfo / env / bmod of a value (fxi = its FX stage, -1 = main), appended to out.modulators.
    void parseModulators(const PatternEvent& e, int fxi, sd::SoundParams& out, std::vector<std::string>& modIds,
                         const Report& report);
};

} // namespace concha
