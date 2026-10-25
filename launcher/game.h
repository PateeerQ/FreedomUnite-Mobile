// FreedomUniteRecomp launcher -- the ready check, and running the game.
//
// The game is mhfu_host.exe, started the way scripts/run.ps1 starts it: the
// decrypted EBOOT, the profile's device root, --iso, --window <render scale>,
// plus --size/--fullscreen/--keys/--nick from the launcher's settings. Its
// output goes straight to work/run.log (so it is kept even if the launcher
// closes) and the Log page follows that file; when it ends, the log and the
// last frame are copied to work/logs/ as run.ps1 does.
#pragma once

#include "config.h"

#include <string>
#include <vector>

struct Check {
    bool ok;
    bool warn;              // not fatal: shown, but Play stays enabled
    std::string label;
    std::string detail;
};

std::vector<Check> ready_check(const Config &c, const std::string &gpu);
bool ready_to_play(const std::vector<Check> &checks);
int  saves_in(const std::string &profile_root);

bool game_start(const Config &c, std::string &err);
bool game_running();
void game_stop();               // closes the window (so the game reports), then forces it
int  game_exit_code();          // of the last run; -1 while running or never run

// The last crash's log (work/launcher/crash-latest.log: the run's log after a
// header with the version, Windows and the exit code), kept whenever the game
// ends with an error rather than by its window or the Stop button.
std::string crash_log_path();
std::string crash_log_text();   // "" when there is none

// New log text since the last call (the whole log after a restart: `reset` true).
std::string game_log_poll(bool &reset);
