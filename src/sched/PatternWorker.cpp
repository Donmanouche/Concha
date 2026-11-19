#include "sched/PatternWorker.h"
#include "superdough/PhaseVocoder.h"
#include "io/ExternalOut.h"
#include "sc/ScDefs.h"

#include "library/SampleLibrary.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace concha {

static std::vector<PatternWorker::ActiveCodeInfo::Block> parseBlocks(const std::string& code);


namespace {

using Clock = std::chrono::steady_clock;

// "[145,60,114]" -> bytes
int parseBytes(const std::string& json, uint8_t* out, int max)
{
    int n = 0;
    const char* p = json.c_str();
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            char* endp = nullptr;
            const long v = std::strtol(p, &endp, 10);
            if (n >= max) return -1;
            out[n++] = static_cast<uint8_t>(v & 0xff);
            p = endp;
        } else {
            ++p;
        }
    }
    return n;
}

} // namespace

PatternWorker::PatternWorker(SpscRing<SchedEvent>& toAudio, SeqLock<TransportState>& transport, SeqLock<SwapPoint>& swap)
    : PatternWorker(toAudio, transport, swap, Options{})
{
}

PatternWorker::PatternWorker(SpscRing<SchedEvent>& toAudio, SeqLock<TransportState>& transport, SeqLock<SwapPoint>& swap,
                             Options options)
    : toAudio_(toAudio), transport_(transport), swap_(swap), options_(std::move(options))
{
    converter_.library = options_.library;
}

PatternWorker::~PatternWorker() { stop(); }

void PatternWorker::start()
{
    if (running_.exchange(true)) return;
    queryThread_ = std::thread([this] { queryLoop(); });
    visualThread_ = std::thread([this] { visualLoop(); });
}

void PatternWorker::stop()
{
    if (!running_.exchange(false)) return;
    evalCv_.notify_all();
    if (queryThread_.joinable()) queryThread_.join();
    if (evalThread_.joinable()) evalThread_.join();
    if (visualThread_.joinable()) visualThread_.join();
}

void PatternWorker::requestLibraryRefresh() { libraryRefresh_.store(true); }

// On the query thread, with the playing engine: the library's sounds (concha.refreshLibrary), the
// user's SynthDefs; the standby engine catches up at its next evaluation (versions).
void PatternWorker::refreshLibrary()
{
    PatternEngine& engine = *engines_[active_];
    if (options_.library) {
        for (auto& m : sc::loadUserSynthDefFolders(options_.library->synthDefFolders()))
            log(m.rfind("loaded", 0) == 0 ? "info" : "warning", std::move(m));
        const int n = sc::scDefCount();
        if (n != registeredDefs_[active_]) {
            if (options_.onNewSynthDefs) options_.onNewSynthDefs();
            registerSuperDirtSynths(engine);
            registeredDefs_[active_] = n;
        }
    }
    std::string json;
    if (!engine.evalJson("concha.refreshLibrary()", json)) return;
    std::vector<std::string> names;
    for (size_t i = 0; (i = json.find('"', i)) != std::string::npos;) {
        const size_t j = json.find('"', i + 1);
        if (j == std::string::npos) break;
        names.push_back(json.substr(i + 1, j - i - 1));
        i = j + 1;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    activeInfo_.soundNames = std::move(names);
    ++status_.soundsVersion;
}

void PatternWorker::requestEvaluate(std::string code)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingCode_ = std::move(code);
    }
    evalCv_.notify_all();
}

PatternWorker::Status PatternWorker::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::vector<LogMessage> PatternWorker::takeLog()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LogMessage> out;
    out.swap(log_);
    return out;
}

bool PatternWorker::queuedUpTo(int32_t lap, double ppq) const
{
    const int32_t l = queuedLap_.load();
    return l > lap || (l == lap && queuedPpq_.load() >= ppq);
}

void PatternWorker::log(std::string type, std::string message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_.size() < 1000) log_.push_back({std::move(type), std::move(message)});
}

void PatternWorker::queryLoop()
{
    // Both engines are created here; afterwards each is used by one thread at a time
    // (PatternEngine re-anchors QuickJS's stack check to the calling thread).
    for (auto& e : engines_) {
        e = std::make_unique<PatternEngine>(options_.engine);
        if (!e->ok()) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.engineError = e->initError();
            return;
        }
        e->takeLog();
        registerSuperDirtSynths(*e);
        registeredDefs_[&e - engines_] = sc::scDefCount();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.ready = true;
    }
    evalThread_ = std::thread([this] { evalLoop(); });
    evalCv_.notify_all();

    while (running_.load()) {
        const TransportState t = transport_.load();
        bool swapped = false;
        std::vector<std::pair<std::string, double>> moves;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            moves.swap(sliderMoves_);
            if (swapPending_) {
                active_ = 1 - active_;
                swapPending_ = false;
                status_.activeCode = standbyCode_;
                status_.activeGeneration = codeGeneration_ + 1;
                activeInfo_.generation = status_.activeGeneration;
                activeInfo_.miniLocations = standbyMini_;
                activeInfo_.sliders = standbySliders_;
                activeInfo_.widgets = standbyWidgets_;
                activeInfo_.shader = standbyShader_;
                activeInfo_.visuals = standbyVisuals_;
                activeInfo_.blocks = parseBlocks(standbyCode_);
                activeInfo_.soundNames = standbySounds_;
                ++status_.soundsVersion;
                swapped = true;
            }
        }
        for (const auto& [id, value] : moves) engines_[active_]->setSlider(id, value); // active_ only changes here
        if (swapped) {
            ++codeGeneration_;
            // New code starts a little after the playhead; events of the old code from there on are
            // dropped by the audio thread.
            SwapPoint sp;
            sp.codeGeneration = codeGeneration_;
            sp.epoch = t.epoch;
            sp.lap = t.lap;
            sp.ppq = t.ppq + t.blockBeats + options_.swapMarginSeconds * t.bpm / 60.0;
            if (t.looping && t.loopEnd > t.loopStart && t.ppq < t.loopEnd && sp.ppq >= t.loopEnd) {
                sp.ppq = t.loopStart + (sp.ppq - t.loopEnd);
                ++sp.lap;
            }
            if (!t.playing) { // stopped: the prepared start is redone with the new code
                sp.epoch = t.epoch + 1;
                sp.lap = 0;
                sp.ppq = t.ppq;
            }
            swap_.store(sp);
            if (sp.epoch == epoch_) {
                cursorLap_ = sp.lap;
                cursorPpq_ = sp.ppq;
                queuedLap_ = sp.lap;
                queuedPpq_ = sp.ppq;
            }
            evalCv_.notify_all(); // the evaluation thread may take the next request
        }

        runQueries(t);
        if (libraryRefresh_.exchange(false)) refreshLibrary();
        playMidiNotes(t);
        flushExternals(t);
        runChannelQueries(t);

        PatternEngine& engine = *engines_[active_];
        for (auto& m : engine.takeLog()) log(std::move(m.type), std::move(m.message));
        for (auto& m : convertLog_) log("warning", std::move(m));
        convertLog_.clear();
        if (options_.library)
            for (auto& m : options_.library->takeErrors()) log("error", std::move(m));
        std::this_thread::sleep_for(options_.period);
    }
}

void PatternWorker::evalLoop()
{
    while (running_.load()) {
        std::string code;
        PatternEngine* standby = nullptr;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            evalCv_.wait(lock, [this] { return !running_.load() || (pendingCode_ && !swapPending_); });
            if (!running_.load()) return;
            code = std::move(*pendingCode_);
            pendingCode_.reset();
            standby = engines_[1 - active_].get();
            status_.evaluating = true;
        }
        if (options_.library) { // the user's SynthDefs: new or changed .scsyndef files
            for (auto& m : sc::loadUserSynthDefFolders(options_.library->synthDefFolders()))
                log(m.rfind("loaded", 0) == 0 ? "info" : "warning", std::move(m));
            const int n = sc::scDefCount();
            int& registered = registeredDefs_[standby == engines_[0].get() ? 0 : 1];
            if (n != registered) {
                if (options_.onNewSynthDefs) options_.onNewSynthDefs();
                registerSuperDirtSynths(*standby);
                registered = n;
            }
        }
        const EvalResult r = standby->evaluate(code);
        if (r.ok && options_.library) {
            // Warm-up: the samples of the next two cycles start loading now; wait a little for them.
            const TransportState t = transport_.load();
            const double N = std::max(1e-6, t.beatsPerCycle);
            const double from = t.ppq / N;
            std::vector<PatternEvent> events;
            std::vector<uint32_t> ids;
            if (standby->query(from, from + 2.0, t.bpm / 60.0 / N, events))
                for (const auto& e : events)
                    if (const ParamValue* url = e.find("__url"))
                        if (const uint32_t id = options_.library->requestSample(url->string)) ids.push_back(id);
            const auto until = Clock::now() + options_.sampleWarmup;
            for (uint32_t id : ids)
                while (!options_.library->isLoaded(id) && Clock::now() < until)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::vector<std::string> sounds; // for the editor's sound list
        if (r.ok) {
            std::string json;
            if (standby->evalJson("concha.soundNames()", json)) {
                for (size_t i = 0; (i = json.find('"', i)) != std::string::npos;) {
                    const size_t j = json.find('"', i + 1);
                    if (j == std::string::npos) break;
                    sounds.push_back(json.substr(i + 1, j - i - 1));
                    i = j + 1;
                }
            }
        }
        auto messages = standby->takeLog();
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& m : messages)
            if (log_.size() < 1000) log_.push_back(std::move(m));
        status_.evaluating = false;
        ++status_.evaluations;
        status_.lastOk = r.ok;
        status_.lastError = r.error;
        status_.lastEvalMs = r.milliseconds;
        if (r.ok) {
            swapPending_ = true; // on error the current code keeps playing, as in Strudel
            standbyCode_ = code;
            standbyMini_ = r.miniLocations;
            standbySliders_ = r.sliders;
            standbyWidgets_ = r.widgets;
            standbyShader_ = r.shader;
            standbyVisuals_ = r.visuals;
            standbySounds_ = sounds;
        } else if (log_.size() < 1000) {
            log_.push_back({"error", r.error});
        }
    }
}

void PatternWorker::runQueries(const TransportState& t)
{
    for (auto& e : engines_) e->setSampleRate(t.sampleRate); // zzfx / bytebeat render at the engine's rate
    // Stopped: prepare the start from the stopped playhead, for the epoch the start will open.
    const uint32_t target = t.playing ? t.epoch : t.epoch + 1;
    if (target != epoch_) { // jump, stop, playhead moved: start again from the playhead
        epoch_ = target;
        cursorLap_ = 0;
        cursorPpq_ = t.ppq;
        startedFromPpq_ = t.ppq;
        queuedLap_ = 0;
        queuedPpq_ = t.ppq;
    }
    if (t.playing && t.lap == cursorLap_ && (t.ppq < startedFromPpq_ - 1e-6)) {
        // started before the prepared position (pre-roll...): query from the real start
        cursorPpq_ = t.ppq;
    }
    const double N = std::max(1e-6, t.beatsPerCycle);
    const bool looping = t.looping && t.loopEnd > t.loopStart + 1e-9;
    const double loopLen = looping ? t.loopEnd - t.loopStart : 0.0;
    auto unwrapped = [&](int32_t lap, double ppq) { return looping ? lap * loopLen + ppq : ppq; };
    const double horizon = options_.lookaheadSeconds * t.bpm / 60.0;
    const double cps = t.bpm / 60.0 / N;
    PatternEngine& engine = *engines_[active_];

    const int32_t playLap = t.playing ? t.lap : 0;
    double ahead = unwrapped(cursorLap_, cursorPpq_) - unwrapped(playLap, t.ppq);
    if (ahead < -t.blockBeats * 2 - 1e-9) { // the worker fell behind: the skipped events are lost
        log("warning", "[scheduler] pattern thread late, events skipped");
        cursorLap_ = playLap;
        cursorPpq_ = t.ppq;
        ahead = 0.0;
    }
    for (int guard = 0; ahead < horizon && guard < 64; ++guard) {
        double chunkEnd = cursorPpq_ + std::min(options_.maxChunkBeats, horizon - ahead + 1e-6);
        const bool beforeLoopEnd = looping && cursorPpq_ < t.loopEnd;
        if (beforeLoopEnd) chunkEnd = std::min(chunkEnd, t.loopEnd);
        if (chunkEnd <= cursorPpq_) break;

        std::string err;
        const auto q0 = Clock::now();
        const bool ok = engine.query(cursorPpq_ / N, chunkEnd / N, cps, scratch_, &err);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - q0).count();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++status_.queries;
            status_.worstQueryMs = std::max(status_.worstQueryMs, ms);
        }
        if (ok) pushEvents(scratch_, t, cursorLap_, cursorPpq_);

        ahead += chunkEnd - cursorPpq_;
        cursorPpq_ = chunkEnd;
        if (beforeLoopEnd && cursorPpq_ >= t.loopEnd - 1e-9) {
            cursorPpq_ = t.loopStart;
            ++cursorLap_;
        }
        queuedPpq_ = cursorPpq_;
        queuedLap_ = cursorLap_;
    }
}

// MQTT and serial messages: handed to the sender with their time once their epoch plays (the
// prepared start waits for the start); messages of an older epoch (stop, jump) are dropped.
// The code's blocks, as Strudel's labels: a line starting with `name:` or `$:` opens a block (`_name:`
// is muted, still a block). No label: the whole code is one block.
static std::vector<PatternWorker::ActiveCodeInfo::Block> parseBlocks(const std::string& code)
{
    std::vector<PatternWorker::ActiveCodeInfo::Block> blocks;
    int dollars = 0;
    size_t lineStart = 0;
    while (lineStart <= code.size()) {
        size_t i = lineStart;
        while (i < code.size() && (code[i] == ' ' || code[i] == '\t')) ++i;
        size_t j = i;
        while (j < code.size() && (std::isalnum(uint8_t(code[j])) || code[j] == '_' || code[j] == '$')) ++j;
        if (j > i && j < code.size() && code[j] == ':' && (j + 1 >= code.size() || code[j + 1] != ':')) {
            std::string name = code.substr(i, j - i);
            if (!blocks.empty()) blocks.back().end = int(lineStart);
            if (name == "$" || name == "_$") name = "$" + std::to_string(++dollars);
            if (name.size() > 1 && name[0] == '_') name = name.substr(1);
            blocks.push_back({name, int(lineStart), int(code.size())});
        }
        const size_t nl = code.find('\n', lineStart);
        if (nl == std::string::npos) break;
        lineStart = nl + 1;
    }
    if (blocks.empty()) blocks.push_back({"pattern", 0, int(code.size())});
    return blocks;
}

std::vector<PatternWorker::ChannelHap> PatternWorker::channelHaps(double from, double to, uint32_t& generation)
{
    std::lock_guard<std::mutex> lock(channelMutex_);
    generation = channelGeneration_;
    std::vector<ChannelHap> out;
    for (const auto& h : channel_)
        if (h.end > from && h.begin < to) out.push_back(h);
    return out;
}

// midi number of a hap: note (name or number), midinote, freq; NaN without pitch
static float hapPitch(const PatternEvent& e)
{
    if (const ParamValue* n = e.find("note")) {
        if (n->type == ParamValue::Type::Number) return float(n->number);
        static const int chroma[7] = {9, 11, 0, 2, 4, 5, 7}; // a..g
        const std::string& s = n->string;
        if (!s.empty() && std::tolower(uint8_t(s[0])) >= 'a' && std::tolower(uint8_t(s[0])) <= 'g') {
            int m = chroma[std::tolower(uint8_t(s[0])) - 'a'];
            size_t i = 1;
            for (; i < s.size() && (s[i] == '#' || s[i] == 's' || s[i] == 'b' || s[i] == 'f'); ++i) m += s[i] == '#' || s[i] == 's' ? 1 : -1;
            const int octave = i < s.size() ? std::atoi(s.c_str() + i) : 3;
            return float((octave + 1) * 12 + m);
        }
    }
    if (const ParamValue* m = e.find("midinote"); m && m->type == ParamValue::Type::Number) return float(m->number);
    if (const ParamValue* f = e.find("freq"); f && f->type == ParamValue::Type::Number && f->number > 0)
        return float(69 + 12 * std::log2(f->number / 440.0));
    return NAN;
}

// Haps for the oscilloscope's channels, queried ahead in short chunks (the music's queries first).
void PatternWorker::runChannelQueries(const TransportState& t)
{
    bool wanted;
    uint32_t generation;
    std::vector<ActiveCodeInfo::Block> blocks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wanted = visuals_.wanted && activeInfo_.visuals.channels;
        generation = activeInfo_.generation;
        if (wanted && generation != channelGeneration_) blocks = activeInfo_.blocks;
    }
    if (!wanted) return;
    const double N = std::max(1e-6, t.beatsPerCycle);
    const double cps = t.bpm / 60.0 / N;
    const double now = t.ppq / N;
    if (generation != channelGeneration_) {
        std::lock_guard<std::mutex> lock(channelMutex_);
        channel_.clear();
        channelGeneration_ = generation;
        channelCursor_ = now - 1.3;
        channelBlocks_ = blocks;
    }
    if (channelCursor_ < now - 2.0 || channelCursor_ > now + 3.0) { // a jump or a loop
        std::lock_guard<std::mutex> lock(channelMutex_);
        channel_.clear();
        channelCursor_ = now - 1.3;
    }
    PatternEngine& engine = *engines_[active_];
    for (int chunk = 0; chunk < 2 && channelCursor_ < now + 1.5; ++chunk) {
        const double end = channelCursor_ + 0.25;
        std::vector<PatternEvent> events;
        if (!engine.query(channelCursor_, end, cps, events)) break;
        std::lock_guard<std::mutex> lock(channelMutex_);
        for (const auto& e : events) {
            if (!e.visual) continue;
            ChannelHap h;
            h.begin = e.begin;
            h.end = e.begin + std::max(1e-3, e.duration);
            const int at = e.locations.empty() ? 0 : e.locations.front().first;
            for (size_t b = 0; b < channelBlocks_.size(); ++b)
                if (at >= channelBlocks_[b].start && at < channelBlocks_[b].end) h.block = int(b);
            h.note = hapPitch(e);
            if (const ParamValue* g = e.find("gain"); g && g->type == ParamValue::Type::Number) h.gain = float(g->number);
            if (const ParamValue* v = e.find("velocity"); v && v->type == ParamValue::Type::Number) h.gain *= float(v->number);
            if (const ParamValue* s = e.find("s"); s && s->type == ParamValue::Type::String) h.s = s->string;
            channel_.push_back(std::move(h));
        }
        channelCursor_ = end;
    }
    std::lock_guard<std::mutex> lock(channelMutex_);
    while (!channel_.empty() && channel_.front().end < now - 2.0) channel_.pop_front();
    while (channel_.size() > 20000) channel_.pop_front();
}

void PatternWorker::setVisuals(bool wanted, int width, int height, double pixelRatio)
{
    std::lock_guard<std::mutex> lock(mutex_);
    visuals_ = {wanted, std::max(1, width), std::max(1, height), pixelRatio > 0 ? pixelRatio : 1.0};
}

std::vector<PatternWorker::Frame> PatternWorker::takeFrames()
{
    std::lock_guard<std::mutex> lock(framesMutex_);
    std::vector<Frame> out(std::make_move_iterator(frames_.begin()), std::make_move_iterator(frames_.end()));
    frames_.clear();
    return out;
}

// Visuals on their own thread and JS engine: the playing code is evaluated there too (patterns are
// deterministic; this engine is never queried for sound), and its requestAnimationFrame callbacks
// run at the transport's cycle (draw.mjs getTime()). A slow frame never delays the music.
void PatternWorker::visualLoop()
{
    std::unique_ptr<PatternEngine> engine;
    uint32_t evaluated = 0;
    VisualsRequest sent;
    size_t slidersSeen = 0;
    auto lastFrame = Clock::now();
    while (running_.load()) {
        VisualsRequest v;
        uint32_t generation;
        std::string code;
        std::vector<std::pair<std::string, double>> sliders;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            v = visuals_;
            generation = status_.activeGeneration;
            if (generation != evaluated) code = status_.activeCode;
            if (visualSliders_.size() != slidersSeen) {
                sliders.assign(visualSliders_.begin() + long(slidersSeen), visualSliders_.end());
                slidersSeen = visualSliders_.size();
            }
        }
        bool custom;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            custom = activeInfo_.visuals.custom;
        }
        if (!v.wanted || generation == 0 || !custom) { // only a user's own .draw() needs this engine
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (!engine) {
            engine = std::make_unique<PatternEngine>(options_.engine);
            if (!engine->ok()) return;
            engine->takeLog();
        }
        if (sent.width != v.width || sent.height != v.height || sent.pixelRatio != v.pixelRatio) {
            engine->setViewport(v.width, v.height, v.pixelRatio);
            sent = v;
        }
        if (generation != evaluated) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                visualSliders_.clear();
                slidersSeen = 0;
            }
            sliders.clear();
            engine->evaluate(code);
            engine->takeLog(); // the sound engines already reported this code's messages
            evaluated = generation;
        }
        for (const auto& [id, value] : sliders) engine->setSlider(id, value);
        const auto next = lastFrame + std::chrono::microseconds(16667);
        const auto now = Clock::now();
        if (now < next) std::this_thread::sleep_for(next - now);
        lastFrame = Clock::now();
        if (!engine->hasFrames()) continue;
        const TransportState t = transport_.load();
        const double N = std::max(1e-6, t.beatsPerCycle);
        Frame f;
        f.generation = evaluated;
        const double ms = std::chrono::duration<double, std::milli>(lastFrame.time_since_epoch()).count();
        if (!engine->frame(t.ppq / N, ms, f.canvases)) {
            for (auto& m : engine->takeLog()) log(std::move(m.type), std::move(m.message));
            continue;
        }
        for (auto& m : engine->takeLog()) log(std::move(m.type), std::move(m.message));
        std::lock_guard<std::mutex> lock(framesMutex_);
        frames_.push_back(std::move(f));
        while (frames_.size() > 8) frames_.pop_front(); // the window is not reading: keep the latest
    }
}

void PatternWorker::flushExternals(const TransportState& t)
{
    for (auto& m : io::takeMessages()) log(m.find("connected") != std::string::npos || m.find("opened") != std::string::npos ? "info" : "warning", std::move(m));
    if (externals_.empty()) return;
    const bool looping = t.looping && t.loopEnd > t.loopStart + 1e-9;
    const double loopLen = looping ? t.loopEnd - t.loopStart : 0.0;
    auto unwrapped = [&](int32_t lap, double ppq) { return looping ? lap * loopLen + ppq : ppq; };
    const auto now = io::Clock::now();
    size_t kept = 0;
    for (auto& x : externals_) {
        if (x.epoch < t.epoch || (!t.playing && x.epoch <= t.epoch)) continue; // dropped
        if (!t.playing || x.epoch != t.epoch) {
            externals_[kept++] = std::move(x);
            continue;
        }
        const double seconds = (unwrapped(x.lap, x.ppq) - unwrapped(t.lap, t.ppq)) * 60.0 / std::max(1e-6, t.bpm);
        const PatternEvent& e = x.event;
        auto str = [&](const char* k) {
            const ParamValue* v = e.find(k);
            return v ? (v->type == ParamValue::Type::String ? v->string : std::to_string(v->number)) : std::string();
        };
        auto num = [&](const char* k, double d) {
            const ParamValue* v = e.find(k);
            return v && v->type == ParamValue::Type::Number ? v->number : d;
        };
        const auto due = now + std::chrono::microseconds(int64_t(std::max(0.0, seconds + num("latency", 0)) * 1e6));
        if (e.kind == "mqtt") {
            io::MqttMessage m;
            m.host = str("host");
            m.client = str("client");
            m.user = str("user");
            m.password = str("password");
            m.topic = str("topic");
            m.payload = str("payload");
            io::sendMqtt(std::move(m), due);
        } else {
            const std::string hex = str("bytes");
            std::vector<uint8_t> bytes;
            for (size_t i = 0; i + 1 < hex.size(); i += 2) bytes.push_back(uint8_t(std::stoi(hex.substr(i, 2), nullptr, 16)));
            io::sendSerial(str("port"), int(num("baud", 115200)), std::move(bytes), due);
        }
    }
    externals_.resize(kept);
}

void PatternWorker::playMidiNotes(const TransportState& t)
{
    if (!options_.midiNotes) return;
    PatternEngine& engine = *engines_[active_];
    const double N = std::max(1e-6, t.beatsPerCycle);
    const double cps = t.bpm / 60.0 / N;
    MidiInNote n;
    while (options_.midiNotes->pop(n)) {
        if (!t.playing || n.epoch != t.epoch) continue; // as Strudel: no notes while stopped
        const double cycle = n.ppq / N;
        char call[160];
        std::snprintf(call, sizeof call, "concha.pushKey(%d,%d,%d,%.17g)", n.note, n.velocity, n.channel, cycle);
        std::string json;
        if (!engine.evalJson(call, json) || json != "true") continue; // no midikeys() in the code
        std::string err;
        if (engine.query(cycle - 1e-6, cycle + 1e-6, cps, scratch_, &err, true)) pushEvents(scratch_, t, n.lap, n.ppq);
    }
}

PatternWorker::ActiveCodeInfo PatternWorker::activeCodeInfo() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return activeInfo_;
}

void PatternWorker::setSlider(std::string id, double value)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (visualSliders_.size() < 4096) visualSliders_.emplace_back(id, value); // the visuals' engine too
    for (auto& m : sliderMoves_)
        if (m.first == id) {
            m.second = value;
            return;
        }
    sliderMoves_.emplace_back(std::move(id), value);
}

std::vector<PatternWorker::VisualHap> PatternWorker::activeHaps(double cycle)
{
    std::vector<VisualHap> out;
    std::lock_guard<std::mutex> lock(visualMutex_);
    // drop what has ended long ago, and what a jump back left far ahead
    while (!visual_.empty() && (visual_.front().end < cycle - 4 || visual_.front().begin > cycle + 16)) visual_.pop_front();
    for (const auto& v : visual_)
        if (v.begin <= cycle && cycle < v.end) out.push_back(v);
    return out;
}

void PatternWorker::pushEvents(const std::vector<PatternEvent>& events, const TransportState& t, int32_t lap,
                               double chunkStartPpq)
{
    const double N = std::max(1e-6, t.beatsPerCycle);
    const bool looping = t.looping && t.loopEnd > t.loopStart + 1e-9 && chunkStartPpq < t.loopEnd;
    const double loopLen = t.loopEnd - t.loopStart;
    uint64_t dropped = 0;

    {
        std::lock_guard<std::mutex> lock(visualMutex_);
        for (const PatternEvent& e : events) {
            if (e.locations.empty() && !e.find("uniform")) continue; // highlighting, shader uniforms
            if (visual_.size() >= 8192) visual_.pop_front();
            VisualHap v;
            v.begin = e.begin;
            v.end = e.end;
            v.generation = codeGeneration_;
            if (const ParamValue* c = e.find("color"); c && c->type == ParamValue::Type::String) v.color = c->string;
            if (const ParamValue* u = e.find("uniform"); u && u->type == ParamValue::Type::String) {
                v.uniform = u->string;
                const ParamValue* g = e.find("gain");
                const ParamValue* vel = e.find("velocity");
                v.uniformValue = (g && g->type == ParamValue::Type::Number ? g->number : 1.0) *
                                 (vel && vel->type == ParamValue::Type::Number ? vel->number : 1.0);
            }
            v.locations = e.locations;
            visual_.push_back(std::move(v));
        }
    }
    for (const PatternEvent& e : events) {
        SchedEvent ev;
        ev.epoch = epoch_;
        ev.codeGeneration = codeGeneration_;
        ev.lap = lap;
        ev.ppq = e.begin * N;
        // Messages after the loop end (note-offs of long notes) belong to the next laps.
        while (looping && ev.ppq >= t.loopEnd) {
            ev.ppq -= loopLen;
            ++ev.lap;
        }
        if (e.kind == "midimsg") {
            const ParamValue* bytes = e.find("bytes");
            if (!bytes) continue;
            ev.type = SchedEvent::Type::Midi;
            const int n = parseBytes(bytes->string, ev.midi, SchedEvent::kMaxMidi);
            if (n <= 0) {
                ++dropped;
                log("warning", "[midi] message too long (sysex over 64 bytes), dropped");
                continue;
            }
            ev.midiLength = static_cast<uint8_t>(n);
        } else if (e.kind == "sound") {
            ev.type = SchedEvent::Type::Sound;
            ev.durationBeats = static_cast<float>(e.duration * N);
            ev.cycle = e.begin;
            converter_.sampleRate = t.sampleRate;
            if (!converter_.convert(e, ev.sound, convertLog_)) continue;
            double advance = 0; // seconds: stretch (superdough's 40 ms + Concha's quantum), StrudelDirt effects
            if (!std::isnan(ev.sound.stretch)) advance += sd::kStretchAdvanceSeconds(t.sampleRate);
            if (ev.sound.numFxModules > 0) advance += sd::FxServer::kLatency / t.sampleRate;
            if (advance > 0) {
                ev.ppq -= advance * t.bpm / 60.0;
                if (looping && ev.ppq < t.loopStart) {
                    ev.ppq += loopLen;
                    --ev.lap;
                }
            }
        } else if (e.kind == "mqtt" || e.kind == "serial") { // sent from Concha at their time (flushExternals)
            if (externals_.size() < 4096) externals_.push_back({ev.epoch, ev.lap, ev.ppq, e});
            continue;
        } else {
            continue; // osc, csound...: later phases
        }
        if (!toAudio_.push(ev)) ++dropped;
    }
    if (dropped) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.droppedEvents += dropped;
    }
}

} // namespace concha
