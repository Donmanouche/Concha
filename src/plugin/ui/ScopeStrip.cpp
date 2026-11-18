#include "plugin/ui/ScopeStrip.h"

#include "plugin/ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace concha::ui {

namespace {
const juce::Colour kPanel(0xff151715), kScreen(0xff0a0f0b), kBezel(0xff2c332d), kGrid(0xff2f4a37), kAxis(0xff3f6a4c),
    kText(0xffd8d8d8), kDim(0xff8a9199), kTitle(0xff8fbf9c), kBox(0xff202320);
// the channel colours of multi-channel oscilloscopes
const juce::Colour kChannel[] = {juce::Colour(0xffC9E66A), juce::Colour(0xff5DD9E8), juce::Colour(0xffE98BD8),
                                 juce::Colour(0xff7CFC9A), juce::Colour(0xffF5A65B), juce::Colour(0xff9DB4FF)};
constexpr double kHalfSpan = 1.25; // cycles each side of the trigger: 10 divisions of 1/4 cycle
constexpr float kReadoutHeight = 30, kPad = 10, kSide = 180;

juce::String noteName(int midi)
{
    static const char* names[] = {"c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b"};
    return juce::String(names[((midi % 12) + 12) % 12]) + juce::String(midi / 12 - 1);
}

float drumHeight(const std::string& s)
{
    auto has = [&](std::initializer_list<const char*> keys) {
        for (const char* k : keys)
            if (s.find(k) != std::string::npos) return true;
        return false;
    };
    if (has({"bd", "kick"})) return 1.0f;
    if (has({"sd", "snare", "cp", "clap", "rim"})) return 0.72f;
    if (has({"hh", "oh", "ch", "hat", "cr", "rd", "ride", "cymb", "shaker"})) return 0.38f;
    return 0.6f;
}
} // namespace

ScopeStrip::ScopeStrip()
{
    setOpaque(true);
    setBufferedToImage(false);
}

ScopeStrip::Layout ScopeStrip::layoutFor(const Model& m) const
{
    Layout l;
    auto r = getLocalBounds().toFloat().reduced(kPad);
    l.readouts = r.removeFromBottom(kReadoutHeight);
    r.removeFromBottom(8);
    const bool side = m.channels && (m.scope || m.fft);
    auto right = side ? r.removeFromRight(kSide) : (m.channels ? juce::Rectangle<float>() : r);
    if (side) r.removeFromRight(kPad);
    if (m.channels) l.main = r;
    if (m.scope && m.fft) {
        if (side) {
            l.scope = right.removeFromTop((right.getHeight() - kPad) / 2);
            right.removeFromTop(kPad);
            l.fft = right;
        } else {
            l.scope = right.removeFromLeft((right.getWidth() - kPad) / 2);
            right.removeFromLeft(kPad);
            l.fft = right;
        }
    } else if (m.scope)
        l.scope = right;
    else if (m.fft)
        l.fft = right;
    return l;
}

void ScopeStrip::line(Frame& v, juce::Point<float> a, juce::Point<float> b, float width, juce::Colour c, float halo) const
{
    v.segs.push_back({a, b, width, c, halo});
}

void ScopeStrip::screen(Frame& v, juce::Rectangle<float> r, int divX, int divY) const
{
    v.screens.push_back({r, divX, divY});
}

void ScopeStrip::update(const Model& m)
{
    Frame v;
    v.segs.reserve(2048);
    const auto l = layoutFor(m);
    std::vector<Readout> readouts;
    std::vector<std::pair<juce::Rectangle<float>, juce::String>> labels;
    std::vector<std::pair<juce::Rectangle<float>, juce::Colour>> labelColours;

    // ---- the main screen: the channels ----
    if (!l.main.isEmpty()) {
        screen(v, l.main, 10, 8);
        const auto in = l.main.reduced(1);
        // the channels: the blocks with haps, in code order
        std::map<int, std::vector<const Hap*>> byBlock;
        for (const auto& h : m.haps) byBlock[h.block].push_back(&h);
        const int n = std::max(1, int(byBlock.size()));
        const float band = in.getHeight() / float(n);
        const double t0 = m.cycle - kHalfSpan;
        auto xOf = [&](double t) { // clipped to the screen
            return std::clamp(in.getX() + float((t - t0) / (2 * kHalfSpan)) * in.getWidth(), in.getX() + 1, in.getRight() - 1);
        };
        auto visible = [&](const Hap& h) { return h.end > t0 && h.begin < t0 + 2 * kHalfSpan; };
        int ch = 0;
        for (auto& [block, haps] : byBlock) {
            const juce::Colour c = kChannel[ch % 6];
            const float top = in.getY() + band * float(ch), bottom = top + band;
            int pitched = 0;
            float lo = 1e9f, hi = -1e9f;
            for (const Hap* h : haps)
                if (!std::isnan(h->note)) {
                    ++pitched;
                    lo = std::min(lo, h->note);
                    hi = std::max(hi, h->note);
                }
            // brightness: what has played fades (afterglow), the rest is full
            auto shade = [&](const Hap& h) {
                if (h.begin <= m.cycle && m.cycle < h.end) return c;
                if (h.end <= m.cycle) return c.withMultipliedAlpha(float(0.5 * (1.0 - 0.6 * (m.cycle - h.end) / kHalfSpan)));
                return c.withMultipliedAlpha(0.9f);
            };
            juce::String info;
            if (pitched * 2 < int(haps.size())) { // drums: pulses
                const float base = bottom - band * 0.18f;
                int hits = 0;
                for (const Hap* h : haps) {
                    if (h->begin >= std::floor(m.cycle) && h->begin < std::floor(m.cycle) + 1) ++hits;
                    if (h->begin < t0 || h->begin > t0 + 2 * kHalfSpan) continue;
                    const float x = xOf(h->begin);
                    const float amp = band * 0.66f * drumHeight(h->s) * std::clamp(h->gain, 0.1f, 1.5f);
                    const auto col = shade(*h);
                    const float decay = std::min({26.0f, float((h->end - h->begin) / (2 * kHalfSpan)) * in.getWidth(), in.getRight() - 2 - x});
                    line(v, {x, base}, {x + 1, base - amp}, 1.4f, col, 2.5f);
                    juce::Point<float> prev{x + 1, base - amp};
                    for (int k = 1; k <= 6; ++k) { // the decay back to the baseline
                        const float f = float(k) / 6;
                        const juce::Point<float> p{x + 1 + decay * f, base - amp * std::exp(-4.0f * f)};
                        line(v, prev, p, 1.4f, col, 2.5f);
                        prev = p;
                    }
                }
                info = juce::String(hits) + " hits/cy";
            } else { // melodies and chords: a level per note
                if (hi - lo < 4) {
                    const float mid = (hi + lo) / 2;
                    lo = mid - 2;
                    hi = mid + 2;
                }
                auto yOf = [&](float note) { return bottom - band * 0.15f - (note - lo) / (hi - lo) * band * 0.7f; };
                int voices = 0;
                for (const Hap* h : haps) {
                    int k = 0;
                    for (const Hap* o : haps)
                        if (o->begin < h->end - 1e-9 && h->begin < o->end - 1e-9) ++k;
                    voices = std::max(voices, k);
                }
                const Hap* prev = nullptr;
                std::vector<const Hap*> sorted(haps.begin(), haps.end());
                std::sort(sorted.begin(), sorted.end(), [](const Hap* a, const Hap* b) { return a->begin < b->begin; });
                for (const Hap* h : sorted) {
                    if (std::isnan(h->note) || !visible(*h)) continue;
                    const float y = yOf(h->note);
                    const auto col = shade(*h);
                    const bool active = h->begin <= m.cycle && m.cycle < h->end;
                    line(v, {xOf(h->begin), y}, {xOf(h->end), y}, active ? 2.0f : 1.4f, col, 2.5f);
                    // a melody: the step joins the previous note when it follows it
                    if (voices <= 1 && prev && std::abs(prev->end - h->begin) < 1e-6)
                        line(v, {xOf(h->begin), yOf(prev->note)}, {xOf(h->begin), y}, 1.4f, col, 2.5f);
                    prev = h;
                }
                if (voices > 1) info = juce::String(voices) + " voices";
                else if (pitched) {
                    float rlo = 1e9f, rhi = -1e9f;
                    for (const Hap* h : haps)
                        if (!std::isnan(h->note)) rlo = std::min(rlo, h->note), rhi = std::max(rhi, h->note);
                    info = noteName(int(std::lround(rlo))) + " to " + noteName(int(std::lround(rhi)));
                }
            }
            const juce::String name = block < int(m.blockNames.size()) ? juce::String(m.blockNames[size_t(block)]) : juce::String(block + 1);
            readouts.push_back({"CH" + juce::String(ch + 1) + " " + name, info, c});
            labels.push_back({{in.getX() + 6, top + 4, 20, 14}, juce::String(ch + 1)});
            labelColours.push_back({{}, c});
            ++ch;
        }
        // the trigger mark (now)
        const float cx = in.getCentreX();
        line(v, {cx - 5, in.getY() + 1}, {cx + 5, in.getY() + 1}, 2, juce::Colours::white.withAlpha(0.8f), 0);
        line(v, {cx - 3, in.getY() + 3.5f}, {cx + 3, in.getY() + 3.5f}, 2, juce::Colours::white.withAlpha(0.8f), 0);
        line(v, {cx - 1, in.getY() + 6}, {cx + 1, in.getY() + 6}, 2, juce::Colours::white.withAlpha(0.8f), 0);
        labels.push_back({{in.getRight() - 40, in.getY() + 4, 36, 14}, "Run"});
        labelColours.push_back({{}, kTitle});
    }

    // ---- SCOPE: the analysed sound from a rising zero crossing ----
    juce::String scopeInfo;
    if (!l.scope.isEmpty()) {
        screen(v, l.scope, 5, 4);
        const auto in = l.scope.reduced(1);
        const auto& d = m.scopeData;
        const int span = std::min<int>(512, int(d.size()) / 2);
        if (span > 8) {
            int start = 0;
            for (int i = 1; i < int(d.size()) - span; ++i)
                if (d[size_t(i - 1)] < 0 && d[size_t(i)] >= 0) {
                    start = i;
                    break;
                }
            std::vector<float> trace(d.begin() + start, d.begin() + start + span);
            auto draw = [&](const std::vector<float>& t, juce::Colour c, float w) {
                juce::Point<float> prev;
                for (int i = 0; i < int(t.size()); ++i) {
                    const juce::Point<float> p{in.getX() + in.getWidth() * float(i) / float(t.size() - 1),
                                               in.getCentreY() - std::clamp(t[size_t(i)], -1.0f, 1.0f) * in.getHeight() * 0.45f};
                    if (i) line(v, prev, p, w, c, w > 1 ? 2.5f : 0);
                    prev = p;
                }
            };
            if (lastScope_.size() == trace.size()) draw(lastScope_, kChannel[0].withAlpha(0.3f), 0.9f);
            draw(trace, kChannel[0], 1.4f);
            lastScope_ = trace;
            const double msPerDiv = span / m.sampleRate * 1000.0 / 5.0;
            scopeInfo = juce::String(msPerDiv, msPerDiv < 1 ? 2 : 1) + " ms/div";
        }
        labels.push_back({{in.getX() + 6, in.getY() + 4, 140, 14}, "SCOPE"});
        labelColours.push_back({{}, kTitle});
        labels.push_back({{in.getRight() - 84, in.getBottom() - 16, 80, 14}, scopeInfo});
        labelColours.push_back({{}, kDim});
    }

    // ---- FFT: the spectrum on a log frequency axis, -100..0 dB ----
    juce::String peakInfo, f0Info;
    if (!l.fft.isEmpty()) {
        screen(v, l.fft, 5, 4);
        const auto in = l.fft.reduced(1);
        const auto& db = m.fftDb;
        const int bins = int(db.size());
        if (bins > 8) {
            const double binHz = m.sampleRate / 2 / bins;
            int peak = 1;
            for (int k = 1; k < bins; ++k)
                if (db[size_t(k)] > db[size_t(peak)]) peak = k;
            juce::Point<float> prev;
            bool started = false;
            for (int px = 0; px <= 120; ++px) { // 120 points along a log axis 20 Hz - 20 kHz, each the loudest bin of its band
                const double f0 = 20.0 * std::pow(1000.0, (px - 0.5) / 120.0), f1 = 20.0 * std::pow(1000.0, (px + 0.5) / 120.0);
                const int k0 = std::clamp(int(f0 / binHz), 1, bins - 1), k1 = std::clamp(int(f1 / binHz), k0, bins - 1);
                float best = -1000.0f;
                for (int k = k0; k <= k1; ++k) best = std::max(best, db[size_t(k)]);
                const float level = std::clamp((best + 100.0f) / 100.0f, 0.0f, 1.0f);
                const juce::Point<float> p{in.getX() + in.getWidth() * float(px) / 120.0f, in.getBottom() - level * in.getHeight() * 0.92f};
                if (started) line(v, prev, p, 1.4f, kChannel[1], 2.5f);
                prev = p;
                started = true;
            }
            peakInfo = "peak " + juce::String(db[size_t(peak)], 1) + " dB";
            f0Info = "f0 " + juce::String(peak * binHz, 1) + " Hz";
        }
        labels.push_back({{in.getX() + 6, in.getY() + 4, 140, 14}, "FFT log"});
        labelColours.push_back({{}, kTitle});
        labels.push_back({{in.getRight() - 84, in.getBottom() - 16, 80, 14}, "20 Hz-20k"});
        labelColours.push_back({{}, kDim});
    }
    if (!l.main.isEmpty()) readouts.push_back({"H 1/4 cycle/div", "trig now", kText});
    if (!l.scope.isEmpty() && l.main.isEmpty()) readouts.push_back({"SCOPE", scopeInfo, kChannel[0]});
    if (!l.fft.isEmpty()) readouts.push_back({peakInfo.isEmpty() ? "FFT" : peakInfo, f0Info, kChannel[1]});

    for (size_t i = 0; i < labels.size(); ++i) {
        v.labels.push_back(labels[i]);
        v.labelColours.push_back(i < labelColours.size() ? labelColours[i].second : kDim);
    }
    {
        auto r = l.readouts;
        const float w = readouts.empty() ? 0 : std::min(150.0f, (r.getWidth() - 6.0f * float(readouts.size() - 1)) / float(readouts.size()));
        for (const auto& ro : readouts) {
            const auto box = r.removeFromLeft(w);
            r.removeFromLeft(6);
            v.boxes.push_back({box, {ro.title, ro.info}});
            v.boxColours.push_back(ro.colour);
        }
    }
    frame_ = std::move(v);
    repaint();
}


void ScopeStrip::resized()
{
    lastScope_.clear();
    staticKey_.clear(); // the screens are drawn again at the new size
}

// the panel and its screens (bezel, glass, graticule): drawn once per layout, kept in an image
void ScopeStrip::paintStatic(juce::Graphics& g, const std::vector<Screen>& screens)
{
    g.fillAll(kPanel);
    for (const auto& s : screens) {
        g.setColour(kBezel);
        g.fillRoundedRectangle(s.r, 3);
        const auto in = s.r.reduced(1);
        g.setColour(kScreen);
        g.fillRect(in);
        for (int i = 1; i < s.divX; ++i) {
            const float x = in.getX() + in.getWidth() * float(i) / float(s.divX);
            g.setColour(i == s.divX / 2 ? kAxis : kGrid);
            g.drawLine(x, in.getY(), x, in.getBottom(), 0.6f);
        }
        for (int j = 1; j < s.divY; ++j) {
            const float y = in.getY() + in.getHeight() * float(j) / float(s.divY);
            g.setColour(j == s.divY / 2 ? kAxis : kGrid);
            g.drawLine(in.getX(), y, in.getRight(), y, 0.6f);
        }
        g.setColour(kAxis);
        const float cx = in.getCentreX(), cy = in.getCentreY();
        for (int i = 0; i < s.divX * 5; ++i) {
            const float x = in.getX() + in.getWidth() * float(i) / float(s.divX * 5);
            g.drawLine(x, cy - 2.5f, x, cy + 2.5f, 0.7f);
        }
        for (int j = 0; j < s.divY * 5; ++j) {
            const float y = in.getY() + in.getHeight() * float(j) / float(s.divY * 5);
            g.drawLine(cx - 2.5f, y, cx + 2.5f, y, 0.7f);
        }
    }
}

void ScopeStrip::paint(juce::Graphics& g)
{
    // the static layer, cached
    juce::String key = juce::String(getWidth()) + "x" + juce::String(getHeight());
    for (const auto& s : frame_.screens) key << ";" << s.r.toString();
    if (key != staticKey_ || !staticImage_.isValid()) {
        staticImage_ = juce::Image(juce::Image::RGB, std::max(1, getWidth()), std::max(1, getHeight()), true);
        juce::Graphics sg(staticImage_);
        paintStatic(sg, frame_.screens);
        staticKey_ = key;
    }
    g.drawImageAt(staticImage_, 0, 0);
    // the traces, batched by colour and width: a faint halo (phosphor), then the line
    std::map<std::tuple<juce::uint32, int, int>, juce::Path> batches;
    for (const auto& s : frame_.segs) {
        auto& p = batches[{s.colour.getARGB(), int(s.width * 10), int(s.halo * 10)}];
        p.startNewSubPath(s.a);
        p.lineTo(s.b);
    }
    for (const auto& [k, p] : batches) {
        const juce::Colour c(std::get<0>(k));
        const float w = float(std::get<1>(k)) / 10, halo = float(std::get<2>(k)) / 10;
        if (halo > 0) {
            g.setColour(c.withMultipliedAlpha(0.16f));
            g.strokePath(p, juce::PathStrokeType(w + 2 * halo, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.setColour(c);
        g.strokePath(p, juce::PathStrokeType(w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    // texts and readouts
    g.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain)));
    for (size_t i = 0; i < frame_.labels.size(); ++i) {
        g.setColour(frame_.labelColours[i]);
        g.drawText(frame_.labels[i].second, frame_.labels[i].first, juce::Justification::topLeft, false);
    }
    for (size_t i = 0; i < frame_.boxes.size(); ++i) {
        const auto box = frame_.boxes[i].first;
        g.setColour(kBox);
        g.fillRoundedRectangle(box, 4);
        g.setColour(frame_.boxColours[i].withAlpha(0.6f));
        g.drawRoundedRectangle(box.reduced(0.5f), 4, 1);
        const auto in = box.reduced(8, 2);
        g.setColour(frame_.boxColours[i]);
        g.drawText(frame_.boxes[i].second.first, in.withHeight(14), juce::Justification::centredLeft, true);
        g.setColour(kDim);
        g.drawText(frame_.boxes[i].second.second, in.withTrimmedTop(in.getHeight() - 13), juce::Justification::centredLeft, true);
    }
}

} // namespace concha::ui
