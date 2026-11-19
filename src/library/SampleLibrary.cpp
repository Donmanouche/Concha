#include "library/SampleLibrary.h"

#include "library/Decoder.h"
#include "library/Soundfont.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace concha::lib {

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool isAudioFile(const fs::path& p)
{
    static const char* exts[] = {".wav", ".wave", ".aif", ".aiff", ".flac", ".mp3", ".ogg"};
    const std::string e = lower(p.extension().string());
    for (const char* x : exts)
        if (e == x) return true;
    return false;
}

std::string jsonString(const std::string& s)
{
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else o += char(c);
        }
    }
    return o + "\"";
}

// URL-encode a path for a file:// URL (superdough concatenates _base + file names).
std::string urlPath(const std::string& p)
{
    std::string o;
    for (unsigned char c : p) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
        else {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", c);
            o += b;
        }
    }
    return o;
}

std::string urlDecode(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            o += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else o += s[i];
    }
    return o;
}

// Strudel sound name of a folder: lower case, spaces as underscores.
// a sound's name from a folder or file name: spaces become _, the case is kept (lookups ignore it)
std::string soundName(const std::string& folder)
{
    std::string s = folder;
    for (auto& c : s)
        if (std::isspace((unsigned char)c)) c = '_';
    return s;
}

std::vector<fs::path> sortedAudio(const fs::path& dir)
{
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && isAudioFile(e.path())) files.push_back(e.path());
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        return lower(a.filename().string()) < lower(b.filename().string());
    });
    return files;
}

std::vector<fs::path> sortedDirs(const fs::path& dir)
{
    std::vector<fs::path> dirs;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_directory(ec) && e.path().filename().string()[0] != '.') dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

std::string homeDir()
{
    const char* h = std::getenv("HOME");
    return h ? h : ".";
}

size_t curlWrite(char* data, size_t size, size_t n, void* user)
{
    auto* out = static_cast<std::vector<unsigned char>*>(user);
    out->insert(out->end(), data, data + size * n);
    return size * n;
}

uint64_t fnv64(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

} // namespace

SampleLibrary& SampleLibrary::instance()
{
    static SampleLibrary lib;
    return lib;
}

SampleLibrary::SampleLibrary()
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const char* xc = std::getenv("XDG_CONFIG_HOME");
    const char* xk = std::getenv("XDG_CACHE_HOME");
    configDir_ = (xc && *xc ? std::string(xc) : homeDir() + "/.config") + "/Concha";
    cacheDir_ = (xk && *xk ? std::string(xk) : homeDir() + "/.cache") + "/Concha";
    load();
    for (int i = 0; i < 2; ++i) loaders_.emplace_back([this] { loaderLoop(); });
}

SampleLibrary::~SampleLibrary()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : loaders_)
        if (t.joinable()) t.join();
}

void SampleLibrary::setDirectories(std::string configDir, std::string cacheDir)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        configDir_ = std::move(configDir);
        cacheDir_ = std::move(cacheDir);
        folders_.clear();
        synthDefFolders_.clear();
        mapCache_.clear();
    }
    load();
    ++version_;
}

// library.txt: one folder per line, "autoload<TAB>name<TAB>path" (autoload: 1 or 0).
void SampleLibrary::load()
{
    std::ifstream f(configDir_ + "/library.txt");
    std::vector<LibraryFolder> folders;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        folders.push_back({line.substr(a + 1, b - a - 1), line.substr(b + 1), line.substr(0, a) == "1"});
    }
    std::vector<std::string> defFolders;
    std::ifstream d(configDir_ + "/synthdefs.txt");
    while (std::getline(d, line))
        if (!line.empty() && line[0] != '#') defFolders.push_back(line);
    std::lock_guard<std::mutex> lock(mutex_);
    folders_ = std::move(folders);
    synthDefFolders_ = std::move(defFolders);
}

std::vector<std::string> SampleLibrary::synthDefFolders() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return synthDefFolders_;
}

void SampleLibrary::setSynthDefFolders(std::vector<std::string> paths)
{
    std::lock_guard<std::mutex> lock(mutex_);
    synthDefFolders_ = std::move(paths);
    std::error_code ec;
    fs::create_directories(configDir_, ec);
    std::ofstream f(configDir_ + "/synthdefs.txt");
    f << "# Concha SynthDef folders (.scsyndef files): one path per line\n";
    for (const auto& p : synthDefFolders_) f << p << '\n';
}

void SampleLibrary::save() const
{
    std::error_code ec;
    fs::create_directories(configDir_, ec);
    std::ofstream f(configDir_ + "/library.txt");
    f << "# Concha sample folders: autoload<TAB>name<TAB>path\n";
    for (const auto& x : folders_) f << (x.autoload ? "1" : "0") << '\t' << x.name << '\t' << x.path << '\n';
}

std::vector<LibraryFolder> SampleLibrary::folders() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return folders_;
}

void SampleLibrary::setFolders(std::vector<LibraryFolder> folders)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        folders_ = std::move(folders);
        mapCache_.clear();
        save();
    }
    ++version_;
}

std::string SampleLibrary::scanFolder(const std::string& path, const std::string& name, std::string* error)
{
    const fs::path root(path);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        if (error) *error = "not a folder: " + path;
        return {};
    }
    const std::string base = "file://" + urlPath(fs::absolute(root).string()) + "/";
    // strudel.json at the root: as is, with the folder as base unless it sets its own.
    if (fs::exists(root / "strudel.json")) {
        std::vector<unsigned char> b;
        if (readFile((root / "strudel.json").string(), b)) {
            std::string json(b.begin(), b.end());
            if (json.find("\"_base\"") == std::string::npos) {
                const auto brace = json.find('{');
                if (brace != std::string::npos) json.insert(brace + 1, "\"_base\":" + jsonString(base) + ",");
            }
            return json;
        }
    }
    std::vector<std::pair<std::string, std::vector<std::string>>> sounds;
    std::set<std::string> used; // lower-case names already given
    auto rel = [&](const fs::path& p) { return urlPath(fs::relative(p, root).generic_string()); };
    auto addSound = [&](const std::string& n, const std::vector<fs::path>& files) {
        if (n.empty() || !used.insert(lower(n)).second || sounds.size() >= 50000) return false;
        std::vector<std::string> list;
        for (const auto& f : files) list.push_back(rel(f));
        sounds.emplace_back(n, std::move(list));
        return true;
    };
    const std::string libName = soundName(name);
    // the files at the root: the folder's name (n picks the file), and each file by its own name
    if (auto files = sortedAudio(root); !files.empty()) {
        addSound(libName, files);
        for (const auto& f : files) addSound(libName + "_" + soundName(f.stem().string()), {f});
    }
    // a bank (tidal-drum-machines): Bank/Bank_bd, Bank/bank-bd... -> Bank_bd, .bank("Bank")
    auto bankEntries = [&](const fs::path& dir) {
        std::vector<std::pair<std::string, fs::path>> entries;
        const std::string bank = lower(dir.filename().string());
        for (const auto& sub : sortedDirs(dir)) {
            const std::string n = lower(sub.filename().string());
            for (const std::string& prefix : {bank + "_", bank + "-", bank + " "})
                if (n.rfind(prefix, 0) == 0 && n.size() > prefix.size() && !sortedAudio(sub).empty())
                    entries.emplace_back(sub.filename().string().substr(prefix.size()), sub);
        }
        return entries;
    };
    // every folder below, at any depth: a sound named after it (Dirt-Samples), also <name>_<folder>
    std::function<void(const fs::path&, int)> walk = [&](const fs::path& dir, int depth) {
        if (depth > 12) return;
        for (const auto& sub : sortedDirs(dir)) {
            const std::string subName = soundName(sub.filename().string());
            const auto bank = bankEntries(sub);
            if (!bank.empty()) {
                for (const auto& [sound, folder] : bank) addSound(subName + "_" + soundName(sound), sortedAudio(folder));
                for (const auto& other : sortedDirs(sub)) { // what is not a bank entry is walked as usual
                    bool entry = false;
                    for (const auto& e : bank) entry = entry || e.second == other;
                    if (!entry) walk(other.parent_path() / other.filename(), depth + 1);
                }
                continue;
            }
            if (auto files = sortedAudio(sub); !files.empty()) {
                // the same folder name twice: the second one takes its parent's name too
                std::string n = subName;
                if (used.count(lower(n))) n = soundName(dir.filename().string()) + "_" + subName;
                addSound(n, files);
                addSound(libName + "_" + n, files);
            }
            walk(sub, depth + 1);
        }
    };
    walk(root, 0);
    if (sounds.empty()) {
        if (error) *error = "no audio files in " + path;
        return {};
    }
    std::ostringstream o;
    o << "{\"_base\":" << jsonString(base);
    auto emit = [&](const std::string& key, const std::vector<std::string>& files) {
        o << ',' << jsonString(key) << ":[";
        for (size_t i = 0; i < files.size(); ++i) o << (i ? "," : "") << jsonString(files[i]);
        o << ']';
    };
    for (const auto& [n, files] : sounds) emit(n, files);
    o << '}';
    return o.str();
}

std::string SampleLibrary::folderMapJson(const std::string& name, std::string* error)
{
    LibraryFolder folder;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string key = lower(name);
        if (auto it = mapCache_.find(key); it != mapCache_.end()) return it->second;
        bool found = false;
        for (const auto& f : folders_)
            if (lower(f.name) == key) {
                folder = f;
                found = true;
            }
        if (!found) return {};
    }
    std::string json = scanFolder(folder.path, folder.name, error);
    if (!json.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        mapCache_[lower(name)] = json;
    }
    return json;
}

std::string SampleLibrary::autoloadJson() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string names;
    for (const auto& f : folders_)
        if (f.autoload) names += (names.empty() ? "" : ",") + jsonString(f.name);
    return "{\"version\":" + std::to_string(version_.load()) + ",\"names\":[" + names + "]}";
}

bool SampleLibrary::httpGet(const std::string& url, std::vector<unsigned char>& bytes, std::string& error)
{
    // disk cache: one file per URL
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dir = cacheDir_ + "/http";
    }
    char name[32];
    std::snprintf(name, sizeof name, "%016llx", (unsigned long long)fnv64(url));
    const std::string cached = dir + "/" + name;
    if (readFile(cached, bytes) && !bytes.empty()) return true;

    CURL* c = curl_easy_init();
    if (!c) {
        error = "curl init failed";
        return false;
    }
    bytes.clear();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &bytes);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Concha");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    const CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        error = std::string("download failed: ") + curl_easy_strerror(rc) + " (" + url + ")";
        return false;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream(cached + ".part", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    fs::rename(cached + ".part", cached, ec);
    return true;
}

bool SampleLibrary::resolveToBytes(const std::string& url, std::vector<unsigned char>& bytes, std::string& error)
{
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) return httpGet(url, bytes, error);
    std::string path = url;
    if (path.rfind("file://", 0) == 0) path = urlDecode(path.substr(7));
    if (path.rfind("~/", 0) == 0) path = homeDir() + path.substr(1);
    if (!readFile(path, bytes)) {
        error = "cannot read " + path;
        return false;
    }
    return true;
}

bool SampleLibrary::fetchText(const std::string& url, std::string& text, std::string& error)
{
    std::vector<unsigned char> bytes;
    if (!resolveToBytes(url, bytes, error)) return false;
    text.assign(bytes.begin(), bytes.end());
    return true;
}

uint32_t SampleLibrary::requestSample(const std::string& url)
{
    if (url.empty()) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = ids_.find(url); it != ids_.end()) return it->second;
    if (nextId_ >= kTransientBase) return 0;
    const uint32_t id = nextId_++;
    ids_[url] = id;
    queue_.emplace_back(id, url);
    cv_.notify_one();
    return id;
}

uint32_t SampleLibrary::publishTransient(const float* data, size_t frames, double sampleRate)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (transients_.empty()) transients_.resize(kTransientSlots);
    const auto now = std::chrono::steady_clock::now();
    for (uint32_t tries = 0; tries < kTransientSlots; ++tries) {
        const uint32_t slot = nextTransient_;
        nextTransient_ = (nextTransient_ + 1) % kTransientSlots;
        Transient& t = transients_[slot];
        if (t.data && now - t.published < std::chrono::seconds(60)) continue; // a voice may still play it
        auto d = std::make_unique<sd::SampleData>();
        d->channels = 1;
        d->frames = int64_t(frames);
        d->sampleRate = sampleRate;
        d->planar.assign(data, data + frames);
        const uint32_t id = kTransientBase + slot;
        store_.publishPointer(id, d.get());
        t.data = std::move(d); // the previous buffer (published over 60 s ago) is freed here
        t.published = now;
        return id;
    }
    return 0;
}

std::vector<std::string> SampleLibrary::takeErrors()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.swap(errors_);
    return out;
}

void SampleLibrary::loadSoundfontNote(uint32_t id, const std::string& url)
{
    auto fail = [&](const std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (errors_.size() < 100) errors_.push_back("[soundfont] " + url.substr(3) + ": " + error);
    };
    const size_t hash = url.rfind('#');
    if (hash == std::string::npos) return fail("no note");
    const std::string preset = url.substr(3, hash - 3);
    const double midi = std::atof(url.c_str() + hash + 1);
    std::shared_ptr<const std::vector<SoundfontZone>> zones;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = soundfonts_.find(preset); it != soundfonts_.end()) zones = it->second;
    }
    if (!zones) { // fetched once (http: through the disk cache), parsed once
        std::string text, error;
        if (!fetchText(preset, text, error)) return fail(error);
        auto parsed = std::make_shared<std::vector<SoundfontZone>>(parseSoundfont(text, error));
        if (parsed->empty()) return fail(error);
        std::lock_guard<std::mutex> lock(mutex_);
        zones = soundfonts_.emplace(preset, std::move(parsed)).first->second;
    }
    const int z = findSoundfontZone(*zones, midi);
    if (z < 0) return fail("no zone for note " + url.substr(hash + 1));
    const std::string zoneKey = preset + "#" + std::to_string(z);
    uint32_t owner = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto [it, fresh] = soundfontZones_.emplace(zoneKey, id);
        if (!fresh) owner = it->second;
    }
    if (owner) { // another note decodes this zone: share it once it is there
        for (int k = 0; k < 30000 && !store_.alias(id, owner); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return;
    }
    std::string error;
    auto data = decodeSoundfontZone((*zones)[size_t(z)], error);
    if (!data) return fail(error);
    store_.publish(id, std::move(data));
}

void SampleLibrary::loaderLoop()
{
    for (;;) {
        std::pair<uint32_t, std::string> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        if (job.second.rfind("sf:", 0) == 0) {
            loadSoundfontNote(job.first, job.second);
            continue;
        }
        std::vector<unsigned char> bytes;
        std::string error;
        std::unique_ptr<sd::SampleData> data;
        if (resolveToBytes(job.second, bytes, error)) data = decodeAudio(bytes, error);
        if (data) {
            store_.publish(job.first, std::move(data));
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            if (errors_.size() < 100) errors_.push_back("[sampler] " + job.second + ": " + error);
        }
    }
}

} // namespace concha::lib
