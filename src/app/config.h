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
    std::string renderer = "software"; // the 3D: software (CPU, exact) or hardware (SDL_GPU)
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
    int draw_mode = 0;         // 0 double buffered (every frame), 1 single buffered (every 2nd), 2 every third frame
    int supersampling = 1;     // hardware renderer: drawn at 1 (off) to 4 times the original resolution
    static constexpr double kMaxAspect = 21.0 / 9.0;
    double aspect_ratio() const; // width / height; 0 = original
    Controls controls;
    // Link play (the communication board; Revision A): this cabinet listens
    // for the one before it in the ring and connects to the next. Master or
    // slave, and the car number, are the game's own settings (test mode).
    bool link = false;
    int link_port = 15112;               // where the cabinet before this one connects
    std::string link_next = "127.0.0.1:15113"; // host:port of the next cabinet
    bool link_framesync = false;         // hold every cabinet to the master's frame
    float ffb_strength = 0.7f; // force feedback (the drive board) on the steering device: 0 off .. 1
    bool ffb_invert = false;   // turn the wheel the other way

    Config() { controls.set_defaults(); }
    static std::string path();  // <pref path>/launcher.ini
    // --profile NAME: a separate data folder (settings, EEPROM, backup RAM),
    // e.g. a second cabinet on the same computer for link play.
    static inline std::string profile;
    static std::string pref_dir(); // where this set's (and profile's) data lives, with a trailing separator
    void load();
    void save() const;
};

} // namespace app
