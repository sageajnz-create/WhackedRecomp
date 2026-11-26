#pragma once

#include <cstdint>
#include <string>

struct KeyBindings {
    // SDL_Scancode integers
    int up       = 82; // UP
    int down     = 81; // DOWN
    int left     = 80; // LEFT
    int right    = 79; // RIGHT
    int ls_up    = 26; // W
    int ls_down  = 22; // S
    int ls_left  = 4;  // A
    int ls_right = 7;  // D
    int rs_left  = 13; // J
    int rs_right = 15; // L
    int rs_up    = 12; // I
    int rs_down  = 14; // K
    int a        = 44; // SPACE
    int b        = 225;// LSHIFT
    int x        = 20; // Q
    int y        = 8;  // E
    int l_trig   = 9;  // F
    int r_trig   = 21; // R
    int white    = 6;  // C
    int black    = 25; // V
    int l_thumb  = 29; // Z
    int r_thumb  = 27; // X
    int start    = 40; // RETURN
    int back     = 43; // TAB
};

enum class PadButtonPreset : int {
    XboxDefault = 0,
    PlayStation = 1, // Square=X, Cross=A, Circle=B, Triangle=Y (positional match on SDL GameController is default, option for B/A swap)
    NintendoSwap = 2 // Swaps A<->B and X<->Y for Nintendo physical layout
};

struct AppConfig {
    // General
    std::string iso_path;
    bool skip_launcher = false;
    std::string player_name = "Player1";

    // Graphics
    std::string renderer = "auto"; // "auto", "d3d11", "opengl"
    int width = 1280;
    int height = 720;
    bool fullscreen = false;
    bool widescreen = true;
    bool hd720 = true;
    bool unstretched_hud = false;
    bool msaa_4x = true;
    bool aniso_16x = true;
    bool bloom_sharpen = true;
    bool vsync = true;
    int fps_cap = 60; // 0 = uncapped
    bool show_fps = true;

    // Audio
    float master_volume = 1.0f;
    float sfx_volume = 1.0f;
    float music_volume = 1.0f;
    bool muted = false;

    // Controllers
    float deadzone_left = 0.15f;
    float deadzone_right = 0.15f;
    bool rumble_enabled = true;
    float rumble_strength = 1.0f;
    bool invert_cam_y = false;
    float cam_sensitivity = 1.0f;
    int pad_preset = 0; // PadButtonPreset
    KeyBindings keys{};

    // Multiplayer / Virtual System Link
    int mp_mode = 0; // 0 = Local LAN / Offline, 1 = Host Room, 2 = Join Room
    int mp_tunnel_port = 30740;
    std::string mp_join_code; // Host IP:Port or WHK-... room code
    bool mp_upnp = true;
};

AppConfig& app_config();
bool app_config_load(const std::string& userdata_root);
bool app_config_save(const std::string& userdata_root);
std::string app_config_path(const std::string& userdata_root);
