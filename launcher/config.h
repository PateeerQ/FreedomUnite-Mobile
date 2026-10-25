// FreedomUniteRecomp launcher -- settings (launcher.ini) and where things live.
//
// The launcher sits next to mhfu_host.exe. The install it serves is the
// folder that holds work/ (searched upwards from the executable, so a build
// in build/Release finds the repository's work/). Everything it writes stays
// in work/: work/launcher/launcher.ini, work/launcher/input.ini, the badge
// cache, and profiles under work/profiles/<name> (the Default profile is
// work/root, the device root run.ps1 uses).
#pragma once

#include <string>
#include <vector>

#define LAUNCHER_VERSION "0.1.0"

struct Config {
    std::string profile = "Default";
    int  render_scale = 2;              // the GE's internal resolution: 480x272 times this
    std::string output = "960x544";     // the window's client size, "WxH"
    bool fullscreen = false;
    std::string nick;                   // multiplayer name (--nick); empty: net.cfg's
    std::string gamepad;                // "": the first connected, "none": keyboard only, else an SDL GUID
    std::string gamepad_name;           // that GUID's device name, for showing
    bool check_updates = true;
    bool close_on_start = false;
    std::string extra_env;              // KEY=VALUE per line
    std::string extra_args;
    std::string iso;                    // empty: work/iso.txt (written by setup.ps1)
    std::string eboot;                  // empty: work/ULES01213_EBOOT.BIN
    std::string ra_user, ra_key;        // RetroAchievements username and Web API key
    std::string ra_token;               // the login token for unlocking in game (never the password)
    bool ra_ingame = true;              // unlock achievements while playing
};

namespace paths {
void init(const char *exe_dir);         // finds the install
const std::string &exe_dir();           // where the launcher and mhfu_host are
const std::string &base();              // the folder that holds work/
std::string work();
std::string launcher();                 // work/launcher
std::string host_exe();
std::string iso(const Config &c);
std::string eboot(const Config &c);
std::string profile_root(const std::string &name);
std::vector<std::string> profiles();    // "Default" first
bool create_profile(const std::string &name, std::string &err);
bool delete_profile(const std::string &name, std::string &err);
}

bool config_load(Config &c);
bool config_save(const Config &c);

// Small helpers shared by the launcher's files.
bool file_exists(const std::string &path);
bool dir_exists(const std::string &path);
std::string read_text(const std::string &path);
bool write_text(const std::string &path, const std::string &text);
void open_in_shell(const std::string &path_or_url);
