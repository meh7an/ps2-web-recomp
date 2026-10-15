#include "input.hpp"

#include <algorithm>
#include <cmath>

namespace ps2::input {

namespace {

std::uint8_t to_byte(double value) { return static_cast<std::uint8_t>(std::clamp(std::lround(127.5 + value * 127.5), 0L, 255L)); }

// Four direction bits in the order up, down, left, right push a stick to the ends of its range.
void keys_to_stick(unsigned directions, std::uint8_t &x, std::uint8_t &y) {
    if (!directions) return;
    const int dx = ((directions & 8u) ? 1 : 0) - ((directions & 4u) ? 1 : 0);
    const int dy = ((directions & 2u) ? 1 : 0) - ((directions & 1u) ? 1 : 0);
    x = dx < 0 ? 0x00 : dx > 0 ? 0xFF : 0x80;
    y = dy < 0 ? 0x00 : dy > 0 ? 0xFF : 0x80;
}

// A digital L2 or R2 already presses fully; otherwise the trigger presses past the threshold.
void trigger(std::int16_t value, std::uint16_t bit, int threshold, std::uint16_t &buttons, std::uint8_t &pressure) {
    if (value <= threshold || (buttons & bit)) return;
    buttons |= bit;
    pressure = static_cast<std::uint8_t>(std::max(1, value * 255 / 32767));
}

}

Target from_keys(const std::uint8_t *state, std::size_t count, std::span<const KeyBinding> bindings) {
    Target held;
    for (const KeyBinding &b : bindings)
        if (b.key < count && state[b.key]) held |= b.target;
    return held;
}

Target from_mouse(std::uint32_t pressed, std::span<const MouseBinding> bindings) {
    Target held;
    for (const MouseBinding &b : bindings)
        if (b.button >= 1 && b.button <= 32 && ((pressed >> (b.button - 1)) & 1u) != 0) held |= b.target;
    return held;
}

void stick(std::int16_t x, std::int16_t y, double deadzone, std::uint8_t &out_x, std::uint8_t &out_y) {
    const double fx = std::clamp(x / 32767.0, -1.0, 1.0), fy = std::clamp(y / 32767.0, -1.0, 1.0);
    const double r = std::hypot(fx, fy), dz = std::clamp(deadzone, 0.0, 0.99);
    if (r <= dz) {
        out_x = out_y = 0x80;
        return;
    }
    // Deflection starts from zero at the deadzone's edge, so small movements stay small.
    const double scale = std::min((r - dz) / (1.0 - dz), 1.0) / r;
    out_x = to_byte(fx * scale);
    out_y = to_byte(fy * scale);
}

PadState resolve(const Held &held, const Settings &settings) {
    PadState s;
    s.buttons = held.digital.buttons;
    if (held.controller) {
        stick(held.left_x, held.left_y, settings.deadzone, s.lx, s.ly);
        stick(held.right_x, held.right_y, settings.deadzone, s.rx, s.ry);
        trigger(held.left_trigger, kL2, settings.trigger_threshold, s.buttons, s.l2);
        trigger(held.right_trigger, kR2, settings.trigger_threshold, s.buttons, s.r2);
    }
    // A held key or mouse button takes its whole stick over from the controller.
    keys_to_stick(held.digital.directions & 0x0Fu, s.lx, s.ly);
    keys_to_stick(held.digital.directions >> 4, s.rx, s.ry);
    return s;
}

}
