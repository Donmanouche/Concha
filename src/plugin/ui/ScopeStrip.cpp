#include "plugin/ui/ScopeStrip.h"

#include "plugin/ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace concha::ui {

namespace {
const juce::Colour kPanel(0xff151715), kScreen(0xff0a0f0b), kBezel(0xff2c332d), kGrid(0xff2f4a37), kAxis(0xff3f6a4c);
// the channel colours of multi-channel oscilloscopes
const juce::Colour kChannel[] = {juce::Colour(0xffC9E66A), juce::Colour(0xff5DD9E8), juce::Colour(0xffE98BD8),
                                 juce::Colour(0xff7CFC9A), juce::Colour(0xffF5A65B), juce::Colour(0xff9DB4FF)};
constexpr double kHalfSpan = 1.25; // cycles each side of the trigger: 10 divisions of 1/4 cycle
constexpr float kPad = 10;

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
    // one screen per kind of visual asked for, side by side, all the same width
    Layout l;
    auto r = getLocalBounds().toFloat().reduced(kPad);
    const int n = int(m.channels) + int(m.scope) + int(m.fft);
    if (n == 0) return l;
    const float w = (r.getWidth() - kPad * float(n - 1)) / float(n);
    auto next = [&] {
        const auto s = r.removeFromLeft(w);
        r.removeFromLeft(kPad);
        return s;
    };
    if (m.channels) l.main = next();
    if (m.scope) l.scope = next();
    if (m.fft) l.fft = next();
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
            if (pitched * 2 < int(haps.size())) { // drums: pulses
                const float base = bottom - band * 0.18f;
                for (const Hap* h : haps) {
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
            }
            ++ch;
        }
        // the trigger mark (now)
        const float cx = in.getCentreX();
        line(v, {cx - 5, in.getY() + 1}, {cx + 5, in.getY() + 1}, 2, juce::Colours::white.withAlpha(0.8f), 0);
        line(v, {cx - 3, in.getY() + 3.5f}, {cx + 3, in.getY() + 3.5f}, 2, juce::Colours::white.withAlpha(0.8f), 0);
        line(v, {cx - 1, in.getY() + 6}, {cx + 1, in.getY() + 6}, 2, juce::Colours::white.withAlpha(0.8f), 0);
    }

    // ---- SCOPE: the analysed sound from a rising zero crossing ----
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
            // about one point per 1.5 pixels: each point the sample of largest magnitude of its slice
            const int points = std::clamp(int(in.getWidth() / 1.5f), 16, span);
            std::vector<float> trace(static_cast<size_t>(points));
            for (int p = 0; p < points; ++p) {
                const int a = start + p * span / points, b = std::max(a + 1, start + (p + 1) * span / points);
                float best = d[size_t(a)];
                for (int i = a + 1; i < b; ++i)
                    if (std::abs(d[size_t(i)]) > std::abs(best)) best = d[size_t(i)];
                trace[size_t(p)] = best;
            }
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
        }
    }

    // ---- FFT: the spectrum on a log frequency axis, -100..0 dB ----
    if (!l.fft.isEmpty()) {
        screen(v, l.fft, 5, 4);
        const auto in = l.fft.reduced(1);
        const auto& db = m.fftDb;
        const int bins = int(db.size());
        if (bins > 8) {
            const double binHz = m.sampleRate / 2 / bins;
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
    // the traces, batched by colour and width (no halo: it doubled the cost of painting)
    std::map<std::tuple<juce::uint32, int, int>, juce::Path> batches;
    for (const auto& s : frame_.segs) {
        auto& p = batches[{s.colour.getARGB(), int(s.width * 10), 0}];
        p.startNewSubPath(s.a);
        p.lineTo(s.b);
    }
    for (const auto& [k, p] : batches) {
        g.setColour(juce::Colour(std::get<0>(k)));
        g.strokePath(p, juce::PathStrokeType(float(std::get<1>(k)) / 10, juce::PathStrokeType::mitered, juce::PathStrokeType::square));
    }
}

} // namespace concha::ui
