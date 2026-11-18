#pragma once

// WebAudio AudioParam automation (https://webaudio.github.io/web-audio-api/#AudioParam), so that
// superdough's envelopes (getParamADSR, filter / pitch envelopes...) can be ported line by line.
// Times are in seconds on the voice's clock. Fixed capacity, no allocation.
//
// Implemented: setValueAtTime, linearRampToValueAtTime, exponentialRampToValueAtTime,
// setTargetAtTime, cancelScheduledValues; the intrinsic value is computed per sample (a-rate).
// Modulation inputs (connected nodes) are added by the caller.

#include <array>
#include <cmath>

namespace concha::sd {

class AudioParam {
public:
    explicit AudioParam(float defaultValue = 0.f, float minValue = -3.4028235e38f, float maxValue = 3.4028235e38f)
        : default_(defaultValue), min_(minValue), max_(maxValue), value_(defaultValue)
    {
    }

    void reset(float v)
    {
        value_ = v;
        count_ = 0;
        current_ = 0;
        lastValue_ = v;
    }
    // AudioParam.value = v (no automation yet): the value until the first event.
    void setValue(float v)
    {
        value_ = v;
        if (count_ == 0) lastValue_ = v;
    }
    float value() const { return value_; }
    bool automated() const { return count_ > 0; }

    void setValueAtTime(float v, double t) { insert({Type::Set, v, t, 0.0}); }
    void linearRampToValueAtTime(float v, double t) { insert({Type::Linear, v, t, 0.0}); }
    void exponentialRampToValueAtTime(float v, double t) { insert({Type::Exponential, v, t, 0.0}); }
    void setTargetAtTime(float v, double t, double timeConstant) { insert({Type::Target, v, t, timeConstant}); }
    void cancelScheduledValues(double t)
    {
        int w = 0;
        for (int i = 0; i < count_; ++i)
            if (events_[size_t(i)].time < t) events_[size_t(w++)] = events_[size_t(i)];
        count_ = w;
        if (current_ > count_) current_ = count_;
    }

    // Intrinsic value at time t. Calls must have non-decreasing t (per-sample rendering).
    float valueAt(double t)
    {
        if (count_ == 0) return clamp(value_);
        // Advance past events that are fully in the past.
        while (current_ < count_ && events_[size_t(current_)].time <= t) {
            const Event& e = events_[size_t(current_)];
            // A ramp ends exactly at its event: from then on the value is the event's.
            if (e.type != Type::Target) {
                lastValue_ = e.value;
                lastTime_ = e.time;
            } else {
                // setTarget starts from the value the param has when it begins
                targetStart_ = lastType_ == Type::Target ? currentBase(e.time) : lastValue_;
                lastTime_ = e.time;
            }
            lastType_ = e.type;
            lastTarget_ = e;
            ++current_;
        }
        // Next event is a ramp in progress?
        if (current_ < count_) {
            const Event& n = events_[size_t(current_)];
            if (n.type == Type::Linear) {
                const double t0 = lastTime_, v0 = currentBase(t);
                const double r = (t - t0) / (n.time - t0);
                value_ = float(v0 + (n.value - v0) * r);
                return clamp(value_);
            }
            if (n.type == Type::Exponential) {
                const double t0 = lastTime_, v0 = currentBase(t);
                // spec: v0 == 0 or opposite signs -> hold v0 until the event
                if (v0 == 0.0 || (v0 > 0.0) != (n.value > 0.0)) value_ = float(v0);
                else value_ = float(v0 * std::pow(n.value / v0, (t - t0) / (n.time - t0)));
                return clamp(value_);
            }
        }
        value_ = float(currentBase(t));
        return clamp(value_);
    }

    double defaultValue() const { return default_; }

private:
    enum class Type { Set, Linear, Exponential, Target };
    struct Event {
        Type type;
        float value;
        double time;
        double timeConstant;
    };

    // Value of the last event at time t (a running setTarget keeps moving).
    double currentBase(double t)
    {
        if (lastType_ == Type::Target && current_ > 0) {
            const Event& e = lastTarget_;
            // v(t) = V1 + (V0 - V1) * exp(-(t - T0) / tau); V0 = value when it started
            const double v = e.value + (targetStart_ - e.value) * std::exp(-(t - e.time) / std::max(1e-9, e.timeConstant));
            lastValue_ = float(v);
            return v;
        }
        return lastValue_;
    }

    void insert(const Event& e)
    {
        if (count_ >= int(events_.size())) return; // full: ignored (superdough uses only a few)
        // keep events sorted by time; equal times keep insertion order
        int i = count_;
        while (i > 0 && events_[size_t(i - 1)].time > e.time) {
            events_[size_t(i)] = events_[size_t(i - 1)];
            --i;
        }
        events_[size_t(i)] = e;
        ++count_;
    }

    float clamp(float v) const { return v < min_ ? min_ : (v > max_ ? max_ : v); }

    float default_, min_, max_;
    float value_;
    std::array<Event, 24> events_{};
    int count_ = 0;
    int current_ = 0;
    float lastValue_ = 0.f;
    double lastTime_ = 0.0;
    Type lastType_ = Type::Set;
    Event lastTarget_{};
    double targetStart_ = 0.0;
};

} // namespace concha::sd
