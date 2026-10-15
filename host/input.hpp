#pragma once

// Turns the host's keys, mouse buttons and controller state into one DualShock 2 state. It knows
// nothing of SDL, so its mapping and stick math can be tested without a window.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ps2::input {

// Pad buttons, in the bit order set_pad_buttons takes.
enum Button : std::uint16_t {
    kSelect = 1u << 0, kL3 = 1u << 1, kR3 = 1u << 2, kStart = 1u << 3,
    kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6, kLeft = 1u << 7,
    kL2 = 1u << 8, kR2 = 1u << 9, kL1 = 1u << 10, kR1 = 1u << 11,
    kTriangle = 1u << 12, kCircle = 1u << 13, kCross = 1u << 14, kSquare = 1u << 15,
};

// Stick directions a key or mouse button can push to the end of their range.
enum Direction : std::uint8_t {
    kLeftUp = 1u << 0, kLeftDown = 1u << 1, kLeftLeft = 1u << 2, kLeftRight = 1u << 3,
    kRightUp = 1u << 4, kRightDown = 1u << 5, kRightLeft = 1u << 6, kRightRight = 1u << 7,
};

// Keys by USB HID usage, which is also what SDL's scancodes are.
enum Key : std::uint16_t {
    kKeyA = 4, kKeyD = 7, kKeyE = 8, kKeyF = 9, kKeyG = 10, kKeyH = 11, kKeyI = 12, kKeyJ = 13, kKeyK = 14, kKeyL = 15,
    kKeyQ = 20, kKeyS = 22, kKeyT = 23, kKeyW = 26, kKey1 = 30, kKey2 = 31, kKey3 = 32, kKey4 = 33,
    kKeyReturn = 40, kKeyBackspace = 42, kKeyRight = 79, kKeyLeft = 80, kKeyDown = 81, kKeyUp = 82,
};

// What one key, mouse button or controller button drives.
struct Target {
    std::uint16_t buttons = 0;
    std::uint8_t directions = 0;

    Target &operator|=(const Target &other) {
        buttons |= other.buttons;
        directions |= other.directions;
        return *this;
    }
    bool operator==(const Target &) const = default;
};

struct KeyBinding {
    std::uint16_t key;
    Target target;
};

// Mouse buttons by SDL's numbering: 1 left, 2 middle, 3 right, 4 and 5 the side buttons.
struct MouseBinding {
    std::uint8_t button;
    Target target;
};

// PCSX2's keyboard layout: each key drives one input, so no press reaches the game twice.
inline constexpr std::array<KeyBinding, 24> kDefaultKeys{{
    {kKeyUp, {kUp, 0}}, {kKeyRight, {kRight, 0}}, {kKeyDown, {kDown, 0}}, {kKeyLeft, {kLeft, 0}},
    {kKeyI, {kTriangle, 0}}, {kKeyL, {kCircle, 0}}, {kKeyK, {kCross, 0}}, {kKeyJ, {kSquare, 0}},
    {kKeyBackspace, {kSelect, 0}}, {kKeyReturn, {kStart, 0}}, {kKeyQ, {kL1, 0}}, {kKey1, {kL2, 0}},
    {kKeyE, {kR1, 0}}, {kKey3, {kR2, 0}}, {kKey2, {kL3, 0}}, {kKey4, {kR3, 0}},
    {kKeyW, {0, kLeftUp}}, {kKeyD, {0, kLeftRight}}, {kKeyS, {0, kLeftDown}}, {kKeyA, {0, kLeftLeft}},
    {kKeyT, {0, kRightUp}}, {kKeyH, {0, kRightRight}}, {kKeyG, {0, kRightDown}}, {kKeyF, {0, kRightLeft}},
}};

struct Settings {
    double deadzone = 0.10;      // radial, as a share of the stick's range
    int trigger_threshold = 2000; // of 32767, before a trigger presses L2 or R2
};

// One port's inputs for one poll. Controller values are in SDL's ranges: sticks -32768 to 32767,
// with up and left negative, and triggers 0 to 32767.
struct Held {
    Target digital;
    bool controller = false;
    std::int16_t left_x = 0, left_y = 0, right_x = 0, right_y = 0;
    std::int16_t left_trigger = 0, right_trigger = 0;
};

struct PadState {
    std::uint16_t buttons = 0;
    std::uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    std::uint8_t l2 = 0xFF, r2 = 0xFF; // pressure, used while the button is pressed
    bool operator==(const PadState &) const = default;
};

// state[key] is non-zero while the key is held, as SDL_GetKeyboardState reports it.
Target from_keys(const std::uint8_t *state, std::size_t count, std::span<const KeyBinding> bindings);
// pressed has bit (button - 1) set while a mouse button is held, as SDL_GetMouseState reports it.
Target from_mouse(std::uint32_t pressed, std::span<const MouseBinding> bindings);
// A radial deadzone, with the rest of the range rescaled to the pad's 0x00-0xFF and 0x80 at rest.
void stick(std::int16_t x, std::int16_t y, double deadzone, std::uint8_t &out_x, std::uint8_t &out_y);
PadState resolve(const Held &held, const Settings &settings);

}
