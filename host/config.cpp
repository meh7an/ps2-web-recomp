#include "config.hpp"

// The file is UTF-8 text, so toml++'s wide-string helpers for Windows are not needed.
#define TOML_ENABLE_WINDOWS_COMPAT 0
#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#endif

// The settings folder's name; the build names its app.
#ifndef PS2_APP_NAME
#define PS2_APP_NAME "ps2-web-recomp"
#endif

namespace ps2::config {

namespace {

constexpr KeyName kKeys[] = {
    {4, "A"}, {5, "B"}, {6, "C"}, {7, "D"}, {8, "E"}, {9, "F"}, {10, "G"}, {11, "H"}, {12, "I"}, {13, "J"}, {14, "K"},
    {15, "L"}, {16, "M"}, {17, "N"}, {18, "O"}, {19, "P"}, {20, "Q"}, {21, "R"}, {22, "S"}, {23, "T"}, {24, "U"},
    {25, "V"}, {26, "W"}, {27, "X"}, {28, "Y"}, {29, "Z"}, {30, "1"}, {31, "2"}, {32, "3"}, {33, "4"}, {34, "5"},
    {35, "6"}, {36, "7"}, {37, "8"}, {38, "9"}, {39, "0"}, {40, "Return"}, {41, "Escape"}, {42, "Backspace"},
    {43, "Tab"}, {44, "Space"}, {45, "-"}, {46, "="}, {47, "["}, {48, "]"}, {49, "\\"}, {50, "#"}, {51, ";"},
    {52, "'"}, {53, "`"}, {54, ","}, {55, "."}, {56, "/"}, {57, "CapsLock"}, {58, "F1"}, {59, "F2"}, {60, "F3"},
    {61, "F4"}, {62, "F5"}, {63, "F6"}, {64, "F7"}, {65, "F8"}, {66, "F9"}, {67, "F10"}, {68, "F11"}, {69, "F12"},
    {70, "PrintScreen"}, {71, "ScrollLock"}, {72, "Pause"}, {73, "Insert"}, {74, "Home"}, {75, "PageUp"},
    {76, "Delete"}, {77, "End"}, {78, "PageDown"}, {79, "Right"}, {80, "Left"}, {81, "Down"}, {82, "Up"},
    {83, "Numlock"}, {84, "Keypad /"}, {85, "Keypad *"}, {86, "Keypad -"}, {87, "Keypad +"}, {88, "Keypad Enter"},
    {89, "Keypad 1"}, {90, "Keypad 2"}, {91, "Keypad 3"}, {92, "Keypad 4"}, {93, "Keypad 5"}, {94, "Keypad 6"},
    {95, "Keypad 7"}, {96, "Keypad 8"}, {97, "Keypad 9"}, {98, "Keypad 0"}, {99, "Keypad ."}, {101, "Application"},
    {103, "Keypad ="}, {224, "Left Ctrl"}, {225, "Left Shift"}, {226, "Left Alt"}, {227, "Left GUI"},
    {228, "Right Ctrl"}, {229, "Right Shift"}, {230, "Right Alt"}, {231, "Right GUI"},
};

// SDL 2.32, src/joystick/SDL_gamecontroller.c (map_StringForControllerButton).
constexpr std::string_view kControllerNames[] = {
    "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder", "rightshoulder",
    "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2", "paddle3", "paddle4", "touchpad",
};
static_assert(std::size(kControllerNames) == kControllerButtons);

constexpr KeyName kMouseNames[] = {{1, "left"}, {2, "middle"}, {3, "right"}, {4, "x1"}, {5, "x2"}};

// The file's pad inputs, in the order it lists them. Parsed bindings keep this order.
struct Input {
    std::string_view name;
    input::Target target;
};

constexpr Input kInputs[] = {
    {"up", {input::kUp, 0}}, {"right", {input::kRight, 0}}, {"down", {input::kDown, 0}}, {"left", {input::kLeft, 0}},
    {"triangle", {input::kTriangle, 0}}, {"circle", {input::kCircle, 0}}, {"cross", {input::kCross, 0}},
    {"square", {input::kSquare, 0}}, {"select", {input::kSelect, 0}}, {"start", {input::kStart, 0}},
    {"l1", {input::kL1, 0}}, {"l2", {input::kL2, 0}}, {"r1", {input::kR1, 0}}, {"r2", {input::kR2, 0}},
    {"l3", {input::kL3, 0}}, {"r3", {input::kR3, 0}},
    {"left_stick_up", {0, input::kLeftUp}}, {"left_stick_right", {0, input::kLeftRight}},
    {"left_stick_down", {0, input::kLeftDown}}, {"left_stick_left", {0, input::kLeftLeft}},
    {"right_stick_up", {0, input::kRightUp}}, {"right_stick_right", {0, input::kRightRight}},
    {"right_stick_down", {0, input::kRightDown}}, {"right_stick_left", {0, input::kRightLeft}},
};

bool same_name(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

int input_index(const input::Target &target) {
    for (std::size_t i = 0; i < std::size(kInputs); ++i)
        if (kInputs[i].target == target) return static_cast<int>(i);
    return -1;
}

std::optional<std::uint16_t> code_in(std::span<const KeyName> names, std::string_view name) {
    for (const KeyName &n : names)
        if (same_name(n.name, name)) return n.code;
    return std::nullopt;
}

std::string_view name_in(std::span<const KeyName> names, std::uint16_t code) {
    for (const KeyName &n : names)
        if (n.code == code) return n.name;
    return {};
}

std::optional<std::uint16_t> controller_code(std::string_view name) {
    for (std::size_t i = 0; i < std::size(kControllerNames); ++i)
        if (same_name(kControllerNames[i], name)) return static_cast<std::uint16_t>(i);
    return std::nullopt;
}

std::string_view controller_name(std::uint16_t code) { return code < std::size(kControllerNames) ? kControllerNames[code] : std::string_view{}; }

// One binding as the keyboard, mouse and controller sections all hold it.
struct Bound {
    std::uint16_t code;
    input::Target target;
};

// How a binding section names its sources.
struct Names {
    std::string_view what;
    std::optional<std::uint16_t> (*code)(std::string_view name);
    std::string_view (*name)(std::uint16_t code);
};

const Names kKeyNaming{"key", [](std::string_view n) { return code_in(kKeys, n); }, [](std::uint16_t c) { return name_in(kKeys, c); }};
const Names kMouseNaming{"mouse button", [](std::string_view n) { return code_in(kMouseNames, n); }, [](std::uint16_t c) { return name_in(kMouseNames, c); }};
const Names kControllerNaming{"controller button", controller_code, controller_name};

int line_of(const toml::source_region &region) { return static_cast<int>(region.begin.line); }

void problem(Loaded &out, int line, std::string setting, std::string reason) { out.problems.push_back({line, std::move(setting), std::move(reason)}); }

void read_int(Loaded &out, const toml::node &node, int line, const std::string &setting, int low, int high, int &value) {
    const std::optional<std::int64_t> v = node.is_integer() ? node.value<std::int64_t>() : std::nullopt;
    if (v && *v >= low && *v <= high) {
        value = static_cast<int>(*v);
        return;
    }
    problem(out, line, setting, std::format("must be a whole number from {} to {}; keeping {}", low, high, value));
}

void read_bool(Loaded &out, const toml::node &node, int line, const std::string &setting, bool &value) {
    if (const toml::value<bool> *v = node.as_boolean()) {
        value = v->get();
        return;
    }
    problem(out, line, setting, std::format("must be true or false; keeping {}", value));
}

std::string_view aspect_name(Aspect aspect) { return aspect == Aspect::kStretch ? "stretch" : "4:3"; }

void read_display(Loaded &out, const toml::table &table) {
    Display &d = out.config.display;
    for (auto &&[key, node] : table) {
        const std::string setting = std::format("display.{}", key.str());
        const int line = line_of(key.source());
        if (key.str() == "width") read_int(out, node, line, setting, 320, 7680, d.width);
        else if (key.str() == "height") read_int(out, node, line, setting, 240, 4320, d.height);
        else if (key.str() == "fullscreen") read_bool(out, node, line, setting, d.fullscreen);
        else if (key.str() == "vsync") read_bool(out, node, line, setting, d.vsync);
        else if (key.str() == "aspect") {
            const std::optional<std::string_view> v = node.value<std::string_view>();
            if (v && *v == "4:3") d.aspect = Aspect::k4x3;
            else if (v && *v == "stretch") d.aspect = Aspect::kStretch;
            else problem(out, line, setting, std::format("must be \"4:3\" or \"stretch\"; keeping \"{}\"", aspect_name(d.aspect)));
        } else {
            problem(out, line, setting, "is not a display setting; ignored");
        }
    }
}

void read_input(Loaded &out, const toml::table &table) {
    input::Settings &s = out.config.input;
    for (auto &&[key, node] : table) {
        const std::string setting = std::format("input.{}", key.str());
        const int line = line_of(key.source());
        if (key.str() == "deadzone") {
            const std::optional<double> v = node.is_number() ? node.value<double>() : std::nullopt;
            if (v && *v >= 0.0 && *v <= 0.9) s.deadzone = *v;
            else problem(out, line, setting, std::format("must be a number from 0.0 to 0.9; keeping {}", s.deadzone));
        } else if (key.str() == "trigger_threshold") {
            read_int(out, node, line, setting, 0, 32767, s.trigger_threshold);
        } else {
            problem(out, line, setting, "is not an input setting; ignored");
        }
    }
}

// A listed input replaces its default bindings, and a source the file binds leaves any default
// input, so one key never drives two inputs (spec 011). Inputs the file does not list keep theirs.
std::vector<Bound> read_bindings(Loaded &out, const toml::table &table, std::string_view section, const Names &names, const std::vector<Bound> &defaults) {
    struct Entry {
        int line;
        std::string_view input;
        const toml::node *node;
    };
    std::vector<Entry> entries;
    for (auto &&[key, node] : table) entries.push_back({line_of(key.source()), key.str(), &node});
    // toml++ keeps a table's keys sorted, and the earlier of two bindings for a source wins.
    std::ranges::stable_sort(entries, {}, &Entry::line);

    std::vector<Bound> chosen;
    std::vector<bool> listed(std::size(kInputs));
    std::vector<std::pair<std::uint16_t, int>> taken; // a bound source, and its input
    const auto owner_of = [&](std::uint16_t code) { return std::ranges::find(taken, code, &std::pair<std::uint16_t, int>::first); };
    for (const Entry &e : entries) {
        const std::string setting = std::format("{}.{}", section, e.input);
        const Input *const found = std::ranges::find(kInputs, e.input, &Input::name);
        if (found == std::end(kInputs)) {
            problem(out, e.line, setting, "is not a pad input; ignored");
            continue;
        }
        const int index = static_cast<int>(found - std::begin(kInputs));
        std::vector<std::string_view> given;
        bool strings = true;
        if (const std::optional<std::string_view> one = e.node->value<std::string_view>()) {
            given.push_back(*one);
        } else if (const toml::array *list = e.node->as_array()) {
            for (const toml::node &item : *list) {
                if (const std::optional<std::string_view> name = item.value<std::string_view>()) given.push_back(*name);
                else strings = false;
            }
        } else {
            strings = false;
        }
        if (!strings) {
            problem(out, e.line, setting, std::format("must be a {} name or a list of them; ignored", names.what));
            continue;
        }
        std::vector<std::uint16_t> codes;
        for (const std::string_view name : given) {
            const std::optional<std::uint16_t> code = names.code(name);
            if (!code) {
                problem(out, e.line, setting, std::format("\"{}\" is not a {} name; ignored", name, names.what));
                continue;
            }
            if (const auto owner = owner_of(*code); owner != taken.end()) {
                problem(out, e.line, setting, std::format("\"{}\" is already bound to {}; ignored", name, kInputs[owner->second].name));
                continue;
            }
            taken.emplace_back(*code, index);
            codes.push_back(*code);
        }
        // An entry whose names all fail leaves its input as it was.
        if (!given.empty() && codes.empty()) continue;
        listed[static_cast<std::size_t>(index)] = true;
        for (const std::uint16_t code : codes) chosen.push_back({code, found->target});
    }

    std::vector<Bound> bound;
    for (const Bound &d : defaults) {
        const int index = input_index(d.target);
        if (index >= 0 && !listed[static_cast<std::size_t>(index)] && owner_of(d.code) == taken.end()) bound.push_back(d);
    }
    bound.insert(bound.end(), chosen.begin(), chosen.end());
    std::ranges::stable_sort(bound, {}, [](const Bound &b) { return input_index(b.target); });
    return bound;
}

template <typename Binding, typename Source>
std::vector<Bound> to_bound(const std::vector<Binding> &bindings, Source source) {
    std::vector<Bound> bound;
    for (const Binding &b : bindings) bound.push_back({static_cast<std::uint16_t>(b.*source), b.target});
    return bound;
}

template <typename Binding, typename Field>
std::vector<Binding> from_bound(const std::vector<Bound> &bound) {
    std::vector<Binding> bindings;
    for (const Bound &b : bound) bindings.push_back({static_cast<Field>(b.code), b.target});
    return bindings;
}

std::string quoted(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out + '"';
}

// TOML reads a number without a point or an exponent as an integer.
std::string decimal(double value) {
    std::string text = std::format("{}", value);
    if (text.find_first_of(".e") == std::string::npos) text += ".0";
    return text;
}

void write_setting(std::string &out, std::size_t width, const std::string &text, std::string_view comment) { out += std::format("{:<{}} # {}\n", text, width, comment); }

void write_bindings(std::string &out, const std::vector<Bound> &bound, const std::vector<Bound> &defaults, const Names &names) {
    for (const Input &in : kInputs) {
        std::string list;
        int count = 0;
        for (const Bound &b : bound) {
            const std::string_view name = b.target == in.target ? names.name(b.code) : std::string_view{};
            if (name.empty()) continue;
            list += (count++ ? ", " : "") + quoted(name);
        }
        const bool by_default = std::ranges::any_of(defaults, [&](const Bound &d) { return d.target == in.target; });
        if (count == 1) out += std::format("{} = {}\n", in.name, list);
        else if (count > 1) out += std::format("{} = [{}]\n", in.name, list);
        else if (by_default) out += std::format("{} = []\n", in.name);
    }
}

}

std::span<const KeyName> key_names() { return kKeys; }

std::span<const std::string_view> controller_names() { return kControllerNames; }

Loaded parse(std::string_view text) {
    Loaded out;
    toml::table root;
    try {
        root = toml::parse(text);
    } catch (const toml::parse_error &error) {
        problem(out, line_of(error.source()), "", std::string(error.description()));
        return out;
    }
    const Config defaults;
    for (auto &&[key, node] : root) {
        const std::string section{key.str()};
        const int line = line_of(key.source());
        const toml::table *table = node.as_table();
        if (section != "display" && section != "input" && section != "keyboard" && section != "mouse" && section != "controller") {
            problem(out, line, section, "is not a section; ignored");
        } else if (!table) {
            problem(out, line, section, "must be a table; ignored");
        } else if (section == "display") {
            read_display(out, *table);
        } else if (section == "input") {
            read_input(out, *table);
        } else if (section == "keyboard") {
            const auto bound = read_bindings(out, *table, section, kKeyNaming, to_bound(defaults.keys, &input::KeyBinding::key));
            out.config.keys = from_bound<input::KeyBinding, std::uint16_t>(bound);
        } else if (section == "mouse") {
            const auto bound = read_bindings(out, *table, section, kMouseNaming, to_bound(defaults.mouse, &input::MouseBinding::button));
            out.config.mouse = from_bound<input::MouseBinding, std::uint8_t>(bound);
        } else {
            const auto bound = read_bindings(out, *table, section, kControllerNaming, to_bound(defaults.controller, &ControllerBinding::button));
            out.config.controller = from_bound<ControllerBinding, std::uint8_t>(bound);
        }
    }
    return out;
}

std::string write(const Config &config) {
    const Config defaults;
    const Display &d = config.display;
    std::string out = std::format("# {} settings. Delete this file to get the defaults back.\n\n[display]\n", PS2_APP_NAME);
    write_setting(out, 20, std::format("width = {}", d.width), "window size at start, 320-7680");
    write_setting(out, 20, std::format("height = {}", d.height), "240-4320");
    write_setting(out, 20, std::format("fullscreen = {}", d.fullscreen), "desktop fullscreen; F11 toggles it while playing");
    write_setting(out, 20, std::format("vsync = {}", d.vsync), "wait for the display's refresh to show a frame");
    write_setting(out, 20, std::format("aspect = \"{}\"", aspect_name(d.aspect)), "\"4:3\" keeps the picture's shape, \"stretch\" fills the window");
    out += "\n[input]\n";
    write_setting(out, 25, std::format("deadzone = {}", decimal(config.input.deadzone)), "stick deadzone as a share of its range, 0.0-0.9");
    write_setting(out, 25, std::format("trigger_threshold = {}", config.input.trigger_threshold), "trigger travel before L2 or R2 presses, 0-32767");
    out += "\n# Each pad input takes one key or a list of keys, by SDL's key names; [] unbinds it.\n[keyboard]\n";
    write_bindings(out, to_bound(config.keys, &input::KeyBinding::key), to_bound(defaults.keys, &input::KeyBinding::key), kKeyNaming);
    out += "\n# Mouse buttons: \"left\", \"middle\", \"right\", \"x1\" and \"x2\". None is bound by default.\n[mouse]\n";
    write_bindings(out, to_bound(config.mouse, &input::MouseBinding::button), to_bound(defaults.mouse, &input::MouseBinding::button), kMouseNaming);
    out += "\n# Controller buttons by SDL's names. Sticks and triggers keep their roles.\n[controller]\n";
    write_bindings(out, to_bound(config.controller, &ControllerBinding::button), to_bound(defaults.controller, &ControllerBinding::button), kControllerNaming);
    return out;
}

std::filesystem::path default_path() {
#ifdef _WIN32
    // Saved Games sits outside %APPDATA%, which an MSIX-packaged parent process redirects.
    PWSTR folder = nullptr;
    std::filesystem::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, KF_FLAG_DEFAULT, nullptr, &folder))) path = std::filesystem::path(folder) / PS2_APP_NAME / "config.toml";
    CoTaskMemFree(folder);
    return path;
#else
    std::filesystem::path base;
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) base = xdg;
    else if (const char *home = std::getenv("HOME"); home && *home) base = std::filesystem::path(home) / ".config";
    return base.empty() ? base : base / PS2_APP_NAME / "config.toml";
#endif
}

Loaded load_or_create(const std::filesystem::path &path) {
    Loaded out;
    if (path.empty()) {
        problem(out, 0, "", "no folder for the settings file is known; using the defaults");
        return out;
    }
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            problem(out, 0, "", "cannot be read; using the defaults");
            return out;
        }
        return parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
    }
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::binary);
    file << write(out.config);
    file.close();
    if (!file) {
        problem(out, 0, "", "cannot be written; using the defaults");
        return out;
    }
    out.created = true;
    return out;
}

}
