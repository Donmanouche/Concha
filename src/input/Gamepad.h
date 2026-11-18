#pragma once

// Gamepads for Strudel's @strudel/gamepad (navigator.getGamepads(), shimmed in js/src/shims.mjs):
// one process-wide reader thread, started on first use. Values follow the Gamepad API's standard
// mapping: axes [left x, left y, right x, right y] in -1..1, buttons 0..16 in 0..1 (A B X Y, LB RB,
// LT RT, back start, L3 R3, up down left right, home).
//
// Linux: evdev devices (/dev/input/event*) that look like gamepads, re-scanned while none is open.
// Other systems: no gamepads yet (phase 9).

#include <array>

namespace concha::input {

struct GamepadState {
    bool connected = false;
    std::array<float, 4> axes{};
    std::array<float, 17> buttons{};
};

// Gamepad `index` (0..3, in the order they were found). Any thread.
GamepadState gamepadState(int index);

} // namespace concha::input
