#include "app_config.h"
#include <SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {
AppConfig g_cfg{};

std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool parse_bool(const std::string& v) {
    std::string l = v;
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return l == "1" || l == "true" || l == "yes" || l == "on";
}

int parse_scancode(const std::string& v) {
    if (v.empty()) return 0;
    bool all_digits = true;
    for (char c : v) {
        if (!std::isdigit(static_cast<unsigned char>(c))) { all_digits = false; break; }
    }
    if (all_digits) return std::atoi(v.c_str());
    SDL_Scancode sc = SDL_GetScancodeFromName(v.c_str());
    if (sc != SDL_SCANCODE_UNKNOWN) return static_cast<int>(sc);
    // Friendly aliases (e.g. from RecompLauncher bindings)
    if (v == "Enter") return SDL_SCANCODE_RETURN;
    if (v == "Esc")   return SDL_SCANCODE_ESCAPE;
    return std::atoi(v.c_str());
}

std::string format_scancode(int code) {
    if (code <= 0) return "0";
    const char* nm = SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
    if (nm && nm[0]) return std::string(nm);
    return std::to_string(code);
}
} // namespace

AppConfig& app_config() { return g_cfg; }

std::string app_config_path(const std::string& userdata_root) {
    return (fs::path(userdata_root) / "whacked_config.ini").string();
}

bool app_config_load(const std::string& userdata_root) {
    std::ifstream in(app_config_path(userdata_root));
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));

        if (k == "iso_path") g_cfg.iso_path = v;
        else if (k == "skip_launcher") g_cfg.skip_launcher = parse_bool(v);
        else if (k == "player_name" && !v.empty()) g_cfg.player_name = v.substr(0, 24);
        else if (k == "renderer" && !v.empty()) g_cfg.renderer = v;
        else if (k == "width") g_cfg.width = std::clamp(std::atoi(v.c_str()), 640, 7680);
        else if (k == "height") g_cfg.height = std::clamp(std::atoi(v.c_str()), 480, 4320);
        else if (k == "fullscreen") g_cfg.fullscreen = parse_bool(v);
        else if (k == "widescreen") g_cfg.widescreen = parse_bool(v);
        else if (k == "hd720") g_cfg.hd720 = parse_bool(v);
        else if (k == "unstretched_hud") g_cfg.unstretched_hud = parse_bool(v);
        else if (k == "msaa_4x") g_cfg.msaa_4x = parse_bool(v);
        else if (k == "aniso_16x") g_cfg.aniso_16x = parse_bool(v);
        else if (k == "bloom_sharpen") g_cfg.bloom_sharpen = parse_bool(v);
        else if (k == "vsync") g_cfg.vsync = parse_bool(v);
        else if (k == "fps_cap") g_cfg.fps_cap = std::clamp(std::atoi(v.c_str()), 0, 240);
        else if (k == "show_fps") g_cfg.show_fps = parse_bool(v);
        else if (k == "master_volume") g_cfg.master_volume = std::clamp(std::strtof(v.c_str(), nullptr), 0.0f, 1.0f);
        else if (k == "sfx_volume") g_cfg.sfx_volume = std::clamp(std::strtof(v.c_str(), nullptr), 0.0f, 1.0f);
        else if (k == "music_volume") g_cfg.music_volume = std::clamp(std::strtof(v.c_str(), nullptr), 0.0f, 1.0f);
        else if (k == "muted") g_cfg.muted = parse_bool(v);
        else if (k == "deadzone_left") g_cfg.deadzone_left = std::clamp(std::strtof(v.c_str(), nullptr), 0.0f, 0.5f);
        else if (k == "deadzone_right") g_cfg.deadzone_right = std::clamp(std::strtof(v.c_str(), nullptr), 0.0f, 0.5f);
        else if (k == "rumble_enabled") g_cfg.rumble_enabled = parse_bool(v);
        else if (k == "rumble_strength") g_cfg.rumble_strength = std::clamp(std::strtof(v.c_str(), nullptr), 0.0f, 1.5f);
        else if (k == "pad_preset") g_cfg.pad_preset = std::clamp(std::atoi(v.c_str()), 0, 2);
        else if (k == "key_up") g_cfg.keys.up = parse_scancode(v);
        else if (k == "key_down") g_cfg.keys.down = parse_scancode(v);
        else if (k == "key_left") g_cfg.keys.left = parse_scancode(v);
        else if (k == "key_right") g_cfg.keys.right = parse_scancode(v);
        else if (k == "key_ls_up") g_cfg.keys.ls_up = parse_scancode(v);
        else if (k == "key_ls_down") g_cfg.keys.ls_down = parse_scancode(v);
        else if (k == "key_ls_left") g_cfg.keys.ls_left = parse_scancode(v);
        else if (k == "key_ls_right") g_cfg.keys.ls_right = parse_scancode(v);
        else if (k == "key_rs_left") g_cfg.keys.rs_left = parse_scancode(v);
        else if (k == "key_rs_right") g_cfg.keys.rs_right = parse_scancode(v);
        else if (k == "key_rs_up") g_cfg.keys.rs_up = parse_scancode(v);
        else if (k == "key_rs_down") g_cfg.keys.rs_down = parse_scancode(v);
        else if (k == "key_a") g_cfg.keys.a = parse_scancode(v);
        else if (k == "key_b") g_cfg.keys.b = parse_scancode(v);
        else if (k == "key_x") g_cfg.keys.x = parse_scancode(v);
        else if (k == "key_y") g_cfg.keys.y = parse_scancode(v);
        else if (k == "key_l") g_cfg.keys.l_trig = parse_scancode(v);
        else if (k == "key_r") g_cfg.keys.r_trig = parse_scancode(v);
        else if (k == "key_white") g_cfg.keys.white = parse_scancode(v);
        else if (k == "key_black") g_cfg.keys.black = parse_scancode(v);
        else if (k == "key_l3") g_cfg.keys.l_thumb = parse_scancode(v);
        else if (k == "key_r3") g_cfg.keys.r_thumb = parse_scancode(v);
        else if (k == "key_start") g_cfg.keys.start = parse_scancode(v);
        else if (k == "key_back") g_cfg.keys.back = parse_scancode(v);
        else if (k == "mp_mode") g_cfg.mp_mode = std::clamp(std::atoi(v.c_str()), 0, 2);
        else if (k == "mp_tunnel_port") g_cfg.mp_tunnel_port = std::clamp(std::atoi(v.c_str()), 1024, 65535);
        else if (k == "mp_join_code") g_cfg.mp_join_code = v;
        else if (k == "mp_upnp") g_cfg.mp_upnp = parse_bool(v);
    }
    return true;
}

bool app_config_save(const std::string& userdata_root) {
    std::error_code ec;
    fs::create_directories(userdata_root, ec);
    std::ofstream out(app_config_path(userdata_root));
    if (!out.is_open()) return false;
    out << "# WhackedRecomp Configuration\n"
        << "[General]\n"
        << "iso_path=" << g_cfg.iso_path << "\n"
        << "skip_launcher=" << (g_cfg.skip_launcher ? 1 : 0) << "\n"
        << "player_name=" << g_cfg.player_name << "\n\n"
        << "[Graphics]\n"
        << "renderer=" << g_cfg.renderer << "\n"
        << "width=" << g_cfg.width << "\n"
        << "height=" << g_cfg.height << "\n"
        << "fullscreen=" << (g_cfg.fullscreen ? 1 : 0) << "\n"
        << "widescreen=" << (g_cfg.widescreen ? 1 : 0) << "\n"
        << "hd720=" << (g_cfg.hd720 ? 1 : 0) << "\n"
        << "unstretched_hud=" << (g_cfg.unstretched_hud ? 1 : 0) << "\n"
        << "msaa_4x=" << (g_cfg.msaa_4x ? 1 : 0) << "\n"
        << "aniso_16x=" << (g_cfg.aniso_16x ? 1 : 0) << "\n"
        << "bloom_sharpen=" << (g_cfg.bloom_sharpen ? 1 : 0) << "\n"
        << "vsync=" << (g_cfg.vsync ? 1 : 0) << "\n"
        << "fps_cap=" << g_cfg.fps_cap << "\n"
        << "show_fps=" << (g_cfg.show_fps ? 1 : 0) << "\n\n"
        << "[Audio]\n"
        << "master_volume=" << g_cfg.master_volume << "\n"
        << "sfx_volume=" << g_cfg.sfx_volume << "\n"
        << "music_volume=" << g_cfg.music_volume << "\n"
        << "muted=" << (g_cfg.muted ? 1 : 0) << "\n\n"
        << "[Input]\n"
        << "deadzone_left=" << g_cfg.deadzone_left << "\n"
        << "deadzone_right=" << g_cfg.deadzone_right << "\n"
        << "rumble_enabled=" << (g_cfg.rumble_enabled ? 1 : 0) << "\n"
        << "rumble_strength=" << g_cfg.rumble_strength << "\n"
        << "pad_preset=" << g_cfg.pad_preset << "\n"
        << "key_up=" << format_scancode(g_cfg.keys.up) << "\n"
        << "key_down=" << format_scancode(g_cfg.keys.down) << "\n"
        << "key_left=" << format_scancode(g_cfg.keys.left) << "\n"
        << "key_right=" << format_scancode(g_cfg.keys.right) << "\n"
        << "key_ls_up=" << format_scancode(g_cfg.keys.ls_up) << "\n"
        << "key_ls_down=" << format_scancode(g_cfg.keys.ls_down) << "\n"
        << "key_ls_left=" << format_scancode(g_cfg.keys.ls_left) << "\n"
        << "key_ls_right=" << format_scancode(g_cfg.keys.ls_right) << "\n"
        << "key_rs_left=" << format_scancode(g_cfg.keys.rs_left) << "\n"
        << "key_rs_right=" << format_scancode(g_cfg.keys.rs_right) << "\n"
        << "key_rs_up=" << format_scancode(g_cfg.keys.rs_up) << "\n"
        << "key_rs_down=" << format_scancode(g_cfg.keys.rs_down) << "\n"
        << "key_a=" << format_scancode(g_cfg.keys.a) << "\n"
        << "key_b=" << format_scancode(g_cfg.keys.b) << "\n"
        << "key_x=" << format_scancode(g_cfg.keys.x) << "\n"
        << "key_y=" << format_scancode(g_cfg.keys.y) << "\n"
        << "key_l=" << format_scancode(g_cfg.keys.l_trig) << "\n"
        << "key_r=" << format_scancode(g_cfg.keys.r_trig) << "\n"
        << "key_white=" << format_scancode(g_cfg.keys.white) << "\n"
        << "key_black=" << format_scancode(g_cfg.keys.black) << "\n"
        << "key_l3=" << format_scancode(g_cfg.keys.l_thumb) << "\n"
        << "key_r3=" << format_scancode(g_cfg.keys.r_thumb) << "\n"
        << "key_start=" << format_scancode(g_cfg.keys.start) << "\n"
        << "key_back=" << format_scancode(g_cfg.keys.back) << "\n\n"
        << "[Multiplayer]\n"
        << "mp_mode=" << g_cfg.mp_mode << "\n"
        << "mp_tunnel_port=" << g_cfg.mp_tunnel_port << "\n"
        << "mp_join_code=" << g_cfg.mp_join_code << "\n"
        << "mp_upnp=" << (g_cfg.mp_upnp ? 1 : 0) << "\n";
    return true;
}
