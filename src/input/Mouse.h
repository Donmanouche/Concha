#pragma once

// Strudel's mousex / mousey (core/signal.mjs reads `mousemove` on the page): the mouse over a Concha
// window, 0..1 from its left / top edge. Process-wide, as the page is for Strudel. Any thread.

#include <atomic>

namespace concha::input {

inline std::atomic<float>& mouseX()
{
    static std::atomic<float> x{0.f};
    return x;
}
inline std::atomic<float>& mouseY()
{
    static std::atomic<float> y{0.f};
    return y;
}
inline void setMouse(float x, float y)
{
    mouseX().store(x);
    mouseY().store(y);
}

} // namespace concha::input
