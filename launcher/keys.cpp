// FreedomUniteRecomp launcher -- see keys.h.
#include "keys.h"
#include "config.h"

#include <sstream>

std::vector<Binding> keys_defaults() {
    return {
        {"cross", "Cross", "k, space"},        {"circle", "Circle", "l"},
        {"square", "Square", "j"},             {"triangle", "Triangle", "i"},
        {"l", "L", "q"},                       {"r", "R", "e"},
        {"start", "Start", "enter"},           {"select", "Select", "v, backspace"},
        {"up", "D-pad up", "up"},              {"down", "D-pad down", "down"},
        {"left", "D-pad left", "left"},        {"right", "D-pad right", "right"},
        {"stick_up", "Analog up", "w"},        {"stick_down", "Analog down", "s"},
        {"stick_left", "Analog left", "a"},    {"stick_right", "Analog right", "d"},
    };
}

static std::string trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}

std::vector<Binding> keys_load(const std::string &path) {
    std::vector<Binding> b = keys_defaults();
    std::istringstream in(read_text(path));
    std::string line;
    while (std::getline(in, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        for (auto &x : b) if (k == x.action) x.keys = v;
    }
    return b;
}

bool keys_save(const std::string &path, const std::vector<Binding> &b) {
    std::ostringstream o;
    o << "# MHFU keyboard bindings: action = key[, key...]. Written by the launcher;\n"
         "# key names are listed in host/keys.h. Controllers work alongside.\n";
    for (auto &x : b) o << x.action << " = " << x.keys << "\n";
    return write_text(path, o.str());
}

std::string key_name(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return std::string(1, (char)('a' + (sc - SDL_SCANCODE_A)));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return std::string(1, (char)('1' + (sc - SDL_SCANCODE_1)));
    if (sc == SDL_SCANCODE_0) return "0";
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) {
        const int n = 1 + (sc - SDL_SCANCODE_F1);
        if (n == 3 || n == 11) return "";                // the window's own: FPS line, fullscreen
        return "f" + std::to_string(n);
    }
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return "kp" + std::to_string(1 + (sc - SDL_SCANCODE_KP_1));
    switch (sc) {
    case SDL_SCANCODE_KP_0: return "kp0";
    case SDL_SCANCODE_SPACE: return "space";
    case SDL_SCANCODE_RETURN: return "enter";
    case SDL_SCANCODE_BACKSPACE: return "backspace";
    case SDL_SCANCODE_TAB: return "tab";
    case SDL_SCANCODE_CAPSLOCK: return "capslock";
    case SDL_SCANCODE_LSHIFT: return "lshift";
    case SDL_SCANCODE_RSHIFT: return "rshift";
    case SDL_SCANCODE_LCTRL: return "lctrl";
    case SDL_SCANCODE_RCTRL: return "rctrl";
    case SDL_SCANCODE_LALT: return "lalt";
    case SDL_SCANCODE_RALT: return "ralt";
    case SDL_SCANCODE_UP: return "up";
    case SDL_SCANCODE_DOWN: return "down";
    case SDL_SCANCODE_LEFT: return "left";
    case SDL_SCANCODE_RIGHT: return "right";
    case SDL_SCANCODE_INSERT: return "insert";
    case SDL_SCANCODE_DELETE: return "delete";
    case SDL_SCANCODE_HOME: return "home";
    case SDL_SCANCODE_END: return "end";
    case SDL_SCANCODE_PAGEUP: return "pageup";
    case SDL_SCANCODE_PAGEDOWN: return "pagedown";
    case SDL_SCANCODE_COMMA: return "comma";
    case SDL_SCANCODE_PERIOD: return "period";
    case SDL_SCANCODE_SLASH: return "slash";
    case SDL_SCANCODE_SEMICOLON: return "semicolon";
    case SDL_SCANCODE_APOSTROPHE: return "quote";
    case SDL_SCANCODE_MINUS: return "minus";
    case SDL_SCANCODE_EQUALS: return "equals";
    case SDL_SCANCODE_LEFTBRACKET: return "lbracket";
    case SDL_SCANCODE_RIGHTBRACKET: return "rbracket";
    case SDL_SCANCODE_BACKSLASH: return "backslash";
    case SDL_SCANCODE_GRAVE: return "grave";
    case SDL_SCANCODE_KP_PLUS: return "kp_plus";
    case SDL_SCANCODE_KP_MINUS: return "kp_minus";
    case SDL_SCANCODE_KP_MULTIPLY: return "kp_multiply";
    case SDL_SCANCODE_KP_DIVIDE: return "kp_divide";
    case SDL_SCANCODE_KP_PERIOD: return "kp_period";
    default: return "";
    }
}
