#include "plugin/SyncProbe.h"

#include <cmath>

namespace concha {

void SyncProbe::prepare(double sampleRate, int numBuses)
{
    sampleRate_ = sampleRate;
    numBuses_ = numBuses;
    reset();
}

void SyncProbe::reset()
{
    for (auto& v : voices_) v = {};
    for (auto& o : offs_) o = {};
    wasPlaying_ = false;
}

void SyncProbe::startVoice(int bus, double freq, float amp, double lengthSeconds)
{
    for (auto& v : voices_) {
        if (v.remaining > 0) continue;
        v.bus = bus;
        v.phase = 0.0;
        v.inc = juce::MathConstants<double>::twoPi * freq / sampleRate_;
        v.amp = amp;
        v.remaining = static_cast<int>(lengthSeconds * sampleRate_);
        // -60 dB over the length of the voice.
        v.decay = static_cast<float>(std::pow(0.001, 1.0 / std::max(1, v.remaining)));
        return;
    }
}

void SyncProbe::renderVoices(int start, int end, juce::AudioBuffer<float>* const* buses, int numBuses)
{
    for (auto& v : voices_) {
        if (v.remaining <= 0 || v.bus < 0 || v.bus >= numBuses || buses[v.bus] == nullptr) continue;
        auto& buf = *buses[v.bus];
        const int n = std::min(end - start, v.remaining);
        for (int i = 0; i < n; ++i) {
            const float s = v.amp * static_cast<float>(std::sin(v.phase));
            v.phase += v.inc;
            v.amp *= v.decay;
            for (int ch = 0; ch < buf.getNumChannels(); ++ch) buf.addSample(ch, start + i, s);
        }
        v.remaining -= n;
    }
}

void SyncProbe::process(const TransportSnapshot& t, int numSamples,
                        juce::AudioBuffer<float>* const* buses, int numBuses, juce::MidiBuffer& midiOut)
{
    // Note-offs that were scheduled in an earlier block.
    for (auto& o : offs_) {
        if (o.note < 0) continue;
        if (!t.playing || o.samplesLeft < numSamples) {
            const int at = t.playing ? static_cast<int>(std::max<int64_t>(0, o.samplesLeft)) : 0;
            midiOut.addEvent(juce::MidiMessage::noteOff(1, o.note), at);
            o.note = -1;
        } else {
            o.samplesLeft -= numSamples;
        }
    }
    if (!t.playing) {
        if (wasPlaying_) midiOut.addEvent(juce::MidiMessage::allNotesOff(1), 0);
        wasPlaying_ = false;
        renderVoices(0, numSamples, buses, numBuses); // let the tails ring out
        return;
    }
    wasPlaying_ = true;

    const double samplesPerBeat = sampleRate_ * 60.0 / t.bpm;
    const int beatsPerBar = std::max(1, t.beatsPerBar);
    int cursor = 0;
    forEachBeatInBlock(t.ppq, t.bpm, sampleRate_, numSamples, [&](int64_t beat, int offset) {
        renderVoices(cursor, offset, buses, numBuses);
        cursor = offset;

        const bool downbeat = (beat % beatsPerBar) == 0;
        const int64_t bar = beat / beatsPerBar;
        startVoice(0, downbeat ? 1760.0 : 880.0, 0.5f, 0.04);
        if (downbeat) {
            for (int b = 1; b < numBuses; ++b)
                startVoice(b, 220.0 * std::pow(2.0, b / 12.0), 0.35f, 0.25);
            midiOut.addEvent(juce::MidiMessage::programChange(1, static_cast<int>(bar % 8)), offset);
        }

        const int note = downbeat ? 72 : 60;
        midiOut.addEvent(juce::MidiMessage::controllerEvent(1, 74, static_cast<int>((beat % 8) * 16)), offset);
        midiOut.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(100)), offset);
        const auto offAt = static_cast<int64_t>(offset) + static_cast<int64_t>(samplesPerBeat * 0.5);
        if (offAt < numSamples) {
            midiOut.addEvent(juce::MidiMessage::noteOff(1, note), static_cast<int>(offAt));
        } else {
            for (auto& o : offs_)
                if (o.note < 0) { o.note = note; o.samplesLeft = offAt - numSamples; break; }
        }
    });
    renderVoices(cursor, numSamples, buses, numBuses);
}

} // namespace concha
