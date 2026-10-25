// FreedomUniteRecomp launcher -- see config.h.
#include "config.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

static fs::path P(const std::string &s) { return fs::u8path(s); }
static std::string S(const fs::path &p) { return p.u8string(); }

bool file_exists(const std::string &path) { std::error_code e; return fs::is_regular_file(P(path), e); }
bool dir_exists(const std::string &path)  { std::error_code e; return fs::is_directory(P(path), e); }

std::string read_text(const std::string &path) {
    std::ifstream f(P(path), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_text(const std::string &path, const std::string &text) {
    std::error_code e;
    fs::create_directories(P(path).parent_path(), e);
    std::ofstream f(P(path), std::ios::binary | std::ios::trunc);
    f << text;
    return (bool)f;
}

void open_in_shell(const std::string &path_or_url) {
    if (path_or_url.find("://") != std::string::npos) SDL_OpenURL(path_or_url.c_str());
    else SDL_OpenURL(("file:///" + path_or_url).c_str());
}

// ---- paths ------------------------------------------------------------------

namespace paths {

static std::string g_exe, g_base;

void init(const char *exe_dir) {
    g_exe = exe_dir ? exe_dir : ".";
    while (!g_exe.empty() && (g_exe.back() == '/' || g_exe.back() == '\\')) g_exe.pop_back();
    fs::path p = P(g_exe);
    g_base = g_exe;
    for (int i = 0; i < 6 && !p.empty(); i++) {
        std::error_code e;
        if (fs::is_directory(p / "work", e)) { g_base = S(p); break; }
        if (p == p.parent_path()) break;
        p = p.parent_path();
    }
}

const std::string &exe_dir() { return g_exe; }
const std::string &base() { return g_base; }
std::string work() { return S(P(g_base) / "work"); }
std::string launcher() { return S(P(work()) / "launcher"); }
std::string host_exe() { return S(P(g_exe) / "mhfu_host.exe"); }

std::string iso(const Config &c) {
    if (!c.iso.empty()) return c.iso;
    std::string t = read_text(S(P(work()) / "iso.txt"));
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
    if (t.size() >= 3 && (unsigned char)t[0] == 0xEF) t = t.substr(3);       // a BOM
    return t;
}

std::string eboot(const Config &c) {
    return c.eboot.empty() ? S(P(work()) / "ULES01213_EBOOT.BIN") : c.eboot;
}

std::string profile_root(const std::string &name) {
    if (name.empty() || name == "Default") return S(P(work()) / "root");
    return S(P(work()) / "profiles" / P(name));
}

std::vector<std::string> profiles() {
    std::vector<std::string> out{"Default"};
    std::error_code e;
    for (auto &d : fs::directory_iterator(P(work()) / "profiles", e))
        if (d.is_directory()) out.push_back(d.path().filename().u8string());
    std::sort(out.begin() + 1, out.end());
    return out;
}

static bool valid_name(const std::string &n) {
    if (n.empty() || n.size() > 32 || n == "Default") return false;
    for (char ch : n)
        if (!(isalnum((unsigned char)ch) || ch == ' ' || ch == '-' || ch == '_')) return false;
    return true;
}

bool create_profile(const std::string &name, std::string &err) {
    if (!valid_name(name)) { err = "Use up to 32 letters, digits, spaces, - or _."; return false; }
    fs::path root = P(profile_root(name));
    std::error_code e;
    if (fs::exists(root, e)) { err = "That profile exists already."; return false; }
    fs::create_directories(root / "ms" / "PSP" / "SAVEDATA", e);
    if (e) { err = e.message(); return false; }
    // The system fonts, which every device root needs (they are firmware, not saves).
    fs::path fonts = P(profile_root("Default")) / "flash0";
    if (fs::is_directory(fonts, e)) fs::copy(fonts, root / "flash0", fs::copy_options::recursive, e);
    return true;
}

bool delete_profile(const std::string &name, std::string &err) {
    if (name.empty() || name == "Default") { err = "The Default profile stays."; return false; }
    std::error_code e;
    fs::remove_all(P(profile_root(name)), e);
    if (e) { err = e.message(); return false; }
    return true;
}

}  // namespace paths

// ---- launcher.ini -----------------------------------------------------------

static std::string escape(const std::string &v) {
    std::string o;
    for (char ch : v) {
        if (ch == '\n') o += "\\n";
        else if (ch == '\\') o += "\\\\";
        else if (ch != '\r') o += ch;
    }
    return o;
}

static std::string unescape(const std::string &v) {
    std::string o;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i] == '\\' && i + 1 < v.size()) { o += v[i + 1] == 'n' ? '\n' : v[i + 1]; i++; }
        else o += v[i];
    }
    return o;
}

static std::string ini_path() { return S(P(paths::launcher()) / "launcher.ini"); }

bool config_load(Config &c) {
    std::ifstream f(P(ini_path()));
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = unescape(line.substr(eq + 1));
        auto b = [&](bool &x) { x = v == "1" || v == "true"; };
        if (k == "profile") c.profile = v;
        else if (k == "render_scale") c.render_scale = std::clamp(atoi(v.c_str()), 1, 5);
        else if (k == "output") c.output = v;
        else if (k == "fullscreen") b(c.fullscreen);
        else if (k == "nick") c.nick = v;
        else if (k == "gamepad") c.gamepad = v;
        else if (k == "gamepad_name") c.gamepad_name = v;
        else if (k == "check_updates") b(c.check_updates);
        else if (k == "close_on_start") b(c.close_on_start);
        else if (k == "extra_env") c.extra_env = v;
        else if (k == "extra_args") c.extra_args = v;
        else if (k == "iso") c.iso = v;
        else if (k == "eboot") c.eboot = v;
        else if (k == "ra_user") c.ra_user = v;
        else if (k == "ra_key") c.ra_key = v;
        else if (k == "ra_token") c.ra_token = v;
        else if (k == "ra_ingame") b(c.ra_ingame);
    }
    return true;
}

bool config_save(const Config &c) {
    std::ostringstream o;
    o << "# FreedomUniteRecomp launcher settings\n";
    o << "profile=" << escape(c.profile) << "\n";
    o << "render_scale=" << c.render_scale << "\n";
    o << "output=" << escape(c.output) << "\n";
    o << "fullscreen=" << (c.fullscreen ? 1 : 0) << "\n";
    o << "nick=" << escape(c.nick) << "\n";
    o << "check_updates=" << (c.check_updates ? 1 : 0) << "\n";
    o << "close_on_start=" << (c.close_on_start ? 1 : 0) << "\n";
    o << "extra_env=" << escape(c.extra_env) << "\n";
    o << "extra_args=" << escape(c.extra_args) << "\n";
    o << "iso=" << escape(c.iso) << "\n";
    o << "eboot=" << escape(c.eboot) << "\n";
    o << "# The Web API key is stored here in plain text.\n";
    o << "ra_user=" << escape(c.ra_user) << "\n";
    o << "ra_key=" << escape(c.ra_key) << "\n";
    o << "ra_token=" << escape(c.ra_token) << "\n";
    o << "ra_ingame=" << (c.ra_ingame ? 1 : 0) << "\n";
    return write_text(ini_path(), o.str());
}
