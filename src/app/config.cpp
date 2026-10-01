#include "app/config.h"

#include <algorithm>

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace app {

std::string Config::path() {
    char *base = SDL_GetPrefPath("daytona-recomp", "daytona93");
    std::string p = base ? std::string(base) + "launcher.ini" : std::string("launcher.ini");
    SDL_free(base);
    return p;
}

void Config::load() {
    std::ifstream f(path());
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "rom") rom_path = v;
        else if (k == "gpu") gpu = v;
        else if (k == "fullscreen") fullscreen = v == "1";
        else if (k == "aspect") aspect = v;
        else if (k == "hud_edges") hud_edges = v == "1";
        else if (k == "volume") volume = std::clamp(std::strtof(v.c_str(), nullptr), 0.0f, 1.0f);
        else if (k == "mute") mute = v == "1";
        else if (k == "native_audio") native_audio = v == "1";
        else if (k == "deadzone") controls.deadzone = std::strtof(v.c_str(), nullptr);
        else if (k == "steer_invert") controls.steer_invert = v == "1";
        else
            for (int a = 0; a < kNumActions; a++) {
                const std::string base = action_key(Action(a));
                if (k == base + ".key") controls.bind[a].key = v.empty() ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(v.c_str());
                else if (k == base + ".pad") controls.bind[a].pad = PadInput::parse(v);
            }
    }
}

void Config::save() const {
    std::ofstream f(path());
    f << "# Daytona USA launcher settings\n";
    f << "rom=" << rom_path << "\n";
    f << "gpu=" << gpu << "\n";
    f << "fullscreen=" << (fullscreen ? 1 : 0) << "\n";
    f << "aspect=" << aspect << "\n";
    f << "hud_edges=" << (hud_edges ? 1 : 0) << "\n";
    f << "volume=" << volume << "\n";
    f << "mute=" << (mute ? 1 : 0) << "\n";
    f << "native_audio=" << (native_audio ? 1 : 0) << "\n";
    f << "deadzone=" << controls.deadzone << "\n";
    f << "steer_invert=" << (controls.steer_invert ? 1 : 0) << "\n";
    for (int a = 0; a < kNumActions; a++) {
        const Binding &b = controls.bind[a];
        f << action_key(Action(a)) << ".key=" << (b.key == SDL_SCANCODE_UNKNOWN ? "" : SDL_GetScancodeName(b.key)) << "\n";
        f << action_key(Action(a)) << ".pad=" << b.pad.save() << "\n";
    }
}

double Config::aspect_ratio() const {
    double w = 0, h = 0;
    if (std::sscanf(aspect.c_str(), "%lf:%lf", &w, &h) != 2 || w <= 0 || h <= 0) return 0;
    return std::min(w / h, kMaxAspect);
}

} // namespace app
