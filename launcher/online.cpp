// FreedomUniteRecomp launcher -- see online.h.
#include "online.h"
#include "config.h"
#include "http.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <thread>

using json = nlohmann::json;
namespace fs = std::filesystem;

static std::mutex g_lock;
static UpdateInfo g_update;
static RaGame g_ra;
static int g_ra_generation;      // a newer fetch or a forget makes older threads' results stale

// ---- versions ----------------------------------------------------------------

static std::vector<int> parts(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v = v.substr(1);
    std::vector<int> out;
    size_t i = 0;
    while (i < v.size() && isdigit((unsigned char)v[i])) {
        int n = 0;
        while (i < v.size() && isdigit((unsigned char)v[i])) n = n * 10 + (v[i++] - '0');
        out.push_back(n);
        if (i < v.size() && v[i] == '.') i++;
        else break;
    }
    return out;
}

bool version_newer(const std::string &tag, const std::string &than) {
    auto a = parts(tag), b = parts(than);
    if (a.empty()) return false;
    a.resize(std::max(a.size(), b.size()));
    b.resize(a.size());
    return a > b;
}

// ---- updates -----------------------------------------------------------------

void update_check_start() {
    const std::string repo = UPDATE_REPO;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (g_update.state == Job::Busy) return;
        g_update = UpdateInfo{};
        g_update.state = Job::Busy;
    }
    std::thread([repo] {
        UpdateInfo u;
        std::string body, err;
        if (repo.find('/') == std::string::npos) {
            u.state = Job::Failed;
            u.error = "no repository set (owner/name)";
        } else if (!http_get("https://api.github.com/repos/" + repo + "/releases/latest", body, err)) {
            u.state = Job::Failed;
            u.error = err == "HTTP 404" ? "no release published yet (or the repository is private)" : err;
        } else {
            try {
                json j = json::parse(body);
                u.latest = j.value("tag_name", "");
                u.name = j.value("name", u.latest);
                u.notes = j.value("body", "");
                u.url = j.value("html_url", "https://github.com/" + repo + "/releases");
                u.newer = version_newer(u.latest, LAUNCHER_VERSION);
                u.state = Job::Done;
            } catch (const std::exception &e) {
                u.state = Job::Failed;
                u.error = std::string("unreadable answer: ") + e.what();
            }
        }
        std::lock_guard<std::mutex> l(g_lock);
        g_update = u;
    }).detach();
}

UpdateInfo update_info() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_update;
}

// ---- RetroAchievements ---------------------------------------------------------

static std::string url_encode(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char ch : s) {
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') o += (char)ch;
        else { o += '%'; o += hex[ch >> 4]; o += hex[ch & 15]; }
    }
    return o;
}

static std::string cache_dir() { return (fs::u8path(paths::launcher()) / "cache").u8string(); }

// The file, downloading it into the cache first if needed. "" on failure.
static std::string cached(const std::string &url, const std::string &name) {
    const fs::path p = fs::u8path(cache_dir()) / name;
    std::error_code e;
    if (fs::is_regular_file(p, e) && fs::file_size(p, e) > 0) return p.u8string();
    std::string body, err;
    if (!http_get(url, body, err) || body.empty()) return "";
    return write_text(p.u8string(), body) ? p.u8string() : "";
}

static std::string str(const json &j, const char *k) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

static int num(const json &j, const char *k) {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return 0;
    if (it->is_number()) return it->get<int>();
    if (it->is_string()) return atoi(it->get<std::string>().c_str());
    return 0;
}

void ra_fetch_start(const std::string &user, const std::string &key) {
    int gen;
    {
        std::lock_guard<std::mutex> l(g_lock);
        gen = ++g_ra_generation;
        g_ra = RaGame{};
        g_ra.state = Job::Busy;
        g_ra.user = user;
    }
    std::thread([user, key, gen] {
        RaGame g;
        g.user = user;
        std::string body, err;
        const std::string url = "https://retroachievements.org/API/API_GetGameInfoAndUserProgress.php?g=" +
                                std::to_string(RA_GAME_ID) + "&u=" + url_encode(user) + "&z=" +
                                url_encode(user) + "&y=" + url_encode(key);
        if (!http_get(url, body, err)) {
            g.state = Job::Failed;
            g.error = err == "HTTP 401" || err == "HTTP 403" ? "the username or Web API key was not accepted" : err;
        } else {
            try {
                json j = json::parse(body);
                if (!j.is_object() || !j.contains("Achievements")) {
                    g.state = Job::Failed;
                    g.error = j.is_object() && j.contains("message") ? str(j, "message")
                                                                      : "the username or Web API key was not accepted";
                } else {
                    g.title = str(j, "Title");
                    g.total = num(j, "NumAchievements");
                    g.unlocked = num(j, "NumAwardedToUser");
                    g.unlocked_hc = num(j, "NumAwardedToUserHardcore");
                    const std::string icon = str(j, "ImageIcon");
                    if (!icon.empty()) g.icon_file = cached("https://media.retroachievements.org" + icon, "game_icon.png");
                    for (auto &[id, a] : j["Achievements"].items()) {
                        Achievement x;
                        x.id = num(a, "ID");
                        x.title = str(a, "Title");
                        x.description = str(a, "Description");
                        x.points = num(a, "Points");
                        x.order = num(a, "DisplayOrder");
                        x.badge = str(a, "BadgeName");
                        x.earned = str(a, "DateEarned");
                        x.earned_hc = str(a, "DateEarnedHardcore");
                        g.points += x.points;
                        if (!x.earned.empty() || !x.earned_hc.empty()) g.points_earned += x.points;
                        g.list.push_back(x);
                    }
                    std::sort(g.list.begin(), g.list.end(), [](const Achievement &a, const Achievement &b) {
                        return a.order != b.order ? a.order < b.order : a.id < b.id;
                    });
                    if (!g.total) g.total = (int)g.list.size();
                    g.state = Job::Done;
                }
            } catch (const std::exception &e) {
                g.state = Job::Failed;
                g.error = std::string("unreadable answer: ") + e.what();
            }
        }
        {
            std::lock_guard<std::mutex> l(g_lock);
            if (gen != g_ra_generation) return;
            g_ra = g;
        }
        if (g.state != Job::Done) return;
        // The badges, after the list is shown: each arrives as it is cached.
        for (size_t i = 0; i < g.list.size(); i++) {
            const std::string b = g.list[i].badge;
            if (b.empty()) continue;
            const std::string on = cached("https://media.retroachievements.org/Badge/" + b + ".png", "badge_" + b + ".png");
            const std::string off =
                cached("https://media.retroachievements.org/Badge/" + b + "_lock.png", "badge_" + b + "_lock.png");
            std::lock_guard<std::mutex> l(g_lock);
            if (gen != g_ra_generation) return;
            g_ra.list[i].badge_file = on;
            g_ra.list[i].lock_file = off;
            g_ra.badges_ready++;
        }
    }).detach();
}

RaGame ra_game() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_ra;
}

static RaLogin g_login;

void ra_login_start(const std::string &user, const std::string &password) {
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (g_login.state == Job::Busy) return;
        g_login = RaLogin{};
        g_login.state = Job::Busy;
    }
    std::thread([user, password] {
        RaLogin r;
        std::string body, err;
        const std::string form = "r=login2&u=" + url_encode(user) + "&p=" + url_encode(password);
        if (!http_post("https://retroachievements.org/dorequest.php", form, body, err) && body.empty()) {
            r.state = Job::Failed;
            r.error = err;
        } else {
            try {
                json j = json::parse(body);
                if (j.value("Success", false) && j.contains("Token")) {
                    r.state = Job::Done;
                    r.user = str(j, "User");
                    r.token = str(j, "Token");
                } else {
                    r.state = Job::Failed;
                    r.error = j.contains("Error") ? str(j, "Error") : "the username or password was not accepted";
                }
            } catch (const std::exception &) {
                r.state = Job::Failed;
                r.error = err.empty() ? "unreadable answer" : err;
            }
        }
        std::lock_guard<std::mutex> l(g_lock);
        g_login = r;
    }).detach();
}

RaLogin ra_login() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_login;
}

void ra_login_reset() {
    std::lock_guard<std::mutex> l(g_lock);
    g_login = RaLogin{};
}

void ra_forget() {
    std::lock_guard<std::mutex> l(g_lock);
    g_ra_generation++;
    g_ra = RaGame{};
}
