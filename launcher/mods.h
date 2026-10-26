// FreedomUniteRecomp launcher -- the Mods page's data: the built-in mods and
// the player's cheats (<profile>/cheats/ULES01213.ini, CWCheat as PPSSPP reads
// it; the game reads it again when it changes, so a tick applies at once).
#pragma once

#include <string>
#include <vector>

struct BuiltinMod {
    const char *id, *name, *description;
};
// The code mods recompiled into the game (tools/mods_convert.py's MODS), on by default.
extern const BuiltinMod k_builtin_mods[];
extern const int k_nbuiltin_mods;

struct Cheat {
    std::string name;
    int line;                // its _C line in the file
    bool on;
    bool header;             // a title or a note (no lines, or only zeros)
    bool writes_code;        // writes the game's code: no effect on a recompiled game
};

std::string cheats_path(const std::string &profile_root);
// The file's cheats for ULES-01213.
std::vector<Cheat> cheats_load(const std::string &path);
// One cheat on or off, in the file.
bool cheats_set(const std::string &path, const Cheat &c, bool on, std::string &err);
// ULES-01213's block out of a cheat database or file (cheat.db, a .ini), into
// the profile's file; cheats it already has keep their on/off. How many cheats.
int cheats_import(const std::string &path, const std::string &text, std::string &err);

// Downloading the CWCheat Database Plus (the one PPSSPP uses), in the background.
enum class CheatJob { Idle, Busy, Done, Failed };
void cheats_download_start(const std::string &path);
CheatJob cheats_download_state(std::string &message);
