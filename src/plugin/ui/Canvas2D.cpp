#include "plugin/ui/Canvas2D.h"

#include "plugin/ui/Theme.h"

#include <cmath>

namespace concha::ui {

namespace {
constexpr double kTwoPi = 6.283185307179586;
}

void Canvas2D::resize(int w, int h)
{
    w = juce::jlimit(1, 8192, w);
    h = juce::jlimit(1, 8192, h);
    width_ = w;
    height_ = h;
    image_ = juce::Image(juce::Image::ARGB, w, h, true); // a resized canvas is cleared
    st_ = {};
    stack_.clear();
    path_.clear();
    hasPoint_ = false;
    drawn_ = false;
}

juce::Point<float> Canvas2D::map(double x, double y) const
{
    float fx = float(x), fy = float(y);
    st_.transform.transformPoint(fx, fy);
    return {fx, fy};
}

void Canvas2D::lineOrMove(juce::Point<float> p)
{
    if (hasPoint_) path_.lineTo(p);
    else path_.startNewSubPath(p);
    hasPoint_ = true;
}

// Canvas angles: 0 = +x, increasing clockwise on screen (y down); the arc joins the current point
void Canvas2D::arcPoints(double cx, double cy, double rx, double ry, double rotation, double a0, double a1, bool ccw)
{
    double sweep;
    if (!ccw) {
        sweep = a1 - a0;
        if (sweep >= kTwoPi) sweep = kTwoPi;
        else if (sweep < 0) sweep = std::fmod(std::fmod(sweep, kTwoPi) + kTwoPi, kTwoPi);
    } else {
        sweep = a1 - a0;
        if (sweep <= -kTwoPi) sweep = -kTwoPi;
        else if (sweep > 0) sweep = -std::fmod(std::fmod(-sweep, kTwoPi) + kTwoPi, kTwoPi);
    }
    const float scale = std::sqrt(std::abs(st_.transform.getDeterminant()));
    const int steps = juce::jlimit(4, 720, int(std::abs(sweep) * std::max(rx, ry) * scale / 3) + 4);
    const double cr = std::cos(rotation), sr = std::sin(rotation);
    for (int i = 0; i <= steps; ++i) {
        const double a = a0 + sweep * i / steps;
        const double px = rx * std::cos(a), py = ry * std::sin(a);
        lineOrMove(map(cx + px * cr - py * sr, cy + px * sr + py * cr));
    }
}

juce::Font Canvas2D::font() const
{
    juce::FontOptions o = juce::FontOptions(st_.fontSize);
    if (st_.mono) o = o.withName(juce::Font::getDefaultMonospacedFontName());
    int style = juce::Font::plain;
    if (st_.bold) style |= juce::Font::bold;
    if (st_.italic) style |= juce::Font::italic;
    return juce::Font(o.withStyle(style == juce::Font::bold ? "Bold" : style == juce::Font::italic ? "Italic" : "Regular"));
}

void Canvas2D::fillPath(juce::Graphics& g, const juce::Path& p)
{
    if (st_.erase) { // destination-out: the path's coverage removes the pixels it covers
        juce::Image mask(juce::Image::SingleChannel, width_, height_, true);
        {
            juce::Graphics mg(mask);
            mg.setColour(juce::Colours::white.withAlpha(st_.fill.getFloatAlpha() * st_.alpha));
            mg.fillPath(p);
        }
        const juce::Image::BitmapData m(mask, juce::Image::BitmapData::readOnly);
        juce::Image::BitmapData d(image_, juce::Image::BitmapData::readWrite);
        for (int y = 0; y < height_; ++y)
            for (int x = 0; x < width_; ++x) {
                const float a = m.getPixelColour(x, y).getFloatAlpha();
                if (a > 0) d.setPixelColour(x, y, d.getPixelColour(x, y).withMultipliedAlpha(1.0f - a));
            }
        return;
    }
    g.setColour(st_.fill.withMultipliedAlpha(st_.alpha));
    g.fillPath(p);
    drawn_ = true;
}

void Canvas2D::strokePath(juce::Graphics& g, const juce::Path& p)
{
    const float scale = std::sqrt(std::abs(st_.transform.getDeterminant()));
    g.setColour(st_.stroke.withMultipliedAlpha(st_.alpha));
    g.strokePath(p, juce::PathStrokeType(std::max(0.01f, st_.lineWidth * scale), st_.join, st_.cap));
    drawn_ = true;
}

void Canvas2D::text(juce::Graphics& g, const juce::String& s, double x, double y, bool stroke)
{
    const juce::Font f = font();
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText(f, s, 0, 0);
    const float w = glyphs.getBoundingBox(0, -1, true).getWidth();
    float dx = 0;
    if (st_.align == "center") dx = -w / 2;
    else if (st_.align == "right" || st_.align == "end") dx = -w;
    float dy = 0; // alphabetic: y is the baseline
    if (st_.baseline == "top" || st_.baseline == "hanging") dy = f.getAscent();
    else if (st_.baseline == "middle") dy = (f.getAscent() - f.getDescent()) / 2;
    else if (st_.baseline == "bottom" || st_.baseline == "ideographic") dy = -f.getDescent();
    juce::Path p;
    glyphs.createPath(p);
    p.applyTransform(juce::AffineTransform::translation(float(x) + dx, float(y) + dy).followedBy(st_.transform));
    stroke ? strokePath(g, p) : fillPath(g, p);
}

void Canvas2D::run(const CanvasFrame& f)
{
    if (!image_.isValid() || f.width != width_ || f.height != height_) resize(f.width, f.height);
    juce::Graphics g(image_);
    const auto& n = f.nums;
    auto str = [&](double i) -> juce::String {
        const size_t k = size_t(i);
        return k < f.strs.size() ? juce::String::fromUTF8(f.strs[k].c_str()) : juce::String();
    };
    auto rectPath = [&](double x, double y, double w, double h) {
        juce::Path p;
        p.startNewSubPath(map(x, y));
        p.lineTo(map(x + w, y));
        p.lineTo(map(x + w, y + h));
        p.lineTo(map(x, y + h));
        p.closeSubPath();
        return p;
    };
    size_t i = 0;
    auto arg = [&](int k) { return i + size_t(k) < n.size() ? n[i + size_t(k)] : 0.0; };
    while (i < n.size()) {
        const int op = int(n[i++]);
        int used = 0;
        switch (op) {
        case FillStyle: st_.fill = cssColour(str(arg(0)), st_.fill); used = 1; break;
        case StrokeStyle: st_.stroke = cssColour(str(arg(0)), st_.stroke); used = 1; break;
        case LineWidth: st_.lineWidth = float(arg(0)); used = 1; break;
        case GlobalAlpha: st_.alpha = float(juce::jlimit(0.0, 1.0, arg(0))); used = 1; break;
        case Font: {
            const juce::String css = str(arg(0)).toLowerCase();
            const int px = css.indexOf("px");
            if (px > 0) {
                int s = px - 1;
                while (s > 0 && (juce::CharacterFunctions::isDigit(css[s - 1]) || css[s - 1] == '.')) --s;
                st_.fontSize = juce::jmax(1.0f, css.substring(s, px).getFloatValue());
            }
            st_.bold = css.contains("bold");
            st_.italic = css.contains("italic");
            st_.mono = css.contains("mono") || css.contains("courier");
            used = 1;
            break;
        }
        case TextAlign: st_.align = str(arg(0)); used = 1; break;
        case TextBaseline: st_.baseline = str(arg(0)); used = 1; break;
        case LineCap: {
            const auto s = str(arg(0));
            st_.cap = s == "round" ? juce::PathStrokeType::rounded : s == "square" ? juce::PathStrokeType::square : juce::PathStrokeType::butt;
            used = 1;
            break;
        }
        case LineJoin: {
            const auto s = str(arg(0));
            st_.join = s == "round" ? juce::PathStrokeType::curved : s == "bevel" ? juce::PathStrokeType::beveled : juce::PathStrokeType::mitered;
            used = 1;
            break;
        }
        case Composite: st_.erase = str(arg(0)) == "destination-out"; used = 1; break;
        case ClearRect: {
            const auto r = rectPath(arg(0), arg(1), arg(2), arg(3)).getBounds().getSmallestIntegerContainer();
            image_.clear(r.getIntersection(image_.getBounds()));
            used = 4;
            break;
        }
        case FillRect: fillPath(g, rectPath(arg(0), arg(1), arg(2), arg(3))); used = 4; break;
        case StrokeRect: strokePath(g, rectPath(arg(0), arg(1), arg(2), arg(3))); used = 4; break;
        case BeginPath: path_.clear(); hasPoint_ = false; break;
        case ClosePath: if (hasPoint_) path_.closeSubPath(); hasPoint_ = false; break;
        case MoveTo: path_.startNewSubPath(map(arg(0), arg(1))); hasPoint_ = true; used = 2; break;
        case LineTo: lineOrMove(map(arg(0), arg(1))); used = 2; break;
        case Arc: arcPoints(arg(0), arg(1), arg(2), arg(2), 0, arg(3), arg(4), arg(5) != 0); used = 6; break;
        case Ellipse: arcPoints(arg(0), arg(1), arg(2), arg(3), arg(4), arg(5), arg(6), arg(7) != 0); used = 8; break;
        case Rect: {
            path_.addPath(rectPath(arg(0), arg(1), arg(2), arg(3)));
            path_.startNewSubPath(map(arg(0), arg(1)));
            hasPoint_ = true;
            used = 4;
            break;
        }
        case QuadraticCurveTo: {
            if (!hasPoint_) lineOrMove(map(arg(0), arg(1)));
            path_.quadraticTo(map(arg(0), arg(1)), map(arg(2), arg(3)));
            used = 4;
            break;
        }
        case BezierCurveTo: {
            if (!hasPoint_) lineOrMove(map(arg(0), arg(1)));
            path_.cubicTo(map(arg(0), arg(1)), map(arg(2), arg(3)), map(arg(4), arg(5)));
            used = 6;
            break;
        }
        case ArcTo: lineOrMove(map(arg(0), arg(1))); used = 5; break; // approximated by its corner
        case Fill: fillPath(g, path_); break;
        case Stroke: strokePath(g, path_); break;
        case FillText: text(g, str(arg(0)), arg(1), arg(2), false); used = 3; break;
        case StrokeText: text(g, str(arg(0)), arg(1), arg(2), true); used = 3; break;
        case Save: stack_.push_back(st_); break;
        case Restore:
            if (!stack_.empty()) {
                st_ = stack_.back();
                stack_.pop_back();
            }
            break;
        case Translate: st_.transform = juce::AffineTransform::translation(float(arg(0)), float(arg(1))).followedBy(st_.transform); used = 2; break;
        case Rotate: st_.transform = juce::AffineTransform::rotation(float(arg(0))).followedBy(st_.transform); used = 1; break;
        case Scale: st_.transform = juce::AffineTransform::scale(float(arg(0)), float(arg(1))).followedBy(st_.transform); used = 2; break;
        case SetTransform:
            st_.transform = juce::AffineTransform(float(arg(0)), float(arg(2)), float(arg(4)), float(arg(1)), float(arg(3)), float(arg(5)));
            used = 6;
            break;
        case ResetTransform: st_.transform = {}; break;
        case Snapshot: { // getImageData(x, y, w, h): a copy, kept for putImageData
            const juce::Rectangle<int> r(int(arg(1)), int(arg(2)), int(arg(3)), int(arg(4)));
            snapshots_[int(arg(0))] = image_.getClippedImage(r.getIntersection(image_.getBounds())).createCopy();
            while (snapshots_.size() > 4) snapshots_.erase(snapshots_.begin());
            used = 5;
            break;
        }
        case PutImage: { // putImageData replaces the pixels (no blending)
            auto it = snapshots_.find(int(arg(0)));
            if (it != snapshots_.end()) {
                const juce::Rectangle<int> dst(int(arg(1)), int(arg(2)), it->second.getWidth(), it->second.getHeight());
                image_.clear(dst.getIntersection(image_.getBounds()));
                g.drawImageAt(it->second, dst.getX(), dst.getY());
                drawn_ = true;
            }
            used = 3;
            break;
        }
        case Resize: resize(int(arg(0)), int(arg(1))); used = 2; return run({f.id, f.widget, width_, height_, std::vector<double>(n.begin() + long(i + 2), n.end()), f.strs});
        default: return; // unknown: the rest of the frame cannot be read
        }
        i += size_t(used);
    }
}

} // namespace concha::ui
