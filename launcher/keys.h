// FreedomUniteRecomp launcher -- the keyboard bindings the game reads
// (work/launcher/input.ini; the format and the key names are host/keys.h's).
#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <vector>

struct Binding {
    const char *action;     // input.ini's name
    const char *label;      // what the Controls page shows
    std::string keys;       // "k, space"
};

std::vector<Binding> keys_defaults();
std::vector<Binding> keys_load(const std::string &path);    // defaults for anything missing
bool keys_save(const std::string &path, const std::vector<Binding> &b);

// The input.ini name of a key, or "" for one the game cannot bind.
std::string key_name(SDL_Scancode sc);
