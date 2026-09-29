// Launcher settings, saved to launcher.ini in the user's data folder (SDL's
// pref path): the ROM set, the GPU backend, fullscreen, and the control
// bindings. Plain key=value lines, so it can be edited by hand.
#pragma once

#include "app/controls.h"

#include <string>

namespace app {

struct Config {
    std::string rom_path;
    std::string gpu;          // "" (automatic), vulkan, direct3d12, metal
    bool fullscreen = false;
    Controls controls;

    Config() { controls.set_defaults(); }
    static std::string path();  // <pref path>/launcher.ini
    void load();
    void save() const;
};

} // namespace app
