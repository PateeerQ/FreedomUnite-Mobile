// FreedomUniteRecomp launcher -- the online parts, each on a thread of its
// own: the newest release on GitHub, and the player's RetroAchievements
// progress for MHFU (game 3732) with its badges cached on disk.
#pragma once

#include <mutex>
#include <string>
#include <vector>

enum class Job { Idle, Busy, Done, Failed };

struct UpdateInfo {
    Job state = Job::Idle;
    std::string latest;         // the release's tag, "v0.2.0"
    std::string name, notes, url;
    bool newer = false;         // newer than LAUNCHER_VERSION
    std::string error;
};

struct Achievement {
    int id = 0, points = 0, order = 0;
    std::string title, description, badge, earned, earned_hc;
    std::string badge_file, lock_file;     // in the cache once downloaded
};

struct RaGame {
    Job state = Job::Idle;
    std::string title, icon_file, user;
    int total = 0, unlocked = 0, unlocked_hc = 0, points = 0, points_earned = 0;
    std::vector<Achievement> list;
    std::string error;
    int badges_ready = 0;       // bumps as badge files arrive
};

constexpr int RA_GAME_ID = 3732;

// Releases come only from the project's own repository.
constexpr const char *UPDATE_REPO = "PateeerQ/FreedomUniteRecomp";

void update_check_start();
UpdateInfo update_info();

void ra_fetch_start(const std::string &user, const std::string &key);
RaGame ra_game();
void ra_forget();

// Signing in with the password once, for the token the game unlocks with.
struct RaLogin {
    Job state = Job::Idle;
    std::string user, token, error;
};
void ra_login_start(const std::string &user, const std::string &password);
RaLogin ra_login();
void ra_login_reset();

bool version_newer(const std::string &tag, const std::string &than);
