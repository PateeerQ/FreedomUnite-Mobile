// FreedomUniteRecomp launcher -- see mods.h.
#include "mods.h"

#include "config.h"
#include "http.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

const BuiltinMod k_builtin_mods[] = {
    {"targetcam", "Target Camera + Crosshair", "Community mod: Target Camera Mod v0.7, with its crosshair."},
    {"damage", "Damage Numbers", "Community mod: Damage Numbers Display v0.4."},
    {"gathering", "Gathering Node Indicators", "Community mod: Gathering Nodes Mod v0.4."},
};
const int k_nbuiltin_mods = (int)(sizeof k_builtin_mods / sizeof k_builtin_mods[0]);

static const char *DB_URL = "https://raw.githubusercontent.com/Saramagrean/CWCheat-Database-Plus-/master/cheat.db";

std::string cheats_path(const std::string &profile_root) {
    return (fs::u8path(profile_root) / "cheats" / "ULES01213.ini").u8string();
}

static std::vector<std::string> lines_of(const std::string &text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
    }
    return out;
}

static std::string trim(std::string s) {
    while (!s.empty() && (s[0] == ' ' || s[0] == '\t')) s.erase(0, 1);
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
    return s;
}

static bool our_game(const std::string &l) {
    return l.find("ULES-01213") != std::string::npos || l.find("ULES01213") != std::string::npos;
}

std::vector<Cheat> cheats_load(const std::string &path) {
    std::vector<Cheat> out;
    const std::vector<std::string> ls = lines_of(read_text(path));
    bool game = true;
    Cheat *cur = nullptr;
    bool any_line = false, any_nonzero = false;
    auto finish = [&]() {
        if (cur) cur->header = !any_line || !any_nonzero;
    };
    for (int i = 0; i < (int)ls.size(); i++) {
        const std::string l = trim(ls[i]);
        if (l.size() < 2 || l[0] != '_') continue;
        if (l[1] == 'S') { finish(); cur = nullptr; game = our_game(l); continue; }
        if (!game) continue;
        if (l[1] == 'C') {
            finish();
            Cheat c;
            c.name = l.size() > 4 ? l.substr(4) : "";
            c.line = i;
            c.on = l.size() > 2 && l[2] >= '1' && l[2] <= '9';
            c.header = false;
            c.writes_code = false;
            out.push_back(c);
            cur = &out.back();
            any_line = any_nonzero = false;
        } else if (l[1] == 'L' && cur) {
            unsigned a = 0, b = 0;
            if (sscanf(l.c_str() + 2, "%x %x", &a, &b) == 2) {
                any_line = true;
                if (a || b) any_nonzero = true;
                // Types 0-2, 4 and 8 write where they say: the EBOOT's code is recompiled (host/mods.h).
                const unsigned t = a >> 28, off = a & 0x0FFFFFFF;
                if ((t <= 2 || t == 4 || t == 8) && off >= 0x4000 && off < 0x10E6B8) cur->writes_code = true;
            }
        }
    }
    finish();
    return out;
}

bool cheats_set(const std::string &path, const Cheat &c, bool on, std::string &err) {
    std::vector<std::string> ls = lines_of(read_text(path));
    if (c.line < 0 || c.line >= (int)ls.size()) { err = "the cheat file changed"; return false; }
    std::string &l = ls[c.line];
    const size_t at = l.find("_C");
    if (at == std::string::npos || at + 2 >= l.size()) { err = "the cheat file changed"; return false; }
    l[at + 2] = on ? '1' : '0';
    std::ofstream f(fs::u8path(path), std::ios::binary);
    for (auto &x : ls) f << x << "\n";
    if (!f) { err = "could not write " + path; return false; }
    return true;
}

int cheats_import(const std::string &path, const std::string &text, std::string &err) {
    // ULES-01213's block (or the whole text when it names no game).
    const std::vector<std::string> src = lines_of(text);
    std::vector<std::string> block;
    bool has_games = false, in = false;
    for (auto &raw : src) {
        const std::string l = trim(raw);
        if (l.rfind("_S", 0) == 0) { has_games = true; in = our_game(l); continue; }
        if (in || !has_games) {
            if (l.rfind("_C", 0) == 0 || l.rfind("_L", 0) == 0) block.push_back(l);
        }
    }
    if (block.empty()) { err = "no cheats for ULES-01213 in it"; return -1; }
    // What is on now stays on.
    std::vector<std::string> on;
    for (auto &c : cheats_load(path))
        if (c.on) on.push_back(c.name);
    std::ofstream f;
    fs::create_directories(fs::u8path(path).parent_path());
    f.open(fs::u8path(path), std::ios::binary);
    f << "_S ULES-01213\n_G Monster Hunter Freedom Unite [EU]\n";
    int n = 0;
    for (auto &l : block) {
        if (l.rfind("_C", 0) == 0) {
            const std::string name = l.size() > 4 ? l.substr(4) : "";
            const bool was_on = std::find(on.begin(), on.end(), name) != on.end();
            f << "_C" << (was_on ? '1' : '0') << " " << name << "\n";
            n++;
        } else {
            f << l << "\n";
        }
    }
    if (!f) { err = "could not write " + path; return -1; }
    return n;
}

static std::mutex g_lock;
static CheatJob g_state = CheatJob::Idle;
static std::string g_message;

void cheats_download_start(const std::string &path) {
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (g_state == CheatJob::Busy) return;
        g_state = CheatJob::Busy;
        g_message = "Downloading the CWCheat Database Plus...";
    }
    std::thread([path] {
        std::string body, err;
        CheatJob st = CheatJob::Failed;
        std::string msg;
        if (!http_get(DB_URL, body, err)) {
            msg = "Could not download: " + err;
        } else {
            const int n = cheats_import(path, body, err);
            if (n < 0) msg = err;
            else { st = CheatJob::Done; msg = std::to_string(n) + " cheats for ULES-01213 from the CWCheat Database Plus."; }
        }
        std::lock_guard<std::mutex> l(g_lock);
        g_state = st;
        g_message = msg;
    }).detach();
}

CheatJob cheats_download_state(std::string &message) {
    std::lock_guard<std::mutex> l(g_lock);
    message = g_message;
    return g_state;
}
