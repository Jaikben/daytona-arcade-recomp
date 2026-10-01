// Launcher settings, saved to launcher.ini in the user's data folder (SDL's
// pref path): the ROM set, the GPU backend, fullscreen, audio volume, and
// the control bindings. Plain key=value lines, so it can be edited by hand.
#pragma once

#include "app/controls.h"

#include <string>

namespace app {

struct Config {
    std::string rom_path;
    std::string gpu;          // "" (automatic), vulkan, direct3d12, metal
    bool fullscreen = false;
    bool skip_launcher = false; // start the game straight away (as --autostart); Esc still opens the launcher
    float volume = 0.8f;      // 0..1
    bool mute = false;
    bool native_audio = false; // applies on reset; reference remains the default
    // Enhancements (off by default).
    std::string aspect;        // widescreen: "" (original 4:3), "16:10", "16:9", "21:9"
    bool hud_edges = false;    // with widescreen: lap times, position and maps at the screen edges
    bool stretch_backdrop = false; // with widescreen, in-game: the tile backdrop stretched across the width, else plain sky
    int draw_distance = 0;     // scenery: 0 = the game's own, -2..+2 (rt::Enhance)
    static constexpr double kMaxAspect = 21.0 / 9.0;
    double aspect_ratio() const; // width / height; 0 = original
    Controls controls;

    Config() { controls.set_defaults(); }
    static std::string path();  // <pref path>/launcher.ini
    void load();
    void save() const;
};

} // namespace app
