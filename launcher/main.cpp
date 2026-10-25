// FreedomUniteRecomp launcher -- the window and its pages.
//
//   Play          ready check, profile, upscaler, output size, fullscreen, DEPART
//   Achievements  RetroAchievements progress for MHFU (game 3732), by Web API key
//   Keyboard      key bindings (work/launcher/input.ini)
//   Controller    the connected controllers, which one the game uses, a tester
//   Advanced      updates, closing on start, extra environment and arguments, files
//   Log           the running game's output (work/run.log)
//   About         who maintains it, what it is built on
//
// The look borrows from Monster Hunter's own menus: dark leather and wood,
// gold-trimmed panels with diamond studs, a quest-board "Depart". The banner
// and icon are PIC1.PNG and ICON0.PNG from the player's own extracted disc
// (work/root/disc/PSP_GAME); nothing of the game ships here.
#include "config.h"
#include "game.h"
#include "image.h"
#include "keys.h"
#include "online.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_opengl.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace fs = std::filesystem;

// ---- look ----------------------------------------------------------------------

static const ImVec4 C_BG     = ImVec4(0.090f, 0.075f, 0.059f, 1.0f);   // dark leather
static const ImVec4 C_SIDE   = ImVec4(0.059f, 0.047f, 0.035f, 1.0f);
static const ImVec4 C_PANEL  = ImVec4(0.133f, 0.106f, 0.082f, 0.94f);  // oiled wood
static const ImVec4 C_FRAME  = ImVec4(0.545f, 0.420f, 0.231f, 1.0f);   // tarnished brass
static const ImVec4 C_GOLD   = ImVec4(0.933f, 0.765f, 0.400f, 1.0f);
static const ImVec4 C_TEXT   = ImVec4(0.988f, 0.969f, 0.929f, 1.0f);
static const ImVec4 C_DIM    = ImVec4(0.792f, 0.741f, 0.651f, 1.0f);
static const ImVec4 C_EMBER  = ImVec4(0.600f, 0.220f, 0.090f, 1.0f);
static const ImVec4 C_EMBER2 = ImVec4(0.800f, 0.360f, 0.130f, 1.0f);
static const ImVec4 C_OK     = ImVec4(0.659f, 0.847f, 0.494f, 1.0f);
static const ImVec4 C_WARN   = ImVec4(0.957f, 0.757f, 0.353f, 1.0f);
static const ImVec4 C_BAD    = ImVec4(0.925f, 0.443f, 0.380f, 1.0f);

static ImFont *g_body, *g_head, *g_title, *g_small, *g_big;

static ImU32 col(const ImVec4 &c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }
static ImVec2 operator+(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
static ImVec2 operator-(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }

static void style() {
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 0; s.ChildRounding = 0; s.FrameRounding = 3; s.GrabRounding = 3; s.PopupRounding = 3;
    s.FramePadding = ImVec2(10, 6); s.ItemSpacing = ImVec2(10, 8); s.WindowBorderSize = 0;
    s.PopupBorderSize = 1; s.FrameBorderSize = 1; s.ScrollbarSize = 12; s.CellPadding = ImVec2(8, 7);
    ImVec4 *c = s.Colors;
    c[ImGuiCol_Text] = C_TEXT;
    c[ImGuiCol_TextDisabled] = C_DIM;
    c[ImGuiCol_WindowBg] = C_BG;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.088f, 0.068f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(C_FRAME.x, C_FRAME.y, C_FRAME.z, 0.55f);
    c[ImGuiCol_FrameBg] = ImVec4(0.075f, 0.060f, 0.047f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.095f, 0.072f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.118f, 0.088f, 1);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.155f, 0.110f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.225f, 0.145f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.38f, 0.28f, 0.17f, 1);
    c[ImGuiCol_Header] = ImVec4(0.25f, 0.19f, 0.12f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.32f, 0.24f, 0.15f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.38f, 0.28f, 0.17f, 1);
    c[ImGuiCol_CheckMark] = C_GOLD;
    c[ImGuiCol_SliderGrab] = C_GOLD;
    c[ImGuiCol_PlotHistogram] = C_GOLD;
    c[ImGuiCol_Separator] = ImVec4(C_FRAME.x, C_FRAME.y, C_FRAME.z, 0.35f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 0.9f, 0.7f, 0.035f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(C_FRAME.x, C_FRAME.y, C_FRAME.z, 0.5f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.65f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(C_GOLD.x, C_GOLD.y, C_GOLD.z, 0.3f);
}

static void fonts() {
    ImGuiIO &io = ImGui::GetIO();
    auto add = [&](const char *path, float px) -> ImFont * {
        return file_exists(path) ? io.Fonts->AddFontFromFileTTF(path, px) : nullptr;
    };
    g_body = add("C:/Windows/Fonts/segoeui.ttf", 17.5f);
    if (!g_body) g_body = io.Fonts->AddFontDefault();
    g_small = add("C:/Windows/Fonts/segoeui.ttf", 15.0f);
    const char *serif_b = file_exists("C:/Windows/Fonts/georgiab.ttf") ? "C:/Windows/Fonts/georgiab.ttf"
                                                                      : "C:/Windows/Fonts/georgia.ttf";
    g_head = add(serif_b, 21.0f);
    g_title = add(serif_b, 30.0f);
    g_big = add(serif_b, 34.0f);
    if (!g_small) g_small = g_body;
    if (!g_head) g_head = g_body;
    if (!g_title) g_title = g_head;
    if (!g_big) g_big = g_title;
}

// ---- textures -----------------------------------------------------------------

struct Tex { GLuint id = 0; int w = 0, h = 0; };
static std::map<std::string, Tex> g_tex;

static Tex texture(const std::string &path) {
    if (path.empty()) return {};
    auto it = g_tex.find(path);
    if (it != g_tex.end()) return it->second;
    Tex t;
    std::vector<uint8_t> px;
    if (file_exists(path) && image_load(path, px, t.w, t.h)) {
        glGenTextures(1, &t.id);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    }
    g_tex[path] = t;
    return t;
}

static ImTextureID tid(const Tex &t) { return (ImTextureID)(intptr_t)t.id; }

// ---- ornaments ---------------------------------------------------------------------

static void diamond(ImDrawList *dl, ImVec2 c, float r, ImU32 color, bool filled = true) {
    const ImVec2 p[4] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y)};
    if (filled) dl->AddConvexPolyFilled(p, 4, color);
    else dl->AddPolyline(p, 4, color, ImDrawFlags_Closed, 1.3f);
}

// A gold-trimmed panel: wood fill, a double brass border, diamond studs.
static void ornate(ImDrawList *dl, ImVec2 a, ImVec2 b, bool fill = true) {
    if (fill) dl->AddRectFilledMultiColor(a, b, col(C_PANEL), col(C_PANEL), col(C_PANEL, 0.80f), col(C_PANEL, 0.80f));
    dl->AddRect(a, b, col(C_FRAME), 0, 0, 1.6f);
    dl->AddRect(a + ImVec2(5, 5), b - ImVec2(5, 5), col(C_FRAME, 0.35f), 0, 0, 1.0f);
    const ImVec2 cs[4] = {a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y)};
    for (auto &c : cs) { diamond(dl, c, 6, col(C_SIDE)); diamond(dl, c, 5, col(C_GOLD)); }
}

// A section heading: gold serif, then a rule that ends in a stud.
static void heading(const char *text) {
    ImGui::PushFont(g_head);
    ImGui::TextColored(C_GOLD, "%s", text);
    ImGui::PopFont();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float y = (a.y + b.y) * 0.5f + 2, x0 = b.x + 12, x1 = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 22;
    if (x1 > x0 + 20) {
        dl->AddLine(ImVec2(x0, y), ImVec2(x1 - 8, y), col(C_FRAME, 0.6f), 1.0f);
        diamond(dl, ImVec2(x1, y), 4, col(C_GOLD, 0.8f));
    }
    ImGui::Spacing();
}

// A page title over a framed rule: "◆──── Keyboard ────◆".
static void page_title(const char *text, const char *sub) {
    ImGui::PushFont(g_title);
    ImGui::TextColored(C_TEXT, "%s", text);
    ImGui::PopFont();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    dl->AddLine(ImVec2(p.x, p.y + 2), ImVec2(p.x + w, p.y + 2), col(C_FRAME, 0.7f), 1.2f);
    diamond(dl, ImVec2(p.x, p.y + 2), 4, col(C_GOLD));
    diamond(dl, ImVec2(p.x + w, p.y + 2), 4, col(C_GOLD));
    diamond(dl, ImVec2(p.x + 90, p.y + 2), 3, col(C_GOLD, 0.8f));
    ImGui::Dummy(ImVec2(w, 10));
    if (sub) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_DIM);
        ImGui::TextWrapped("%s", sub);
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
}

// A framed child; EndChild closes it.
static void begin_panel(const char *id, ImVec2 size) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (size.x <= 0) size.x = avail.x + size.x;
    if (size.y <= 0) size.y = avail.y + size.y;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ornate(ImGui::GetWindowDrawList(), p, p + size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 18));
    ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
}

static void dim(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, C_DIM);
    ImGui::TextWrappedV(fmt, ap);
    ImGui::PopStyleColor();
    va_end(ap);
}

// A check mark, an exclamation mark or a cross, drawn (the fonts lack them).
static void mark(int kind) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float s = ImGui::GetTextLineHeight();
    const ImU32 c = col(kind == 0 ? C_OK : kind == 1 ? C_WARN : C_BAD);
    diamond(dl, ImVec2(p.x + s * 0.5f, p.y + s * 0.52f), s * 0.52f, col(kind == 0 ? C_OK : kind == 1 ? C_WARN : C_BAD, 0.16f));
    if (kind == 0) {
        dl->AddLine(ImVec2(p.x + s * 0.25f, p.y + s * 0.55f), ImVec2(p.x + s * 0.45f, p.y + s * 0.75f), c, 2.2f);
        dl->AddLine(ImVec2(p.x + s * 0.45f, p.y + s * 0.75f), ImVec2(p.x + s * 0.80f, p.y + s * 0.30f), c, 2.2f);
    } else if (kind == 1) {
        dl->AddLine(ImVec2(p.x + s * 0.5f, p.y + s * 0.25f), ImVec2(p.x + s * 0.5f, p.y + s * 0.60f), c, 2.4f);
        dl->AddCircleFilled(ImVec2(p.x + s * 0.5f, p.y + s * 0.77f), 1.6f, c);
    } else {
        dl->AddLine(ImVec2(p.x + s * 0.30f, p.y + s * 0.32f), ImVec2(p.x + s * 0.70f, p.y + s * 0.72f), c, 2.2f);
        dl->AddLine(ImVec2(p.x + s * 0.70f, p.y + s * 0.32f), ImVec2(p.x + s * 0.30f, p.y + s * 0.72f), c, 2.2f);
    }
    ImGui::Dummy(ImVec2(s, s));
}

// The quest board's button: an ember plate in a brass frame.
static bool depart_button(const char *label, const char *sub, ImVec2 size, bool enabled) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool hit = ImGui::InvisibleButton("depart", size);
    ImGui::EndDisabled();
    const bool hov = enabled && ImGui::IsItemHovered(), held = enabled && ImGui::IsItemActive();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 b = p + size;
    const ImVec4 top = enabled ? (hov ? C_EMBER2 : C_EMBER) : ImVec4(0.22f, 0.18f, 0.14f, 1);
    const ImVec4 bot = enabled ? ImVec4(top.x * 0.55f, top.y * 0.45f, top.z * 0.45f, 1) : ImVec4(0.14f, 0.11f, 0.09f, 1);
    dl->AddRectFilledMultiColor(p, b, col(top), col(top), col(bot), col(bot));
    if (held) dl->AddRectFilled(p, b, IM_COL32(0, 0, 0, 40));
    dl->AddRect(p, b, col(enabled ? C_GOLD : C_FRAME), 0, 0, 2.0f);
    dl->AddRect(p + ImVec2(5, 5), b - ImVec2(5, 5), col(C_GOLD, enabled ? 0.45f : 0.2f), 0, 0, 1.0f);
    diamond(dl, ImVec2(p.x, (p.y + b.y) / 2), 8, col(C_SIDE));
    diamond(dl, ImVec2(p.x, (p.y + b.y) / 2), 6, col(C_GOLD));
    diamond(dl, ImVec2(b.x, (p.y + b.y) / 2), 8, col(C_SIDE));
    diamond(dl, ImVec2(b.x, (p.y + b.y) / 2), 6, col(C_GOLD));
    const ImVec2 ts = g_big->CalcTextSizeA(g_big->FontSize, FLT_MAX, 0, label);
    const ImVec2 ss = g_small->CalcTextSizeA(g_small->FontSize, FLT_MAX, 0, sub);
    const float y = p.y + (size.y - ts.y - ss.y) / 2;
    dl->AddText(g_big, g_big->FontSize, ImVec2(p.x + (size.x - ts.x) / 2 + 1, y + 1), IM_COL32(0, 0, 0, 140), label);
    dl->AddText(g_big, g_big->FontSize, ImVec2(p.x + (size.x - ts.x) / 2, y), col(enabled ? C_TEXT : C_DIM), label);
    dl->AddText(g_small, g_small->FontSize, ImVec2(p.x + (size.x - ss.x) / 2, y + ts.y), col(C_GOLD, enabled ? 0.9f : 0.5f), sub);
    return hit;
}

// ---- state ---------------------------------------------------------------------

enum Page { PLAY, ACHIEVEMENTS, KEYBOARD, CONTROLLER, ADVANCED, LOG, ABOUT, NPAGES };
static const char *k_pages[NPAGES] = {"Play", "Achievements", "Keyboard", "Controller", "Advanced", "Log", "About"};

static Config g_cfg;
static bool g_dirty;
static Page g_page = PLAY;
static std::vector<Check> g_checks;
static bool g_recheck = true;
static std::string g_gpu, g_status;
static std::vector<Binding> g_keys;
static int g_capture = -1;
static std::string g_log;
static bool g_autoscroll = true;
static std::string g_ra_user_in, g_ra_key_in;
static int g_ra_filter;
static SDL_Window *g_window;
static bool g_quit;
static std::map<SDL_JoystickID, SDL_Gamepad *> g_pads;
static SDL_JoystickID g_pad_sel;

static std::string keys_path() { return (fs::u8path(paths::launcher()) / "input.ini").u8string(); }
static std::string disc_file(const char *name) {
    return (fs::u8path(paths::profile_root("Default")) / "disc" / "PSP_GAME" / name).u8string();
}
static void changed() { g_dirty = true; }

// The file dialog answers on whichever thread SDL likes.
static std::mutex g_pick_lock;
static std::string g_pick_result;
static int g_pick_target;           // 1 ISO, 2 EBOOT
static void SDLCALL picked(void *, const char *const *files, int) {
    std::lock_guard<std::mutex> l(g_pick_lock);
    if (files && files[0]) g_pick_result = files[0];
}
static void pick(int target, const char *name, const char *pattern) {
    static SDL_DialogFileFilter f[2];
    f[0] = {name, pattern};
    f[1] = {"All files", "*"};
    g_pick_target = target;
    SDL_ShowOpenFileDialog(picked, nullptr, g_window, f, 2, nullptr, false);
}

static std::string pad_guid(SDL_JoystickID id) {
    char g[64];
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), g, sizeof g);
    return g;
}

// ---- sidebar ----------------------------------------------------------------------

static const float SIDE_W = 240;

static void sidebar(float h) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    dl->AddRectFilledMultiColor(o, o + ImVec2(SIDE_W, h), col(C_SIDE), col(ImVec4(0.075f, 0.058f, 0.043f, 1)),
                                col(ImVec4(0.075f, 0.058f, 0.043f, 1)), col(C_SIDE));
    dl->AddLine(o + ImVec2(SIDE_W - 1, 0), o + ImVec2(SIDE_W - 1, h), col(C_FRAME, 0.8f), 1.5f);
    dl->AddLine(o + ImVec2(SIDE_W - 5, 0), o + ImVec2(SIDE_W - 5, h), col(C_FRAME, 0.25f), 1.0f);

    ImGui::BeginChild("side", ImVec2(SIDE_W, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    // The crest: the title between two studded rules, and who made it.
    const ImVec2 c = ImGui::GetCursorScreenPos();
    dl = ImGui::GetWindowDrawList();
    auto rule = [&](float y) {
        dl->AddLine(ImVec2(c.x + 22, y), ImVec2(c.x + SIDE_W - 26, y), col(C_FRAME, 0.7f), 1.0f);
        diamond(dl, ImVec2(c.x + SIDE_W / 2 - 2, y), 4, col(C_GOLD));
    };
    rule(c.y + 20);
    ImGui::SetCursorPos(ImVec2(0, 32));
    auto centred = [&](ImFont *f, const ImVec4 &cl, const char *t) {
        ImGui::PushFont(f);
        ImGui::SetCursorPosX((SIDE_W - 4 - ImGui::CalcTextSize(t).x) / 2);
        ImGui::TextColored(cl, "%s", t);
        ImGui::PopFont();
    };
    centred(g_small, C_DIM, "MONSTER HUNTER");
    centred(g_title, C_GOLD, "FREEDOM");
    centred(g_title, C_GOLD, "UNITE");
    rule(ImGui::GetCursorScreenPos().y + 4);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 14);
    centred(g_body, C_TEXT, "by PateeerQ");
    centred(g_small, C_DIM, "native recomp \xC2\xB7 Windows \xC2\xB7 v" LAUNCHER_VERSION);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 16);

    for (int i = 0; i < NPAGES; i++) {
        const bool sel = g_page == i;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("nav", ImVec2(SIDE_W - 6, 44))) g_page = (Page)i;
        const bool hov = ImGui::IsItemHovered();
        if (sel)
            dl->AddRectFilledMultiColor(p, p + ImVec2(SIDE_W - 6, 44), col(C_GOLD, 0.20f), col(C_GOLD, 0.0f),
                                        col(C_GOLD, 0.0f), col(C_GOLD, 0.20f));
        else if (hov)
            dl->AddRectFilledMultiColor(p, p + ImVec2(SIDE_W - 6, 44), col(C_GOLD, 0.08f), col(C_GOLD, 0.0f),
                                        col(C_GOLD, 0.0f), col(C_GOLD, 0.08f));
        if (sel) dl->AddLine(p + ImVec2(0, 4), p + ImVec2(0, 40), col(C_GOLD), 3.0f);
        diamond(dl, p + ImVec2(30, 22), 6, col(sel ? C_GOLD : C_FRAME), sel);
        if (sel) diamond(dl, p + ImVec2(30, 22), 2.5f, col(C_SIDE));
        dl->AddText(p + ImVec2(50, 12), col(sel ? C_GOLD : hov ? C_TEXT : C_DIM), k_pages[i]);
        ImGui::PopID();
    }

    // A newer release, as a framed notice at the bottom.
    const UpdateInfo u = update_info();
    if (u.state == Job::Done && u.newer) {
        ImGui::SetCursorPos(ImVec2(16, h - 140));
        begin_panel("upd", ImVec2(SIDE_W - 34, 124));
        ImGui::TextColored(C_GOLD, "Version %s", u.latest.c_str());
        ImGui::TextColored(C_DIM, "is available.");
        if (ImGui::Button("Update", ImVec2(-1, 0))) open_in_shell(u.url);
        if (ImGui::Button("What's new", ImVec2(-1, 0))) ImGui::OpenPopup("whatsnew");
        if (ImGui::BeginPopupModal("whatsnew", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            heading(u.name.c_str());
            ImGui::BeginChild("notes", ImVec2(560, 320), ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s", u.notes.empty() ? "(no notes)" : u.notes.c_str());
            ImGui::EndChild();
            if (ImGui::Button("Open the release page")) open_in_shell(u.url);
            ImGui::SameLine();
            if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

// ---- the banner ---------------------------------------------------------------------

static void banner(float w) {
    const float h = 200;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const Tex pic = texture(disc_file("PIC1.PNG"));
    if (pic.id) {
        const float scale = std::max(w / pic.w, h / pic.h);
        const float sw = w / (pic.w * scale), sh = h / (pic.h * scale);
        const ImVec2 uv0((1 - sw) / 2, (1 - sh) / 2), uv1(uv0.x + sw, uv0.y + sh);
        dl->AddImage(tid(pic), p, p + ImVec2(w, h), uv0, uv1, IM_COL32(255, 240, 220, 255));
    } else {
        dl->AddRectFilledMultiColor(p, p + ImVec2(w, h), IM_COL32(52, 38, 24, 255), IM_COL32(30, 36, 28, 255),
                                    IM_COL32(18, 14, 10, 255), IM_COL32(18, 14, 10, 255));
    }
    // A warm vignette into the leather below, and a brass edge.
    dl->AddRectFilledMultiColor(ImVec2(p.x, p.y + h * 0.35f), p + ImVec2(w, h), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                col(C_BG), col(C_BG));
    dl->AddRectFilledMultiColor(p, p + ImVec2(w * 0.55f, h), IM_COL32(10, 7, 4, 150), IM_COL32(10, 7, 4, 0),
                                IM_COL32(10, 7, 4, 0), IM_COL32(10, 7, 4, 150));
    dl->AddLine(ImVec2(p.x, p.y + h - 1), ImVec2(p.x + w, p.y + h - 1), col(C_FRAME, 0.5f), 1.0f);

    const Tex icon = texture(disc_file("ICON0.PNG"));
    float tx = p.x + 34;
    if (icon.id) {
        const ImVec2 a(p.x + 34, p.y + h - 104), b(a.x + 144, a.y + 80);
        dl->AddImage(tid(icon), a, b);
        ornate(dl, a - ImVec2(4, 4), b + ImVec2(4, 4), false);
        tx += 172;
    }
    dl->AddText(g_small, g_small->FontSize, ImVec2(tx, p.y + h - 102), col(C_GOLD), "HUNTER'S GUILD  \xC2\xB7  POKKE VILLAGE");
    dl->AddText(g_big, g_big->FontSize, ImVec2(tx + 1, p.y + h - 79), IM_COL32(0, 0, 0, 160), "Monster Hunter Freedom Unite");
    dl->AddText(g_big, g_big->FontSize, ImVec2(tx, p.y + h - 80), col(C_TEXT), "Monster Hunter Freedom Unite");
    dl->AddText(g_body, g_body->FontSize, ImVec2(tx, p.y + h - 40), col(C_DIM), "ULES01213  \xC2\xB7  Europe  \xC2\xB7  PSP");
    ImGui::Dummy(ImVec2(w, h));
}

// ---- Play ---------------------------------------------------------------------------------

static const char *k_scales[] = {"1\xC3\x97 native (480\xC3\x97" "272)", "2\xC3\x97 (960\xC3\x97" "544)",
                                 "3\xC3\x97 (1440\xC3\x97" "816)", "4\xC3\x97 (1920\xC3\x97" "1088)",
                                 "5\xC3\x97 (2400\xC3\x97" "1360)"};
static const char *k_outputs[] = {"960x544", "1280x720", "1440x816", "1600x900", "1920x1080", "2560x1440", "3840x2160"};

static std::string pretty(const std::string &wxh) {
    std::string s = wxh;
    const size_t x = s.find('x');
    if (x != std::string::npos) s.replace(x, 1, " \xC3\x97 ");
    return s;
}

static void profile_row(float width, float lw) {
    static std::string new_name, prof_err;
    const ImGuiStyle &st = ImGui::GetStyle();
    const float bnew = ImGui::CalcTextSize("New").x + st.FramePadding.x * 2,
                bdel = ImGui::CalcTextSize("Delete").x + st.FramePadding.x * 2;
    ImGui::SetNextItemWidth(width - lw - bnew - bdel - st.ItemSpacing.x * 2);
    const auto profs = paths::profiles();
    if (ImGui::BeginCombo("##profile", g_cfg.profile.c_str())) {
        for (auto &p : profs)
            if (ImGui::Selectable(p.c_str(), p == g_cfg.profile)) { g_cfg.profile = p; changed(); g_recheck = true; }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("New")) { new_name.clear(); prof_err.clear(); ImGui::OpenPopup("New profile"); }
    ImGui::SameLine();
    ImGui::BeginDisabled(g_cfg.profile == "Default");
    if (ImGui::Button("Delete")) { prof_err.clear(); ImGui::OpenPopup("Delete profile"); }
    ImGui::EndDisabled();
    if (ImGui::BeginPopupModal("New profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        heading("New hunter profile");
        ImGui::Text("A profile keeps its own saves and multiplayer settings.");
        ImGui::SetNextItemWidth(320);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("Name", &new_name, ImGuiInputTextFlags_EnterReturnsTrue);
        if (!prof_err.empty()) ImGui::TextColored(C_BAD, "%s", prof_err.c_str());
        if (ImGui::Button("Create") || enter) {
            if (paths::create_profile(new_name, prof_err)) {
                g_cfg.profile = new_name; changed(); g_recheck = true; ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Delete profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        heading("Delete profile");
        ImGui::Text("Delete \"%s\" and all of its saves?", g_cfg.profile.c_str());
        ImGui::TextColored(C_WARN, "This cannot be undone.");
        if (!prof_err.empty()) ImGui::TextColored(C_BAD, "%s", prof_err.c_str());
        if (ImGui::Button("Delete")) {
            if (paths::delete_profile(g_cfg.profile, prof_err)) {
                g_cfg.profile = "Default"; changed(); g_recheck = true; ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

static void page_play(float w, float h) {
    banner(w);
    const float pad = 30, bar = 96;
    ImGui::SetCursorPos(ImVec2(pad, ImGui::GetCursorPosY() + 10));
    const float panel_h = h - ImGui::GetCursorPosY() - bar - 14;
    const float total = w - pad * 2, right = std::min(430.0f, total * 0.46f), left = total - right - 22;

    begin_panel("ready", ImVec2(left, panel_h));
    heading("Ready Check");
    for (auto &k : g_checks) {
        mark(k.ok ? (k.warn ? 1 : 0) : (k.warn ? 1 : 2));
        ImGui::SameLine(0, 10);
        ImGui::BeginGroup();
        ImGui::Text("%s", k.label.c_str());
        ImGui::PushFont(g_small);
        dim("%s", k.detail.c_str());
        ImGui::PopFont();
        ImGui::EndGroup();
    }
    ImGui::Spacing();
    if (ImGui::Button("Check again")) g_recheck = true;
    ImGui::EndChild();

    ImGui::SameLine(0, 22);
    begin_panel("quick", ImVec2(right, panel_h));
    heading("Quick Settings");
    const float lw = 118, width = ImGui::GetContentRegionAvail().x;
    auto label = [&](const char *t) { ImGui::AlignTextToFramePadding(); ImGui::Text("%s", t); ImGui::SameLine(lw); };
    label("Profile");
    profile_row(width, lw);
    label("Upscaler");
    ImGui::SetNextItemWidth(width - lw);
    int scale = g_cfg.render_scale - 1;
    if (ImGui::Combo("##scale", &scale, k_scales, IM_ARRAYSIZE(k_scales))) { g_cfg.render_scale = scale + 1; changed(); }
    label("Output");
    ImGui::SetNextItemWidth(width - lw);
    if (ImGui::BeginCombo("##output", pretty(g_cfg.output).c_str())) {
        for (auto *o : k_outputs)
            if (ImGui::Selectable(pretty(o).c_str(), g_cfg.output == o)) { g_cfg.output = o; changed(); }
        ImGui::EndCombo();
    }
    ImGui::SetCursorPosX(lw);
    if (ImGui::Checkbox("Fullscreen", &g_cfg.fullscreen)) changed();
    label("Hunter name");
    ImGui::SetNextItemWidth(width - lw);
    if (ImGui::InputTextWithHint("##nick", "multiplayer name", &g_cfg.nick)) changed();
    ImGui::PushFont(g_small);
    ImGui::SetCursorPosX(lw);
    ImGui::TextColored(C_DIM, "F11 or Alt+Enter: fullscreen in game");
    ImGui::PopFont();
    ImGui::EndChild();

    // The bar: what Depart will do, Stop and DEPART.
    ImGui::SetCursorPos(ImVec2(pad, h - bar));
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddLine(bp - ImVec2(0, 8), bp + ImVec2(total, -8), col(C_FRAME, 0.4f), 1.0f);
    const bool running = game_running();
    ImGui::BeginGroup();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10);
    ImGui::TextColored(C_DIM, "%s  \xC2\xB7  %s  \xC2\xB7  %s  \xC2\xB7  profile %s", k_scales[g_cfg.render_scale - 1],
                       pretty(g_cfg.output).c_str(), g_cfg.fullscreen ? "Fullscreen" : "Windowed", g_cfg.profile.c_str());
    if (!g_status.empty()) ImGui::TextColored(C_WARN, "%s", g_status.c_str());
    else if (running) ImGui::TextColored(C_OK, "On the hunt: the game is running.");
    else if (game_exit_code() > 0) ImGui::TextColored(C_WARN, "The game ended with code %d: see Log.", game_exit_code());
    else if (!ready_to_play(g_checks)) ImGui::TextColored(C_BAD, "Not ready: see the ready check.");
    else ImGui::TextColored(C_OK, "Ready to depart.");
    ImGui::EndGroup();
    const float bw = 250, sw = 110, bh = 66;
    ImGui::SetCursorPos(ImVec2(pad + total - bw - sw - 22, h - bar + 4));
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Stop", ImVec2(sw, bh))) game_stop();
    ImGui::EndDisabled();
    ImGui::SameLine(0, 22);
    if (depart_button(running ? "HUNTING" : "DEPART", running ? "the game is running" : "start the game",
                      ImVec2(bw, bh), !running && ready_to_play(g_checks))) {
        config_save(g_cfg);
        g_dirty = false;
        std::string err;
        if (!game_start(g_cfg, err)) g_status = err;
        else {
            g_status.clear();
            if (g_cfg.close_on_start) g_quit = true;
        }
    }
}

// ---- Achievements --------------------------------------------------------------------------

// A form row: the label on the left, then the field (sized up to 380 px).
static const float FORM_LABEL_W = 140;
static void form_label(const char *label) {
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", label);
    ImGui::SameLine(FORM_LABEL_W + ImGui::GetStyle().WindowPadding.x);
    ImGui::SetNextItemWidth(std::min(380.0f, ImGui::GetContentRegionAvail().x));
}

// Unlocking in game: a login token from the password (asked once, never
// kept), handed to the game as MHFU_RA_USER / MHFU_RA_TOKEN.
static void ingame_panel() {
    static std::string user_in, pass_in;
    const RaLogin lg = ra_login();
    if (lg.state == Job::Done) {
        g_cfg.ra_token = lg.token;
        if (!lg.user.empty()) g_cfg.ra_user = lg.user;
        changed();
        pass_in.clear();
        ra_login_reset();
    }
    begin_panel("ingame", ImVec2(0, g_cfg.ra_token.empty() ? 250 : 150));
    heading("Unlock while playing");
    if (!g_cfg.ra_token.empty()) {
        ImGui::TextColored(C_OK, "Signed in as %s for in-game unlocks (softcore).", g_cfg.ra_user.c_str());
        if (ImGui::Checkbox("Unlock achievements while playing", &g_cfg.ra_ingame)) changed();
        ImGui::SameLine(0, 24);
        if (ImGui::Button("Sign out")) { g_cfg.ra_token.clear(); changed(); }
        ImGui::PushFont(g_small);
        dim("Unlocks show as a toast in the game's corner with a chime. Hardcore is for emulators RetroAchievements has approved.");
        ImGui::PopFont();
    } else {
        if (user_in.empty()) user_in = g_cfg.ra_user;
        form_label("Username");
        ImGui::InputText("##ig_user", &user_in);
        form_label("Password");
        const bool enter = ImGui::InputText("##ig_pass", &pass_in, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + FORM_LABEL_W);
        ImGui::BeginDisabled(lg.state == Job::Busy || user_in.empty() || pass_in.empty());
        if (ImGui::Button("Sign in") || (enter && !pass_in.empty())) ra_login_start(user_in, pass_in);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (lg.state == Job::Busy) ImGui::TextColored(C_DIM, "Signing in...");
        else if (lg.state == Job::Failed) ImGui::TextColored(C_BAD, "%s", lg.error.c_str());
        ImGui::PushFont(g_small);
        dim("The password is sent once to retroachievements.org for a login token and is not kept. The token is "
            "stored in work/launcher/launcher.ini.");
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::Spacing();
}

static void page_achievements(float w, float) {
    ImGui::SetCursorPos(ImVec2(30, 24));
    ImGui::BeginChild("ach", ImVec2(w - 60, -20));
    page_title("Achievements", "From RetroAchievements (game 3732): your account's progress, and unlocking while you "
                               "play once you sign in below.");

    if (g_cfg.ra_user.empty() || g_cfg.ra_key.empty()) {
        ingame_panel();
        begin_panel("connect", ImVec2(0, 230));
        heading("Connect your account");
        form_label("Username");
        ImGui::InputText("##ra_user", &g_ra_user_in);
        form_label("Web API key");
        ImGui::InputText("##ra_key", &g_ra_key_in, ImGuiInputTextFlags_Password);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + FORM_LABEL_W);
        if (ImGui::Button("Connect") && !g_ra_user_in.empty() && !g_ra_key_in.empty()) {
            g_cfg.ra_user = g_ra_user_in;
            g_cfg.ra_key = g_ra_key_in;
            changed();
            ra_fetch_start(g_cfg.ra_user, g_cfg.ra_key);
        }
        ImGui::SameLine();
        if (ImGui::Button("Get your Web API key")) open_in_shell("https://retroachievements.org/settings");
        ImGui::PushFont(g_small);
        dim("Settings \xE2\x86\x92 Keys on retroachievements.org. The key is kept in work/launcher/launcher.ini.");
        ImGui::PopFont();
        ImGui::EndChild();
        ImGui::EndChild();
        return;
    }

    RaGame g = ra_game();
    if (g.state == Job::Idle) { ra_fetch_start(g_cfg.ra_user, g_cfg.ra_key); g = ra_game(); }
    auto toolbar = [&] {
        if (ImGui::Button("Refresh")) ra_fetch_start(g_cfg.ra_user, g_cfg.ra_key);
        ImGui::SameLine();
        if (ImGui::Button("Open on RetroAchievements")) open_in_shell("https://retroachievements.org/game/3732");
        ImGui::SameLine();
        if (ImGui::Button("Disconnect")) {
            g_ra_user_in = g_cfg.ra_user;
            g_cfg.ra_user.clear();
            g_cfg.ra_key.clear();
            g_cfg.ra_token.clear();
            changed();
            ra_forget();
        }
    };
    if (g.state == Job::Busy) { ImGui::Text("Loading %s's hunter record...", g_cfg.ra_user.c_str()); ImGui::EndChild(); return; }
    if (g.state == Job::Failed) {
        ImGui::TextColored(C_BAD, "Could not load: %s", g.error.c_str());
        toolbar();
        ImGui::EndChild();
        return;
    }

    begin_panel("summary", ImVec2(0, 150));
    const Tex icon = texture(g.icon_file);
    if (icon.id) { ImGui::Image(tid(icon), ImVec2(80, 80)); ImGui::SameLine(0, 18); }
    ImGui::BeginGroup();
    ImGui::PushFont(g_title);
    ImGui::TextColored(C_GOLD, "%s", g.title.empty() ? "Monster Hunter Freedom Unite" : g.title.c_str());
    ImGui::PopFont();
    ImGui::Text("%d of %d unlocked  \xC2\xB7  %d of %d points", g.unlocked, g.total, g.points_earned, g.points);
    ImGui::TextColored(C_DIM, "Hunter %s  \xC2\xB7  %d in hardcore", g.user.c_str(), g.unlocked_hc);
    ImGui::EndGroup();
    ImGui::ProgressBar(g.total ? (float)g.unlocked / g.total : 0.0f, ImVec2(-1, 6), "");
    ImGui::EndChild();
    ImGui::Spacing();
    ingame_panel();
    toolbar();
    ImGui::SameLine(0, 30);
    ImGui::RadioButton("All", &g_ra_filter, 0); ImGui::SameLine();
    ImGui::RadioButton("Unlocked", &g_ra_filter, 1); ImGui::SameLine();
    ImGui::RadioButton("Locked", &g_ra_filter, 2);
    ImGui::Spacing();

    begin_panel("list", ImVec2(0, 0));
    for (auto &a : g.list) {
        const bool got = !a.earned.empty() || !a.earned_hc.empty();
        if ((g_ra_filter == 1 && !got) || (g_ra_filter == 2 && got)) continue;
        ImGui::PushID(a.id);
        const Tex b = texture(got ? a.badge_file : a.lock_file);
        const ImVec2 bp = ImGui::GetCursorScreenPos();
        if (b.id) ImGui::Image(tid(b), ImVec2(56, 56));
        else ImGui::Dummy(ImVec2(56, 56));
        ImGui::GetWindowDrawList()->AddRect(bp - ImVec2(2, 2), bp + ImVec2(58, 58), col(got ? C_GOLD : C_FRAME, 0.8f));
        ImGui::SameLine(0, 16);
        ImGui::BeginGroup();
        ImGui::TextColored(got ? C_GOLD : C_TEXT, "%s", a.title.c_str());
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 150);
        ImGui::TextColored(C_DIM, "%s", a.description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 130);
        ImGui::BeginGroup();
        ImGui::Text("%d pts", a.points);
        ImGui::PushFont(g_small);
        if (got) ImGui::TextColored(C_OK, "%s", (a.earned_hc.empty() ? a.earned : a.earned_hc).substr(0, 10).c_str());
        else ImGui::TextColored(C_DIM, "locked");
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

// ---- Keyboard ------------------------------------------------------------------------------

// The keys of one binding as keycaps.
static void keycaps(const std::string &keys) {
    if (keys.empty()) { ImGui::AlignTextToFramePadding(); ImGui::TextColored(C_DIM, "(none)"); return; }
    std::istringstream in(keys);
    std::string k;
    bool first = true;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    while (std::getline(in, k, ',')) {
        k.erase(0, k.find_first_not_of(' '));
        while (!k.empty() && k.back() == ' ') k.pop_back();
        if (k.empty()) continue;
        std::string up = k;
        for (auto &ch : up) ch = (char)toupper((unsigned char)ch);
        if (!first) ImGui::SameLine(0, 6);
        first = false;
        const ImVec2 ts = ImGui::CalcTextSize(up.c_str());
        const ImVec2 size(std::max(ts.x + 18, 34.0f), ImGui::GetFrameHeight());
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, p + size, col(ImVec4(0.20f, 0.16f, 0.12f, 1)), 4);
        dl->AddRectFilled(p, p + ImVec2(size.x, size.y - 3), col(ImVec4(0.27f, 0.215f, 0.155f, 1)), 4);
        dl->AddRect(p, p + size, col(C_FRAME, 0.8f), 4);
        dl->AddText(p + ImVec2((size.x - ts.x) / 2, (size.y - 3 - ts.y) / 2), col(C_TEXT), up.c_str());
        ImGui::Dummy(size);
    }
}

static void page_keyboard(float w, float) {
    ImGui::SetCursorPos(ImVec2(30, 24));
    ImGui::BeginChild("kbd", ImVec2(w - 60, -20));
    page_title("Keyboard", "Key bindings for the PSP's controls. A controller keeps working side by side (see "
                           "Controller). F3 shows the FPS line and F11 or Alt+Enter toggles fullscreen; those stay fixed.");
    begin_panel("keys", ImVec2(0, -54));
    heading("Buttons");
    if (ImGui::BeginTable("keys", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthFixed, 190);
        ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, 96);
        ImGui::TableSetupColumn("d", ImGuiTableColumnFlags_WidthFixed, 96);
        for (int i = 0; i < (int)g_keys.size(); i++) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", g_keys[i].label);
            ImGui::TableNextColumn();
            keycaps(g_keys[i].keys);
            ImGui::TableNextColumn();
            if (ImGui::Button("Set...", ImVec2(-1, 0))) { g_capture = i; ImGui::OpenPopup("capture"); }
            ImGui::TableNextColumn();
            if (ImGui::Button("Clear", ImVec2(-1, 0))) { g_keys[i].keys.clear(); keys_save(keys_path(), g_keys); }
            if (g_capture == i && ImGui::BeginPopupModal("capture", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                heading("Bind a key");
                ImGui::Text("Press a key for %s.", g_keys[i].label);
                ImGui::TextColored(C_DIM, "Escape cancels.");
                if (g_capture < 0) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::Spacing();
    if (ImGui::Button("Reset to defaults")) { g_keys = keys_defaults(); keys_save(keys_path(), g_keys); }
    ImGui::SameLine();
    ImGui::TextColored(C_DIM, "Saved to work/launcher/input.ini");
    ImGui::EndChild();
}

// ---- Controller ----------------------------------------------------------------------------

static bool down(SDL_Gamepad *p, SDL_GamepadButton b) { return SDL_GetGamepadButton(p, b); }
static float axis(SDL_Gamepad *p, SDL_GamepadAxis a) { return SDL_GetGamepadAxis(p, a) / 32767.0f; }

// The pad, drawn live: sticks, triggers, shoulders, the d-pad, the face buttons
// (with the PlayStation shapes the PSP's buttons have).
static void draw_pad(SDL_Gamepad *p, ImVec2 o, float w) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float cx = o.x + w / 2, top = o.y + 10;
    const ImU32 line = col(C_FRAME), on = col(C_GOLD), off = col(ImVec4(0.18f, 0.145f, 0.11f, 1));
    // Body.
    dl->AddRectFilled(ImVec2(cx - 250, top + 46), ImVec2(cx + 250, top + 250), col(ImVec4(0.11f, 0.088f, 0.068f, 1)), 70);
    dl->AddRect(ImVec2(cx - 250, top + 46), ImVec2(cx + 250, top + 250), line, 70, 0, 1.5f);
    // Shoulders and triggers.
    auto shoulder = [&](float x0, float x1, bool pressed, const char *t) {
        dl->AddRectFilled(ImVec2(x0, top + 22), ImVec2(x1, top + 40), pressed ? on : off, 6);
        dl->AddRect(ImVec2(x0, top + 22), ImVec2(x1, top + 40), line, 6);
        dl->AddText(g_small, g_small->FontSize, ImVec2((x0 + x1) / 2 - 8, top + 22), col(pressed ? C_SIDE : C_DIM), t);
    };
    shoulder(cx - 230, cx - 130, down(p, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), "L1");
    shoulder(cx + 130, cx + 230, down(p, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), "R1");
    auto trigger = [&](float x, float v, const char *t) {
        const float y0 = top + 60, y1 = top + 230;
        dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + 14, y1), off, 4);
        if (v > 0.01f) dl->AddRectFilled(ImVec2(x, y1 - (y1 - y0) * v), ImVec2(x + 14, y1), on, 4);
        dl->AddRect(ImVec2(x, y0), ImVec2(x + 14, y1), line, 4);
        dl->AddText(g_small, g_small->FontSize, ImVec2(x - 2, y1 + 4), col(C_DIM), t);
    };
    trigger(cx - 290, std::max(0.0f, axis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)), "L2");
    trigger(cx + 276, std::max(0.0f, axis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)), "R2");
    // Sticks.
    auto stick = [&](ImVec2 c, float x, float y, bool pressed) {
        dl->AddCircleFilled(c, 38, off, 32);
        dl->AddCircle(c, 38, line, 32, 1.5f);
        dl->AddLine(c - ImVec2(30, 0), c + ImVec2(30, 0), col(C_FRAME, 0.3f));
        dl->AddLine(c - ImVec2(0, 30), c + ImVec2(0, 30), col(C_FRAME, 0.3f));
        const ImVec2 d = c + ImVec2(x * 26, y * 26);
        dl->AddCircleFilled(d, 13, pressed ? on : col(C_GOLD, 0.55f), 24);
        dl->AddCircle(d, 13, line, 24);
    };
    stick(ImVec2(cx - 160, top + 112), axis(p, SDL_GAMEPAD_AXIS_LEFTX), axis(p, SDL_GAMEPAD_AXIS_LEFTY),
          down(p, SDL_GAMEPAD_BUTTON_LEFT_STICK));
    stick(ImVec2(cx + 80, top + 192), axis(p, SDL_GAMEPAD_AXIS_RIGHTX), axis(p, SDL_GAMEPAD_AXIS_RIGHTY),
          down(p, SDL_GAMEPAD_BUTTON_RIGHT_STICK));
    // D-pad.
    const ImVec2 dp(cx - 80, top + 192);
    auto arm = [&](ImVec2 c, bool pressed) {
        dl->AddRectFilled(c - ImVec2(11, 11), c + ImVec2(11, 11), pressed ? on : off, 3);
        dl->AddRect(c - ImVec2(11, 11), c + ImVec2(11, 11), line, 3);
    };
    arm(dp + ImVec2(0, -24), down(p, SDL_GAMEPAD_BUTTON_DPAD_UP));
    arm(dp + ImVec2(0, 24), down(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN));
    arm(dp + ImVec2(-24, 0), down(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT));
    arm(dp + ImVec2(24, 0), down(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
    // Face buttons, by position, in the PSP's shapes and colours.
    const ImVec2 fc(cx + 160, top + 112);
    auto face = [&](ImVec2 c, bool pressed, int shape, ImVec4 tint) {
        dl->AddCircleFilled(c, 17, pressed ? col(tint) : off, 28);
        dl->AddCircle(c, 17, line, 28, 1.3f);
        const ImU32 s = pressed ? col(C_SIDE) : col(tint);
        if (shape == 0) { dl->AddLine(c - ImVec2(7, 7), c + ImVec2(7, 7), s, 2.4f); dl->AddLine(c + ImVec2(-7, 7), c + ImVec2(7, -7), s, 2.4f); }
        else if (shape == 1) dl->AddCircle(c, 7.5f, s, 20, 2.2f);
        else if (shape == 2) dl->AddRect(c - ImVec2(7, 7), c + ImVec2(7, 7), s, 0, 0, 2.2f);
        else dl->AddTriangle(c + ImVec2(0, -8), c + ImVec2(8, 6), c + ImVec2(-8, 6), s, 2.2f);
    };
    face(fc + ImVec2(0, 34), down(p, SDL_GAMEPAD_BUTTON_SOUTH), 0, ImVec4(0.55f, 0.70f, 0.95f, 1));
    face(fc + ImVec2(34, 0), down(p, SDL_GAMEPAD_BUTTON_EAST), 1, ImVec4(0.95f, 0.45f, 0.45f, 1));
    face(fc + ImVec2(-34, 0), down(p, SDL_GAMEPAD_BUTTON_WEST), 2, ImVec4(0.95f, 0.60f, 0.85f, 1));
    face(fc + ImVec2(0, -34), down(p, SDL_GAMEPAD_BUTTON_NORTH), 3, ImVec4(0.45f, 0.85f, 0.70f, 1));
    // Select, Start, Home.
    auto pill = [&](ImVec2 c, bool pressed, const char *t) {
        dl->AddRectFilled(c - ImVec2(18, 7), c + ImVec2(18, 7), pressed ? on : off, 7);
        dl->AddRect(c - ImVec2(18, 7), c + ImVec2(18, 7), line, 7);
        const ImVec2 ts = g_small->CalcTextSizeA(g_small->FontSize, FLT_MAX, 0, t);
        dl->AddText(g_small, g_small->FontSize, c + ImVec2(-ts.x / 2, 10), col(C_DIM), t);
    };
    pill(ImVec2(cx - 44, top + 96), down(p, SDL_GAMEPAD_BUTTON_BACK), "Select");
    pill(ImVec2(cx + 44, top + 96), down(p, SDL_GAMEPAD_BUTTON_START), "Start");
    dl->AddCircleFilled(ImVec2(cx, top + 150), 12, down(p, SDL_GAMEPAD_BUTTON_GUIDE) ? on : off, 24);
    dl->AddCircle(ImVec2(cx, top + 150), 12, line, 24);
    if (SDL_GamepadHasButton(p, SDL_GAMEPAD_BUTTON_TOUCHPAD)) {
        const bool t = down(p, SDL_GAMEPAD_BUTTON_TOUCHPAD);
        dl->AddRectFilled(ImVec2(cx - 40, top + 58), ImVec2(cx + 40, top + 82), t ? on : off, 4);
        dl->AddRect(ImVec2(cx - 40, top + 58), ImVec2(cx + 40, top + 82), line, 4);
    }
    ImGui::Dummy(ImVec2(w, 290));
}

static const char *conn_name(SDL_JoystickConnectionState s) {
    switch (s) {
    case SDL_JOYSTICK_CONNECTION_WIRED: return "wired";
    case SDL_JOYSTICK_CONNECTION_WIRELESS: return "wireless";
    default: return "unknown";
    }
}

static void page_controller(float w, float) {
    ImGui::SetCursorPos(ImVec2(30, 24));
    ImGui::BeginChild("pad", ImVec2(w - 60, -20));
    page_title("Controller", "DualSense, DualShock and Xbox controllers, over USB or Bluetooth. Buttons go by "
                             "position; the right stick works the d-pad (MHFU's camera).");
    const float left = 300;
    begin_panel("devices", ImVec2(left, 0));
    heading("Connected");
    if (g_pads.empty()) dim("No controller found. Connect one by USB or Bluetooth.");
    for (auto &[id, pad] : g_pads) {
        const char *name = SDL_GetGamepadName(pad);
        if (!g_pads.count(g_pad_sel)) g_pad_sel = id;
        if (ImGui::Selectable(name ? name : "Controller", g_pad_sel == id, 0, ImVec2(0, 30))) g_pad_sel = id;
    }
    ImGui::Spacing();
    heading("Used in game");
    std::string current = g_cfg.gamepad.empty() ? "Automatic (the first connected)"
                          : g_cfg.gamepad == "none" ? "None (keyboard only)"
                                                    : (g_cfg.gamepad_name.empty() ? g_cfg.gamepad : g_cfg.gamepad_name);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##use", current.c_str())) {
        if (ImGui::Selectable("Automatic (the first connected)", g_cfg.gamepad.empty())) { g_cfg.gamepad.clear(); g_cfg.gamepad_name.clear(); changed(); }
        if (ImGui::Selectable("None (keyboard only)", g_cfg.gamepad == "none")) { g_cfg.gamepad = "none"; g_cfg.gamepad_name.clear(); changed(); }
        for (auto &[id, pad] : g_pads) {
            const std::string guid = pad_guid(id);
            const char *name = SDL_GetGamepadName(pad);
            ImGui::PushID((int)id);
            if (ImGui::Selectable(name ? name : guid.c_str(), g_cfg.gamepad == guid)) {
                g_cfg.gamepad = guid;
                g_cfg.gamepad_name = name ? name : "";
                changed();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::PushFont(g_small);
    dim("Takes effect the next time the game starts.");
    ImGui::PopFont();
    ImGui::EndChild();

    ImGui::SameLine(0, 22);
    begin_panel("tester", ImVec2(0, 0));
    heading("Tester");
    auto it = g_pads.find(g_pad_sel);
    if (it == g_pads.end()) {
        dim("Pick a controller on the left to see its buttons and sticks move here.");
    } else {
        SDL_Gamepad *p = it->second;
        const char *name = SDL_GetGamepadName(p);
        int pct = -1;
        const SDL_PowerState power = SDL_GetGamepadPowerInfo(p, &pct);
        ImGui::TextColored(C_GOLD, "%s", name ? name : "Controller");
        ImGui::TextColored(C_DIM, "%s  \xC2\xB7  %s  \xC2\xB7  %04X:%04X%s", SDL_GetGamepadStringForType(SDL_GetGamepadType(p)),
                           conn_name(SDL_GetGamepadConnectionState(p)), SDL_GetGamepadVendor(p), SDL_GetGamepadProduct(p),
                           power == SDL_POWERSTATE_ON_BATTERY && pct >= 0 ? ("  \xC2\xB7  battery " + std::to_string(pct) + "%").c_str() : "");
        draw_pad(p, ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x);
        if (ImGui::Button("Rumble")) SDL_RumbleGamepad(p, 0xC000, 0xC000, 600);
        ImGui::SameLine();
        ImGui::TextColored(C_DIM, "LX %+.2f  LY %+.2f   RX %+.2f  RY %+.2f   L2 %.2f  R2 %.2f", axis(p, SDL_GAMEPAD_AXIS_LEFTX),
                           axis(p, SDL_GAMEPAD_AXIS_LEFTY), axis(p, SDL_GAMEPAD_AXIS_RIGHTX), axis(p, SDL_GAMEPAD_AXIS_RIGHTY),
                           axis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), axis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        ImGui::Spacing();
        ImGui::PushFont(g_small);
        dim("In game: the bottom/right/left/top face buttons are Cross/Circle/Square/Triangle, L1 or L2 is L, R1 or R2 "
            "is R, Start is Start, Select/Back/Create is Select, the left stick is the analog stick, the d-pad and the "
            "right stick are the d-pad.");
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

// ---- Advanced ------------------------------------------------------------------------------

static void page_advanced(float w, float) {
    ImGui::SetCursorPos(ImVec2(30, 24));
    ImGui::BeginChild("adv", ImVec2(w - 60, -20));
    page_title("Advanced", nullptr);
    begin_panel("advp", ImVec2(0, 0));

    heading("Updates");
    if (ImGui::Checkbox("Check for updates when the launcher starts", &g_cfg.check_updates)) changed();
    ImGui::TextColored(C_DIM, "Releases from github.com/%s", UPDATE_REPO);
    if (ImGui::Button("Check now")) update_check_start();
    ImGui::SameLine();
    const UpdateInfo u = update_info();
    if (u.state == Job::Busy) ImGui::TextColored(C_DIM, "Checking...");
    else if (u.state == Job::Failed) ImGui::TextColored(C_WARN, "Could not check: %s", u.error.c_str());
    else if (u.state == Job::Done)
        ImGui::TextColored(u.newer ? C_GOLD : C_OK, u.newer ? "Version %s is available (this is %s)." : "Up to date (%s; this is %s).",
                           u.latest.c_str(), LAUNCHER_VERSION);
    else ImGui::TextColored(C_DIM, "This is version %s.", LAUNCHER_VERSION);
    ImGui::Spacing();

    heading("Launcher");
    if (ImGui::Checkbox("Close the launcher when the game starts", &g_cfg.close_on_start)) changed();
    ImGui::Spacing();

    heading("Game");
    ImGui::Text("Extra environment variables, one KEY=VALUE per line");
    if (ImGui::InputTextMultiline("##env", &g_cfg.extra_env, ImVec2(-1, 100))) changed();
    ImGui::PushFont(g_small);
    dim("For example MHFU_MOVIE_SYNC_MS=1250 (movie sound timing) or PSPRECOMP_TRACE_IO=1 (log every file the game opens).");
    ImGui::PopFont();
    ImGui::SetNextItemWidth(-190);
    if (ImGui::InputText("Extra arguments", &g_cfg.extra_args)) changed();
    ImGui::PushFont(g_small);
    dim("Passed to mhfu_host, e.g. --net-create --net-pass 1234.");
    ImGui::PopFont();
    ImGui::Spacing();

    heading("Files");
    ImGui::SetNextItemWidth(-190);
    const std::string iso_hint = paths::iso(Config{});
    if (ImGui::InputTextWithHint("##iso", iso_hint.empty() ? "the game's .iso" : iso_hint.c_str(), &g_cfg.iso)) { changed(); g_recheck = true; }
    ImGui::SameLine();
    if (ImGui::Button("Browse##iso")) pick(1, "PSP disc image", "iso;cso");
    ImGui::SameLine(); ImGui::Text("Game dump");
    ImGui::SetNextItemWidth(-190);
    if (ImGui::InputTextWithHint("##eboot", paths::eboot(Config{}).c_str(), &g_cfg.eboot)) { changed(); g_recheck = true; }
    ImGui::SameLine();
    if (ImGui::Button("Browse##eboot")) pick(2, "Decrypted EBOOT", "BIN;bin");
    ImGui::SameLine(); ImGui::Text("Decrypted EBOOT");
    ImGui::TextColored(C_DIM, "Game folder: %s", paths::base().c_str());
    if (ImGui::Button("Open the game folder")) open_in_shell(paths::work());
    ImGui::SameLine();
    if (ImGui::Button("Open this profile's saves"))
        open_in_shell((fs::u8path(paths::profile_root(g_cfg.profile)) / "ms" / "PSP" / "SAVEDATA").u8string());
    ImGui::EndChild();
    ImGui::EndChild();
}

// ---- Log -------------------------------------------------------------------------------------

static void page_about(float w, float) {
    ImGui::SetCursorPos(ImVec2(30, 24));
    ImGui::BeginChild("about", ImVec2(w - 60, -20));
    page_title("About", nullptr);
    begin_panel("aboutp", ImVec2(0, 0));
    const Tex crest = texture((fs::u8path(paths::exe_dir()) / "mhfu.png").u8string());
    if (crest.id) {
        ImGui::Image((ImTextureID)(intptr_t)crest.id, ImVec2(128, 128));
        ImGui::SameLine(0, 24);
    }
    ImGui::BeginGroup();
    ImGui::PushFont(g_title);
    ImGui::TextColored(C_GOLD, "%s", MHFU_APP_NAME);
    ImGui::PopFont();
    ImGui::TextColored(C_TEXT, "Version %s  \xC2\xB7  Windows", MHFU_VERSION);
    ImGui::Spacing();
    ImGui::PushFont(g_big);
    ImGui::TextColored(C_TEXT, "Maintained by %s", MHFU_MAINTAINER);
    ImGui::PopFont();
    if (ImGui::Button("github.com/" MHFU_REPO)) open_in_shell(MHFU_REPO_URL);
    ImGui::EndGroup();
    ImGui::Spacing();
    heading("What it is");
    dim("A native port of Monster Hunter Freedom Unite (PSP, ULES01213): the game's own code, statically "
        "recompiled to x86-64 with psprecomp, running on a reimplementation of the PSP's firmware. No "
        "emulator in between.");
    heading("Built on");
    dim("psprecomp (sp00nznet) \xC2\xB7 SDL3 \xC2\xB7 FFmpeg (LGPL) \xC2\xB7 rcheevos \xC2\xB7 Dear ImGui \xC2\xB7 "
        "nlohmann/json \xC2\xB7 stb. PPSSPP's source was the reference for the firmware's behaviour.");
    heading("Notice");
    dim("Not affiliated with or endorsed by Capcom. Monster Hunter is a trademark of Capcom. Play with your "
        "own copy of the game: no game files come with this program.");
    ImGui::EndChild();
    ImGui::EndChild();
}

static void page_log(float w, float) {
    ImGui::SetCursorPos(ImVec2(30, 24));
    ImGui::BeginChild("logp", ImVec2(w - 60, -20));
    page_title("Log", nullptr);
    if (game_running()) ImGui::TextColored(C_OK, "The game is running. Its output goes to work/run.log.");
    else if (game_exit_code() >= 0)
        ImGui::TextColored(C_DIM, "The game ended with code %d. Each run is kept in work/logs.", game_exit_code());
    else ImGui::TextColored(C_DIM, "Nothing has run yet. Each run is kept in work/logs.");
    if (ImGui::Button("Copy")) ImGui::SetClipboardText(g_log.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Clear")) g_log.clear();
    ImGui::SameLine();
    if (ImGui::Button("Open the logs folder")) open_in_shell((fs::u8path(paths::work()) / "logs").u8string());
    // The last crash's log, for a report: copied whole (Discord and GitHub take a long paste as a file).
    static std::string crash_note;
    if (file_exists(crash_log_path())) {
        ImGui::SameLine();
        if (ImGui::Button("Copy crash report")) {
            ImGui::SetClipboardText(crash_log_text().c_str());
            crash_note = "Crash report copied: paste it into your bug report.";
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &g_autoscroll);
    if (!crash_note.empty()) ImGui::TextColored(C_OK, "%s", crash_note.c_str());
    else if (game_exit_code() > 0 && file_exists(crash_log_path()))
        ImGui::TextColored(C_WARN, "The game closed because of an error. \"Copy crash report\" puts its log on the clipboard.");
    ImGui::Spacing();

    const ImVec2 p = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    ornate(ImGui::GetWindowDrawList(), p, p + avail);
    ImGui::SetCursorScreenPos(p + ImVec2(10, 10));
    ImGui::BeginChild("logtext", avail - ImVec2(20, 20), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(g_small);
    // Line by line through a clipper: a long run's log is many thousand lines.
    static std::vector<int> starts;
    static size_t indexed;
    if (indexed > g_log.size()) { starts.clear(); indexed = 0; }
    if (starts.empty()) starts.push_back(0);
    for (; indexed < g_log.size(); indexed++) if (g_log[indexed] == '\n') starts.push_back((int)indexed + 1);
    ImGuiListClipper clip;
    clip.Begin((int)starts.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const char *b = g_log.c_str() + starts[i];
            const char *e = i + 1 < (int)starts.size() ? g_log.c_str() + starts[i + 1] - 1 : g_log.c_str() + g_log.size();
            ImGui::TextUnformatted(b, e);
        }
    ImGui::PopFont();
    if (g_autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::EndChild();
}

// ---- main -----------------------------------------------------------------------------------

int main(int, char **) {
    paths::init(SDL_GetBasePath());
    config_load(g_cfg);
    g_ra_user_in = g_cfg.ra_user;
    g_keys = keys_load(keys_path());
    if (!file_exists(keys_path())) keys_save(keys_path(), g_keys);

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");          // the crest (assets/icon/mhfu.rc)
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_window = SDL_CreateWindow("Monster Hunter Freedom Unite", 1200, 780,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!g_window) return 1;
    SDL_SetWindowMinimumSize(g_window, 1060, 680);
    SDL_GLContext gl = SDL_GL_CreateContext(g_window);
    SDL_GL_MakeCurrent(g_window, gl);
    SDL_GL_SetSwapInterval(1);
    SDL_ShowWindow(g_window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    style();
    fonts();
    ImGui_ImplSDL3_InitForOpenGL(g_window, gl);
    ImGui_ImplOpenGL3_Init("#version 130");

    const char *renderer = (const char *)glGetString(GL_RENDERER);
    g_gpu = renderer ? renderer : "";

    // The window's icon: the game's own ICON0, from the player's disc.
    {
        std::vector<uint8_t> px;
        int iw, ih;
        if (image_load(disc_file("ICON0.PNG"), px, iw, ih)) {
            SDL_Surface *s = SDL_CreateSurfaceFrom(iw, ih, SDL_PIXELFORMAT_RGBA32, px.data(), iw * 4);
            if (s) { SDL_SetWindowIcon(g_window, s); SDL_DestroySurface(s); }
        }
    }

    if (g_cfg.check_updates) update_check_start();

    // MHFU_LAUNCHER_SHOT=file.png (and MHFU_LAUNCHER_PAGE=0-5): save the
    // launcher's own frame after a moment, then quit -- for checking the look.
    const char *shot = SDL_getenv("MHFU_LAUNCHER_SHOT");
    if (const char *pg = SDL_getenv("MHFU_LAUNCHER_PAGE")) g_page = (Page)std::clamp(atoi(pg), 0, NPAGES - 1);
    int frames = 0;

    uint64_t last_save = SDL_GetTicks();
    while (!g_quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (g_capture >= 0 && ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.scancode == SDL_SCANCODE_ESCAPE) g_capture = -1;
                else {
                    const std::string n = key_name(ev.key.scancode);
                    if (!n.empty()) {
                        g_keys[g_capture].keys = n;
                        keys_save(keys_path(), g_keys);
                        g_capture = -1;
                    }
                }
                continue;                     // not to ImGui: Enter would press a button
            }
            if (ev.type == SDL_EVENT_GAMEPAD_ADDED) {
                if (SDL_Gamepad *p = SDL_OpenGamepad(ev.gdevice.which)) g_pads[ev.gdevice.which] = p;
            } else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
                auto it = g_pads.find(ev.gdevice.which);
                if (it != g_pads.end()) { SDL_CloseGamepad(it->second); g_pads.erase(it); }
            }
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) g_quit = true;
        }
        if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) { SDL_Delay(50); continue; }
        // In the background (the game in front): about 10 frames a second is plenty.
        if (!shot && !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS)) SDL_Delay(90);

        if (g_recheck) { g_checks = ready_check(g_cfg, g_gpu); g_recheck = false; }
        {
            std::lock_guard<std::mutex> l(g_pick_lock);
            if (!g_pick_result.empty()) {
                (g_pick_target == 1 ? g_cfg.iso : g_cfg.eboot) = g_pick_result;
                g_pick_result.clear();
                changed();
                g_recheck = true;
            }
        }
        bool reset = false;
        const std::string more = game_log_poll(reset);
        if (reset) g_log.clear();
        g_log += more;
        if (g_log.size() > (8u << 20)) g_log.erase(0, g_log.size() - (6u << 20));
        if (g_dirty && SDL_GetTicks() - last_save > 1000) { config_save(g_cfg); g_dirty = false; last_save = SDL_GetTicks(); }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar();
        ImGui::PushFont(g_body);
        sidebar(vp->Size.y);
        ImGui::SameLine(0, 0);
        const float cw = vp->Size.x - SIDE_W, ch = vp->Size.y;
        ImGui::BeginChild("content", ImVec2(cw, ch), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        switch (g_page) {
        case PLAY: page_play(cw, ch); break;
        case ACHIEVEMENTS: page_achievements(cw, ch); break;
        case KEYBOARD: page_keyboard(cw, ch); break;
        case CONTROLLER: page_controller(cw, ch); break;
        case ADVANCED: page_advanced(cw, ch); break;
        case ABOUT: page_about(cw, ch); break;
        default: page_log(cw, ch); break;
        }
        ImGui::EndChild();
        ImGui::PopFont();
        ImGui::End();

        ImGui::Render();
        int fw, fh;
        SDL_GetWindowSizeInPixels(g_window, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(C_BG.x, C_BG.y, C_BG.z, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (shot && ++frames == 90) {
            std::vector<uint8_t> px((size_t)fw * fh * 4), flip(px.size());
            glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (int y = 0; y < fh; y++) memcpy(&flip[(size_t)y * fw * 4], &px[(size_t)(fh - 1 - y) * fw * 4], (size_t)fw * 4);
            image_save_png(shot, flip.data(), fw, fh);
            g_quit = true;
        }
        SDL_GL_SwapWindow(g_window);
    }

    if (g_dirty) config_save(g_cfg);
    for (auto &[id, p] : g_pads) SDL_CloseGamepad(p);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
