#pragma once

#include <functional>
#include <string>

struct WindowOptions {
    bool fullscreen = false; // --fullscreen, over the settings file's choice
    bool use_config = true; // false: the defaults, and no file is read or written
    std::string config_path; // empty: the settings file's default path
};

bool open_window(const WindowOptions &options);
int run_window(const std::function<int()> &emulate);
