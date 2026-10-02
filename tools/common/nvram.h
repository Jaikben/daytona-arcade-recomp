// --nvram DIR for the headless tools: start from the settings EEPROM and
// backup RAM the app saved in its data folder (ioboard_eeprom.bin,
// backup_ram.bin), e.g. a cabinet type set in test mode. A file that is
// missing or the wrong size is reported and left as the board's default.
#pragma once

#include "runtime/game_loop.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace tools {

inline void load_nvram(rt::GameLoop &game, const std::string &dir) {
    auto load = [](const std::string &path, auto &into) {
        std::ifstream f(path, std::ios::binary);
        const std::vector<uint8_t> d{std::istreambuf_iterator<char>(f), {}};
        if (d.size() == into.size()) std::copy(d.begin(), d.end(), into.begin());
        else std::fprintf(stderr, "nvram: %s not loaded\n", path.c_str());
    };
    load(dir + "/ioboard_eeprom.bin", game.board().io().eeprom);
    load(dir + "/backup_ram.bin", game.board().backup_ram());
}

} // namespace tools
