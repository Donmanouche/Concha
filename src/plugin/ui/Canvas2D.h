#pragma once

// A canvas of Strudel's visuals in the window: replays the Canvas 2D calls a frame recorded in JS
// (js/src/canvas.mjs, opcodes below) on an image that persists between frames, as a browser
// canvas does. Arcs and ellipses are drawn as short segments, after the current transform.

#include "pattern/PatternEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <vector>

namespace concha::ui {

class Canvas2D {
public:
    enum Op {
        FillStyle = 1, StrokeStyle, LineWidth, GlobalAlpha, Font, TextAlign, TextBaseline, LineCap, LineJoin, ClearRect,
        FillRect, StrokeRect, BeginPath, ClosePath, MoveTo, LineTo, Arc, Ellipse, Rect, QuadraticCurveTo, BezierCurveTo,
        Fill, Stroke, FillText, StrokeText, Save, Restore, Translate, Rotate, Scale, SetTransform, ResetTransform,
        Snapshot, PutImage, ArcTo, Composite, Resize
    };

    void run(const CanvasFrame& frame);
    const juce::Image& image() const { return image_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool drawn() const { return drawn_; } // anything drawn since the canvas was made or resized

private:
    struct State {
        juce::Colour fill{juce::Colours::black}, stroke{juce::Colours::black};
        float lineWidth = 1, alpha = 1;
        float fontSize = 10;
        bool bold = false, italic = false, mono = false;
        juce::String align = "start", baseline = "alphabetic";
        juce::PathStrokeType::EndCapStyle cap = juce::PathStrokeType::butt;
        juce::PathStrokeType::JointStyle join = juce::PathStrokeType::mitered;
        bool erase = false; // globalCompositeOperation destination-out
        juce::AffineTransform transform;
    };

    void resize(int w, int h);
    juce::Point<float> map(double x, double y) const;
    void arcPoints(double cx, double cy, double rx, double ry, double rotation, double a0, double a1, bool ccw);
    void lineOrMove(juce::Point<float> p);
    void fillPath(juce::Graphics& g, const juce::Path& p);
    void strokePath(juce::Graphics& g, const juce::Path& p);
    void text(juce::Graphics& g, const juce::String& s, double x, double y, bool stroke);
    juce::Font font() const;

    juce::Image image_;
    int width_ = 0, height_ = 0;
    bool drawn_ = false;
    State st_;
    std::vector<State> stack_;
    juce::Path path_;
    bool hasPoint_ = false;
    std::map<int, juce::Image> snapshots_;
};

} // namespace concha::ui
