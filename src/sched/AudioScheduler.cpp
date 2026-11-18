#include "sched/AudioScheduler.h"

#include <algorithm>
#include <cmath>

namespace concha {

namespace {
bool isNoteOff(const SchedEvent& e)
{
    if (e.type != SchedEvent::Type::Midi || e.midiLength < 3) return false;
    const uint8_t s = e.midi[0] & 0xf0;
    return s == 0x80 || (s == 0x90 && e.midi[2] == 0);
}

// (lap, ppq) ordering on the looped timeline.
bool before(int32_t lapA, double ppqA, int32_t lapB, double ppqB)
{
    return lapA < lapB || (lapA == lapB && ppqA < ppqB);
}
} // namespace

AudioScheduler::AudioScheduler(SpscRing<SchedEvent>& fromPattern, SeqLock<TransportState>& transport, SeqLock<SwapPoint>& swap)
    : in_(fromPattern), transport_(transport), swap_(swap)
{
}

void AudioScheduler::prepare(double sampleRate)
{
    sampleRate_ = sampleRate;
    wasPlaying_ = false;
    pendingCount_ = 0;
}

void AudioScheduler::releaseNotes(Sink& sink, int offset)
{
    for (int ch = 0; ch < 16; ++ch) {
        if (notesOn_[size_t(ch)].none()) continue;
        for (int n = 0; n < 128; ++n) {
            if (!notesOn_[size_t(ch)][size_t(n)]) continue;
            const uint8_t off[3] = {uint8_t(0x80 | ch), uint8_t(n), 0};
            sink.midi(offset, off, 3);
        }
        notesOn_[size_t(ch)].reset();
    }
}

void AudioScheduler::trackMidi(const uint8_t* b, int n)
{
    if (n < 3) return;
    const int s = b[0] & 0xf0, ch = b[0] & 0x0f;
    if (s == 0x90 && b[2] > 0) notesOn_[size_t(ch)].set(b[1] & 127);
    else if (s == 0x80 || s == 0x90) notesOn_[size_t(ch)].reset(b[1] & 127);
}

void AudioScheduler::jump(Sink& sink)
{
    ++epoch_;
    lap_ = 0;
    // Keep what the pattern thread prepared for this epoch (start from a stopped playhead).
    int w = 0;
    for (int i = 0; i < pendingCount_; ++i)
        if (pending_[size_t(i)].epoch == epoch_) pending_[size_t(w++)] = pending_[size_t(i)];
    pendingCount_ = w;
    releaseNotes(sink, 0);
    stats_.jumps.fetch_add(1, std::memory_order_relaxed);
}

void AudioScheduler::process(const HostPosition& host, int numSamples, double beatsPerCycle, Sink& sink)
{
    const double bpm = host.bpm > 0.0 ? host.bpm : 120.0;
    const double spb = sampleRate_ * 60.0 / bpm; // samples per beat
    const double blockBeats = numSamples / spb;
    const bool looping = host.looping && host.loopEnd > host.loopStart + 1e-9;

    // 1. Where are we? Continuous playback, or a jump.
    const double tol = std::max(1e-6, 2.0 / spb);
    if (host.playing) {
        if (wasPlaying_ && std::fabs(host.ppq - expectedPpq_) <= tol) lap_ = expectedLap_;
        else if (wasPlaying_ && std::fabs(host.ppq - altPpq_) <= tol) lap_ = altLap_;
        else jump(sink);
    } else if (wasPlaying_ || std::fabs(host.ppq - expectedPpq_) > tol) {
        // Stopped, or the playhead moved while stopped: what was prepared is no longer valid.
        jump(sink);
        pendingCount_ = 0;
    }
    wasPlaying_ = host.playing;

    // 2. Tell the pattern thread.
    TransportState ts;
    ts.ppq = host.ppq;
    ts.bpm = bpm;
    ts.beatsPerCycle = beatsPerCycle;
    ts.loopStart = host.loopStart;
    ts.loopEnd = host.loopEnd;
    ts.looping = looping;
    ts.sampleRate = sampleRate_;
    ts.blockBeats = blockBeats;
    ts.lap = lap_;
    ts.epoch = epoch_;
    ts.playing = host.playing;
    transport_.store(ts);

    // 3. New code?
    const SwapPoint sp = swap_.load();
    if (sp.codeGeneration != swapPoint_.codeGeneration) {
        swapPoint_ = sp;
        stats_.swaps.fetch_add(1, std::memory_order_relaxed);
    }

    // 4. Collect the queued events of this epoch.
    // While stopped, the pattern thread prepares the start: those events carry epoch_ + 1.
    const uint32_t accepted = host.playing ? epoch_ : epoch_ + 1;
    SchedEvent ev;
    while (in_.pop(ev)) {
        if (ev.epoch != accepted) continue;
        if (pendingCount_ >= kPending) {
            stats_.overflow.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        pending_[size_t(pendingCount_++)] = ev;
    }
    if (!host.playing) {
        expectedPpq_ = altPpq_ = host.ppq;
        expectedLap_ = altLap_ = lap_;
        return;
    }

    // 5. The block on the timeline: one segment, or two when it crosses the loop end.
    struct Segment {
        int32_t lap;
        double start, end;
        double firstSample;
    } segs[2];
    int nSegs = 1;
    segs[0] = {lap_, host.ppq, host.ppq + blockBeats, 0.0};
    if (looping && host.ppq < host.loopEnd && host.ppq + blockBeats > host.loopEnd) {
        segs[0].end = host.loopEnd;
        const double rest = host.ppq + blockBeats - host.loopEnd;
        segs[1] = {lap_ + 1, host.loopStart, host.loopStart + rest, (host.loopEnd - host.ppq) * spb};
        nSegs = 2;
    }

    // 6. Due events.
    int nDue = 0;
    for (int i = 0; i < pendingCount_; ++i) {
        const SchedEvent& e = pending_[size_t(i)];
        const bool off = isNoteOff(e);
        if (!off && e.codeGeneration < swapPoint_.codeGeneration && swapPoint_.epoch == epoch_ &&
            !before(e.lap, e.ppq, swapPoint_.lap, swapPoint_.ppq)) {
            pending_[size_t(i)].epoch = UINT32_MAX; // replaced by the new code
            continue;
        }
        int offset = -1;
        if (before(e.lap, e.ppq, segs[0].lap, segs[0].start)) { // late
            const double lateBeats = e.lap == segs[0].lap ? segs[0].start - e.ppq : 1e9;
            if (off || lateBeats <= kLateToleranceBeats) offset = 0;
            else {
                pending_[size_t(i)].epoch = UINT32_MAX;
                stats_.late.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
        } else {
            for (int s = 0; s < nSegs; ++s) {
                if (e.lap == segs[s].lap && e.ppq >= segs[s].start && e.ppq < segs[s].end) {
                    offset = int(std::floor(segs[s].firstSample + (e.ppq - segs[s].start) * spb + 1e-4));
                    break;
                }
            }
        }
        if (offset < 0 || offset >= numSamples) continue; // later (rounding may put it on the next block)
        due_[size_t(nDue++)] = {offset, off ? 0 : 1, i};
    }
    std::sort(due_.begin(), due_.begin() + nDue, [](const Due& a, const Due& b) {
        return a.offset != b.offset ? a.offset < b.offset : a.priority != b.priority ? a.priority < b.priority : a.index < b.index;
    });
    for (int k = 0; k < nDue; ++k) {
        SchedEvent& e = pending_[size_t(due_[size_t(k)].index)];
        if (e.type == SchedEvent::Type::Midi) {
            trackMidi(e.midi, e.midiLength);
            sink.midi(due_[size_t(k)].offset, e.midi, e.midiLength);
        } else {
            sink.sound(due_[size_t(k)].offset, e, spb);
        }
        e.epoch = UINT32_MAX; // done
        stats_.emitted.fetch_add(1, std::memory_order_relaxed);
    }

    // 7. Compact: remove the events that went out or were dropped.
    int w = 0;
    for (int i = 0; i < pendingCount_; ++i)
        if (pending_[size_t(i)].epoch == epoch_) pending_[size_t(w++)] = pending_[size_t(i)];
    pendingCount_ = w;

    // 8. Where the next block should start. A block ending exactly on the loop end may be followed
    // by the loop start (next lap) or, if the host wraps later, by the loop end itself.
    expectedPpq_ = altPpq_ = segs[nSegs - 1].end;
    expectedLap_ = altLap_ = segs[nSegs - 1].lap;
    if (looping && nSegs == 1 && std::fabs(segs[0].end - host.loopEnd) <= std::max(1e-6, 2.0 / spb)) {
        expectedPpq_ = host.loopStart;
        expectedLap_ = lap_ + 1;
    }
}

} // namespace concha
