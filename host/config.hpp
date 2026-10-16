#pragma once

// The window runner's settings file: bindings, the window and the picture, in one TOML file. The
// schema and its defaults need only the input module, so a build without toml++ still has them;
// parsing, writing and the name tables live in config.cpp, which is built with toml++ and defines
// PS2_CONFIG. Nothing here knows SDL, so tests need no window and a launcher can link the same code.

#include "input.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ps2::config {

enum class Aspect : std::uint8_t { k4x3, kStretch };

struct Display {
    int width = 960, height = 720; // the window's size at start
    bool fullscreen = false, vsync = false;
    Aspect aspect = Aspect::k4x3; // k4x3 letterboxes, kStretch fills the window
    bool operator==(const Display &) const = default;
};

// Controller buttons in SDL_GameControllerButton's numbering, which window.cpp checks.
enum ControllerButton : std::uint8_t {
    kButtonA, kButtonB, kButtonX, kButtonY, kButtonBack, kButtonGuide, kButtonStart, kButtonLeftStick, kButtonRightStick,
    kButtonLeftShoulder, kButtonRightShoulder, kButtonDpadUp, kButtonDpadDown, kButtonDpadLeft, kButtonDpadRight,
    kButtonMisc1, kButtonPaddle1, kButtonPaddle2, kButtonPaddle3, kButtonPaddle4, kButtonTouchpad, kControllerButtons,
};

struct ControllerBinding {
    std::uint8_t button;
    input::Target target;
    bool operator==(const ControllerBinding &) const = default;
};

// SDL's standard layout. Sticks and triggers have fixed roles.
inline constexpr std::array<ControllerBinding, 14> kDefaultController{{
    {kButtonDpadUp, {input::kUp, 0}}, {kButtonDpadRight, {input::kRight, 0}}, {kButtonDpadDown, {input::kDown, 0}},
    {kButtonDpadLeft, {input::kLeft, 0}}, {kButtonY, {input::kTriangle, 0}}, {kButtonB, {input::kCircle, 0}},
    {kButtonA, {input::kCross, 0}}, {kButtonX, {input::kSquare, 0}}, {kButtonBack, {input::kSelect, 0}},
    {kButtonStart, {input::kStart, 0}}, {kButtonLeftShoulder, {input::kL1, 0}}, {kButtonRightShoulder, {input::kR1, 0}},
    {kButtonLeftStick, {input::kL3, 0}}, {kButtonRightStick, {input::kR3, 0}},
}};

// A default Config holds today's tables and values. Bindings stay in the order of the file's pad
// inputs, so a Config written and parsed again compares equal.
struct Config {
    Display display;
    input::Settings input;
    std::vector<input::KeyBinding> keys{input::kDefaultKeys.begin(), input::kDefaultKeys.end()};
    // No mouse button is bound by default: GoW2 has no camera to aim (spec 011).
    std::vector<input::MouseBinding> mouse;
    std::vector<ControllerBinding> controller{kDefaultController.begin(), kDefaultController.end()};
    bool operator==(const Config &) const = default;
};

struct Problem {
    int line = 0; // 1-based; 0 for a problem with no place in the file
    std::string setting, reason;
};

struct Loaded {
    Config config;
    std::vector<Problem> problems;
    bool created = false; // load_or_create wrote the defaults
};

struct KeyName {
    std::uint16_t code;
    std::string_view name;
};

// SDL's scancode names (SDL 2.32, src/events/SDL_keyboard.c), matched without regard to case.
std::span<const KeyName> key_names();
// SDL's controller button names, indexed by ControllerButton.
std::span<const std::string_view> controller_names();

// Settings missing from text keep their defaults; each bad one is a Problem and keeps its default.
Loaded parse(std::string_view text);
std::string write(const Config &config);
// Saved Games\<app>\config.toml on Windows, or <app>/config.toml under XDG_CONFIG_HOME elsewhere;
// empty when that folder is unknown.
std::filesystem::path default_path();
// Reads path, or writes the defaults there when nothing exists at path. It never overwrites a file.
Loaded load_or_create(const std::filesystem::path &path);

}
