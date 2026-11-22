#pragma once

// StrudelDirt's SynthDefs compiled into Concha (resources/strudeldirt/synthdefs, packed at build
// time): their bytes for scsynth, their controls, and which ones are instruments (playable with
// s("name")) rather than StrudelDirt's internal effect / routing synths.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace concha::sc {

struct ScDef {
    std::string name;
    std::vector<std::string> controls; // in order (the SynthDef's control names)
    std::vector<float> defaults;
    bool instrument = false;
    std::string_view bytes; // the .scsyndef file
    std::shared_ptr<const std::string> owned; // a user's file (bytes points into it)
    std::string source; // a user's file path
    std::string serverName; // scsynth's name when `name` is another (a file's name for its only SynthDef)
    std::vector<std::string> ugens; // a user's SynthDef: its UGens' class names (each once)
};

// StrudelDirt's (built in, immutable)
const std::vector<ScDef>& scDefs();

// Every SynthDef: StrudelDirt's, then the user's (appended at run time, never moved or removed: a
// SynthDef read again from a changed file is a new entry, and the latest of a name wins). Any thread.
int scDefCount();
const ScDef& scDef(int index);
// -1 when unknown. builtinOnly: StrudelDirt's (effect modules a user SynthDef must not replace).
int scDefIndex(std::string_view name, bool builtinOnly = false);
int scControlIndex(const ScDef& def, std::string_view control); // -1 when the def has none of that name

// The SynthDefs of a .scsyndef file (SCgf versions 1 and 2): names, controls (by index, an array
// control's further slots named "<name>[k]"), defaults. False with a message when unreadable.
bool parseSynthDefFile(std::string_view bytes, std::vector<ScDef>& out, std::string& error);
// The user's SynthDef folders: reads the .scsyndef files (recursively) that are new or changed
// since the last call and appends their SynthDefs as instruments. A file holding one SynthDef
// also plays under the file's name (AmbientTextures.scsyndef holding \task_efef68ae-...). Returns the messages (errors,
// "loaded n SynthDefs from ..."). Any thread (serialised).
std::vector<std::string> loadUserSynthDefFolders(const std::vector<std::string>& paths);

// Problems found when a World receives a user's SynthDef (UGens Concha does not have), reported
// once per SynthDef whatever the number of Worlds; taken (and cleared) by the console's writer.
void reportSynthDefProblem(int index, std::string message);
std::vector<std::string> takeSynthDefProblems();

} // namespace concha::sc
