#include "sched/PatternWorker.h"
#include "superdough/PhaseVocoder.h"

#include "library/SampleLibrary.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace concha {

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
}

void PatternWorker::stop()
{
    if (!running_.exchange(false)) return;
    evalCv_.notify_all();
    if (queryThread_.joinable()) queryThread_.join();
    if (evalThread_.joinable()) evalThread_.join();
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (swapPending_) {
                active_ = 1 - active_;
                swapPending_ = false;
                status_.activeCode = standbyCode_;
                swapped = true;
            }
        }
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

void PatternWorker::pushEvents(const std::vector<PatternEvent>& events, const TransportState& t, int32_t lap,
                               double chunkStartPpq)
{
    const double N = std::max(1e-6, t.beatsPerCycle);
    const bool looping = t.looping && t.loopEnd > t.loopStart + 1e-9 && chunkStartPpq < t.loopEnd;
    const double loopLen = t.loopEnd - t.loopStart;
    uint64_t dropped = 0;

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
            if (!std::isnan(ev.sound.stretch)) { // superdough: t - 0.04 (vocoder latency), plus Concha's quantum
                ev.ppq -= sd::kStretchAdvanceSeconds(t.sampleRate) * t.bpm / 60.0;
                if (looping && ev.ppq < t.loopStart) {
                    ev.ppq += loopLen;
                    --ev.lap;
                }
            }
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
