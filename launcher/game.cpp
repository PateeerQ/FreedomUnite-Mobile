// FreedomUniteRecomp launcher -- see game.h.
#include "game.h"
#include "keys.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
static fs::path P(const std::string &s) { return fs::u8path(s); }

// ---- the ready check ---------------------------------------------------------

int saves_in(const std::string &root) {
    int n = 0;
    std::error_code e;
    for (auto &d : fs::directory_iterator(P(root) / "ms" / "PSP" / "SAVEDATA", e))
        if (d.is_directory() && d.path().filename().u8string().rfind("ULES01213", 0) == 0) n++;
    return n;
}

// The disc's product code appears in its first files (UMD_DATA.BIN, PARAM.SFO).
static bool iso_is_mhfu_eu(const std::string &iso) {
    std::ifstream f(P(iso), std::ios::binary);
    if (!f) return false;
    std::string buf(1 << 20, '\0');
    for (int chunk = 0; chunk < 32; chunk++) {
        f.read(buf.data(), (std::streamsize)buf.size());
        const std::string got = buf.substr(0, (size_t)f.gcount());
        if (got.find("ULES-01213") != std::string::npos || got.find("ULES01213") != std::string::npos) return true;
        if (f.gcount() < (std::streamsize)buf.size()) break;
    }
    return false;
}

static int count_dirs(const fs::path &p) {
    int n = 0;
    std::error_code e;
    for (auto &d : fs::directory_iterator(p, e)) if (d.is_directory()) n++;
    return n;
}

static int count_files(const fs::path &p, const char *ext) {
    int n = 0;
    std::error_code e;
    for (auto &d : fs::directory_iterator(p, e))
        if (d.is_regular_file() && d.path().extension().u8string() == ext) n++;
    return n;
}

std::vector<Check> ready_check(const Config &c, const std::string &gpu) {
    std::vector<Check> out;
    const std::string iso = paths::iso(c), eboot = paths::eboot(c);
    std::error_code e;

    if (iso.empty()) out.push_back({false, false, "Game dump", "no ISO set (run setup.ps1, or choose it under Advanced)"});
    else if (!file_exists(iso)) out.push_back({false, false, "Game dump", "not found: " + iso});
    else if (!iso_is_mhfu_eu(iso))
        out.push_back({false, true, "Game dump", "not recognised as ULES01213 (Europe): " + P(iso).filename().u8string()});
    else out.push_back({true, false, "Game dump", "Monster Hunter Freedom Unite, ULES01213 (Europe)"});

    std::string head = read_text(eboot).substr(0, 4);
    if (!file_exists(eboot)) out.push_back({false, false, "Decrypted EBOOT", "not found: " + eboot});
    else if (head != "\x7F" "ELF") out.push_back({false, false, "Decrypted EBOOT", "not decrypted (no ELF header)"});
    else out.push_back({true, false, "Decrypted EBOOT", P(eboot).filename().u8string()});

    const int ovl = count_dirs(P(paths::work()) / "gen_ovl");
    if (!file_exists(paths::host_exe())) out.push_back({false, false, "Recompiled game", "mhfu_host.exe is missing: run setup.ps1"});
    else if (ovl == 0)                  // a release: the overlays are inside mhfu_host.exe
        out.push_back({true, false, "Recompiled game", "mhfu_host.exe"});
    else out.push_back({true, ovl < 298, "Recompiled game",
                        "mhfu_host.exe, " + std::to_string(ovl) + " of 298 overlays recompiled"});

    const int fonts = count_files(P(paths::profile_root("Default")) / "flash0" / "font", ".pgf");
    if (!fonts) out.push_back({false, false, "PSP fonts", "none in work/root/flash0/font: all text would be blank"});
    else out.push_back({true, false, "PSP fonts", std::to_string(fonts) + " system fonts"});

    const bool ffmpeg = file_exists((P(paths::exe_dir()) / "avcodec-61.dll").u8string());
    out.push_back({ffmpeg, !ffmpeg, "Movies and music",
                   ffmpeg ? "FFmpeg found" : "no FFmpeg next to the game: movies black, music silent"});

    const std::string root = paths::profile_root(c.profile);
    const int saves = saves_in(root);
    out.push_back({true, false, "Saves",
                   saves ? "found in " + (P(root) / "ms" / "PSP" / "SAVEDATA").u8string()
                         : "none yet in profile \"" + c.profile + "\": the game starts fresh"});

    if (!gpu.empty()) out.push_back({true, false, "Graphics card", gpu});
    return out;
}

bool ready_to_play(const std::vector<Check> &checks) {
    for (auto &k : checks) if (!k.ok && !k.warn) return false;
    return true;
}

// ---- the game process -----------------------------------------------------------

static std::mutex g_lock;
static std::string g_log_path;
static uint64_t g_log_off;
static bool g_log_reset;
static int g_exit = -1;
static bool g_stopping;         // the Stop button: an end, not a crash

std::string crash_log_path() { return (P(paths::launcher()) / "crash-latest.log").u8string(); }

std::string crash_log_text() { return file_exists(crash_log_path()) ? read_text(crash_log_path()) : std::string(); }

// The run's log as the crash log, with what ended it on top.
static void keep_crash_log(int code) {
    char when[64];
    std::time_t t = std::time(nullptr);
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    char head[256];
    std::snprintf(head, sizeof head, "MHFU Recomp %s, Windows\ncrash: exit code 0x%08X\ntime: %s\n\n",
                  LAUNCHER_VERSION, (unsigned)code, when);
    std::error_code e;
    fs::create_directories(P(paths::launcher()), e);
    std::ofstream out(P(crash_log_path()), std::ios::binary);
    out << head << read_text(g_log_path);
}

#ifdef _WIN32
#include <windows.h>

static HANDLE g_proc;
static DWORD g_pid;

static std::wstring W(const std::string &s) {
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), (int)w.size());
    return w;
}

static std::wstring quote(const std::string &s) { return L"\"" + W(s) + L"\""; }

// This process's environment plus `set` (names compared without case), as a
// CreateProcess block.
static std::wstring env_block(const std::map<std::string, std::string> &set) {
    std::map<std::wstring, std::wstring> env;
    auto upper = [](std::wstring s) { for (auto &ch : s) ch = (wchar_t)towupper(ch); return s; };
    std::map<std::wstring, std::wstring> names;
    wchar_t *all = GetEnvironmentStringsW();
    for (wchar_t *p = all; *p; p += wcslen(p) + 1) {
        std::wstring kv = p;
        const size_t eq = kv.find(L'=', 1);
        if (eq == std::wstring::npos) continue;
        env[upper(kv.substr(0, eq))] = kv;
    }
    FreeEnvironmentStringsW(all);
    for (auto &[k, v] : set) {
        const std::wstring wk = W(k);
        if (v.empty()) env.erase(upper(wk));
        else env[upper(wk)] = wk + L"=" + W(v);
    }
    std::wstring block;
    for (auto &[k, kv] : env) { block += kv; block += L'\0'; }
    block += L'\0';
    return block;
}

static void copy_logs() {
    const fs::path work = P(paths::work());
    char tag[32];
    std::time_t t = std::time(nullptr);
    std::strftime(tag, sizeof tag, "%Y%m%d-%H%M%S", std::localtime(&t));
    std::error_code e;
    fs::create_directories(work / "logs", e);
    fs::copy_file(P(g_log_path), work / "logs" / (std::string("run-") + tag + ".log"), fs::copy_options::overwrite_existing, e);
}

bool game_start(const Config &c, std::string &err) {
    if (game_running()) { err = "The game is already running."; return false; }
    const std::string root = paths::profile_root(c.profile);
    std::error_code e;
    fs::create_directories(P(root) / "ms" / "PSP" / "SAVEDATA", e);
    fs::create_directories(P(paths::work()) / "overlays", e);

    const std::string keys = (P(paths::launcher()) / "input.ini").u8string();
    if (!file_exists(keys)) keys_save(keys, keys_defaults());

    std::wstring cmd = quote(paths::host_exe()) + L" " + quote(paths::eboot(c)) + L" " + quote(root);
    if (!paths::iso(c).empty()) cmd += L" --iso " + quote(paths::iso(c));
    cmd += L" --budget 2000000000 --seconds 864000 --window " + std::to_wstring(c.render_scale);
    if (c.fullscreen) cmd += L" --fullscreen";
    if (!c.output.empty()) cmd += L" --size " + W(c.output);
    cmd += L" --keys " + quote(keys);
    if (!c.nick.empty()) cmd += L" --nick " + quote(c.nick);
    if (!c.extra_args.empty()) cmd += L" " + W(c.extra_args);

    std::map<std::string, std::string> env{{"MHFU_RENDER", "gl"}};
    if (c.fps60) env["MHFU_FPS"] = "60";
    if (c.volume != 100) env["MHFU_VOLUME"] = std::to_string(c.volume);
    if (c.mouse_sens != 100) env["MHFU_MOUSE_SENS"] = std::to_string(c.mouse_sens);
    if (c.widescreen) env["MHFU_WIDESCREEN"] = "1";
    std::string mods;                                     // "damage=0,gathering=1" (host/mods.c)
    for (auto &m : c.mods) mods += (mods.empty() ? "" : ",") + m.first + "=" + (m.second ? "1" : "0");
    if (!mods.empty()) env["MHFU_MODS"] = mods;
    env["MHFU_LAUNCHER_INI"] = (P(paths::launcher()) / "launcher.ini").u8string();   // the game's Options menu saves there
    if (c.gamepad == "none") env["MHFU_NO_GAMEPAD"] = "1";
    else if (!c.gamepad.empty()) env["MHFU_GAMEPAD_GUID"] = c.gamepad;
    if (c.ra_ingame && !c.ra_user.empty() && !c.ra_token.empty()) {
        env["MHFU_RA_USER"] = c.ra_user;
        env["MHFU_RA_TOKEN"] = c.ra_token;
    }
    std::istringstream lines(c.extra_env);
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos || eq == 0) continue;
        env[line.substr(0, eq)] = line.substr(eq + 1);
    }
    std::wstring block = env_block(env);

    // Its output into work/run.log, through an inheritable handle.
    const std::string log = (P(paths::work()) / "run.log").u8string();
    SECURITY_ATTRIBUTES sa{sizeof sa, NULL, TRUE};
    HANDLE out = CreateFileW(W(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) { err = "cannot write " + log; return false; }

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError = out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring dir = W(paths::exe_dir());
    const BOOL ok = CreateProcessW(NULL, cmd.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                   block.data(), dir.c_str(), &si, &pi);
    CloseHandle(out);
    if (!ok) { err = "could not start mhfu_host.exe (error " + std::to_string(GetLastError()) + ")"; return false; }
    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_proc = pi.hProcess;
        g_pid = pi.dwProcessId;
        g_log_path = log;
        g_log_off = 0;
        g_log_reset = true;
        g_exit = -1;
        g_stopping = false;
    }
    // When it ends: its exit code, and the log kept under work/logs (and as
    // the crash log when it ended with an error).
    HANDLE h = pi.hProcess;
    std::thread([h] {
        WaitForSingleObject(h, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(h, &code);
        std::lock_guard<std::mutex> l(g_lock);
        g_exit = (int)code;
        copy_logs();
        if (code != 0 && !g_stopping) keep_crash_log((int)code);
        CloseHandle(h);
        if (g_proc == h) { g_proc = NULL; g_pid = 0; }
    }).detach();
    return true;
}

bool game_running() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_proc != NULL;
}

int game_exit_code() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_proc ? -1 : g_exit;
}

static BOOL CALLBACK close_window(HWND w, LPARAM pid) {
    DWORD p = 0;
    GetWindowThreadProcessId(w, &p);
    wchar_t cls[64];
    if (p == (DWORD)pid && GetClassNameW(w, cls, 64) && !wcscmp(cls, L"mhfu_host")) PostMessageW(w, WM_CLOSE, 0, 0);
    return TRUE;
}

void game_stop() {
    HANDLE h;
    DWORD pid;
    {
        std::lock_guard<std::mutex> l(g_lock);
        h = g_proc;
        pid = g_pid;
    }
    if (!h) return;
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_stopping = true;
    }
    EnumWindows(close_window, (LPARAM)pid);       // the window's close: the game writes its report
    std::thread([h] {
        if (WaitForSingleObject(h, 8000) == WAIT_TIMEOUT) TerminateProcess(h, 1);
    }).detach();
}

#else

bool game_start(const Config &, std::string &err) { err = "starting the game is Windows-only for now"; return false; }
bool game_running() { return false; }
int game_exit_code() { return g_exit; }
void game_stop() {}

#endif

std::string game_log_poll(bool &reset) {
    std::string path;
    uint64_t off;
    {
        std::lock_guard<std::mutex> l(g_lock);
        reset = g_log_reset;
        g_log_reset = false;
        path = g_log_path;
        off = g_log_off;
    }
    if (path.empty()) return "";
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return "";
    f.seekg(0, std::ios::end);
    const uint64_t size = (uint64_t)f.tellg();
    if (size <= off) return "";
    const uint64_t n = std::min<uint64_t>(size - off, 1 << 20);
    std::string buf((size_t)n, '\0');
    f.seekg((std::streamoff)off);
    f.read(buf.data(), (std::streamsize)n);
    buf.resize((size_t)f.gcount());
    std::lock_guard<std::mutex> l(g_lock);
    g_log_off = off + buf.size();
    return buf;
}
