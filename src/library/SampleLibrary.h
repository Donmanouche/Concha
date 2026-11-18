#pragma once

// Concha's sample library, one per process (shared by every plug-in instance):
//  - named sample folders chosen by the user (none by default), saved in
//    ~/.config/Concha/library.txt; samples('Name') / samples('lib:Name') load one, and folders
//    marked "load at start" are registered before every evaluation;
//  - text loading for samples(url) / strudel.json: local paths, file:// and http(s), the latter
//    through a disk cache (~/.cache/Concha/http), so github: sample banks are downloaded once;
//  - sample loading: requestSample(url) returns an id at once, a loader thread decodes the file
//    and publishes it to the SampleStore the audio thread reads.
//
// Folder layout (scanFolder):
//  - a strudel.json at the root is used as is (paths relative to the folder);
//  - each sub-folder with audio files is a sound, named after the folder (Dirt-Samples layout);
//  - a sub-folder that only holds sub-folders is a bank: Bank/bank-bd/*.wav -> sound "Bank_bd"
//    (the tidal-drum-machines layout, so .bank("Bank") works);
//  - audio files at the root are one sound named after the folder's library name;
//  - every sound is also registered as <name>_<sound>, so .bank("<name>") works.

#include "superdough/SampleStore.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace concha::lib {

struct LibraryFolder {
    std::string name; // the name used in code
    std::string path; // absolute
    bool autoload = true;
};

class SampleLibrary {
public:
    static SampleLibrary& instance();

    sd::SampleStore& store() { return store_; }

    // ---- named folders ----
    std::vector<LibraryFolder> folders() const;
    void setFolders(std::vector<LibraryFolder> folders); // saves the settings
    uint64_t version() const { return version_.load(); }
    // Sample map JSON for a folder (by name, case-insensitive); empty if unknown.
    std::string folderMapJson(const std::string& name, std::string* error = nullptr);
    // {"version": n, "names": [...]} of the folders loaded at start.
    std::string autoloadJson() const;
    static std::string scanFolder(const std::string& path, const std::string& name, std::string* error);

    // ---- SynthDef folders (.scsyndef files, played like StrudelDirt's instruments; none by
    // default), saved in ~/.config/Concha/synthdefs.txt ----
    std::vector<std::string> synthDefFolders() const;
    void setSynthDefFolders(std::vector<std::string> paths); // saves the settings

    // ---- loading ----
    // Text of a URL or a path (blocking; http(s) cached on disk).
    bool fetchText(const std::string& url, std::string& text, std::string& error);
    // Id of a sample (loaded in the background when new). 0 for an empty URL.
    uint32_t requestSample(const std::string& url);
    bool isLoaded(uint32_t id) const { return store_.get(id) != nullptr; }
    // A buffer made at run time (zzfx, bytebeat notes): kept in a ring of transient slots, each
    // reused 60 s after it was published at the earliest. Returns its id (0 when all are recent).
    uint32_t publishTransient(const float* data, size_t frames, double sampleRate);
    static constexpr uint32_t kTransientBase = 60000, kTransientSlots = 4096;
    // Errors met by the loader since the last call (for the console).
    std::vector<std::string> takeErrors();

    // For tests: where settings and the cache live (default: XDG folders).
    void setDirectories(std::string configDir, std::string cacheDir);

    ~SampleLibrary();

private:
    SampleLibrary();
    void load();
    void save() const;
    void loaderLoop();
    bool resolveToBytes(const std::string& url, std::vector<unsigned char>& bytes, std::string& error);
    bool httpGet(const std::string& url, std::vector<unsigned char>& bytes, std::string& error);

    sd::SampleStore store_;
    mutable std::mutex mutex_;
    std::vector<LibraryFolder> folders_;
    std::vector<std::string> synthDefFolders_;
    std::map<std::string, std::string> mapCache_; // folder name -> scanned map
    std::atomic<uint64_t> version_{1};
    std::string configDir_, cacheDir_;

    std::map<std::string, uint32_t> ids_;
    uint32_t nextId_ = 1;
    std::deque<std::pair<uint32_t, std::string>> queue_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::vector<std::thread> loaders_;
    std::vector<std::string> errors_;
    struct Transient {
        std::unique_ptr<sd::SampleData> data;
        std::chrono::steady_clock::time_point published{};
    };
    std::vector<Transient> transients_;
    uint32_t nextTransient_ = 0;
};

} // namespace concha::lib
