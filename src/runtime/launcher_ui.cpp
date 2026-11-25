#include "launcher_ui.h"
#include "app_config.h"
#include "gfx_backend.h"
#include "net_lobby.h"
#include "xiso_vfs.h"

#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include "imgui_impl_dx11.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Runtime hooks in hle_input.cpp, hle_audio.cpp, hle_d3d8.cpp
void hle_input_poll_ui_snapshot();
bool hle_input_get_port_status(int port, std::string& out_name, int16_t* out_lx, int16_t* out_ly,
                               int16_t* out_rx, int16_t* out_ry, uint32_t* out_btn_mask);
void hle_input_test_rumble(int port);
bool hle_input_consume_hotplug_notice(std::string& out_msg);
void hle_d3d8_set_hd720(bool enable);
void audio_set_master_volume(float vol_0_to_1);

namespace {

enum class BackendUiType { None, DX11, OpenGL3 };

BackendUiType g_ui_backend = BackendUiType::None;
bool g_imgui_inited = false;
SDL_Window* g_sdl_win = nullptr;

std::string g_userdata_root = "userdata";
std::string g_iso_path;
bool g_iso_ok = false;
std::string g_iso_detail;
std::string g_backend_name = "Auto";

bool g_launcher_active = false;
bool g_overlay_open = false;
bool g_launch_requested = false;
bool g_quit_requested = false;

char g_iso_buf[512]{};
char g_name_buf[64]{};
char g_join_buf[128]{};
std::string g_ui_toast;
double g_ui_toast_until = 0.0;

// Cross-platform built-in File/Folder Browser Modal state
bool g_fb_open = false;
bool g_fb_iso_only = true;
fs::path g_fb_dir;
std::string g_fb_selected;

// Interactive key rebind state
int* g_rebinding_key = nullptr;
std::string g_rebinding_label;

bool try_mount_game_source(const std::string& raw_path) {
    auto& cfg = app_config();
    XisoProbeResult pr = xiso_probe_game(raw_path);
    if (!pr.readable || !pr.is_xbox_media || !pr.has_xbe) {
        g_iso_ok = false;
        g_iso_detail = pr.message;
        return false;
    }
    if (!pr.is_whacked) {
        g_iso_ok = false;
        g_iso_detail = pr.message;
        return false;
    }
    if (!XisoVfs::instance().mount_iso(raw_path, g_userdata_root)) {
        g_iso_ok = false;
        g_iso_detail = "Failed mounting verified Whacked! image.";
        return false;
    }
    cfg.iso_path = XisoVfs::instance().mounted_source();
    g_iso_path = cfg.iso_path;
    g_iso_detail = pr.message;
    g_iso_ok = true;
    std::snprintf(g_iso_buf, sizeof(g_iso_buf), "%s", g_iso_path.c_str());
    app_config_save(g_userdata_root);
    return true;
}

void open_file_browser() {
    std::error_code ec;
    fs::path start = fs::path(g_iso_buf);
    if (!start.empty() && fs::exists(start, ec)) {
        g_fb_dir = fs::is_directory(start, ec) ? start : start.parent_path();
    } else {
        g_fb_dir = fs::current_path(ec);
    }
    if (g_fb_dir.empty() || !fs::exists(g_fb_dir, ec)) {
#ifdef _WIN32
        const char* h = std::getenv("USERPROFILE");
#else
        const char* h = std::getenv("HOME");
#endif
        g_fb_dir = (h && h[0]) ? fs::path(h) : fs::current_path(ec);
    }
    g_fb_selected.clear();
    g_fb_open = true;
    ImGui::OpenPopup("Select Whacked! ISO or Game Folder");
}

void render_file_browser_modal() {
    if (g_fb_open && !ImGui::IsPopupOpen("Select Whacked! ISO or Game Folder")) {
        ImGui::OpenPopup("Select Whacked! ISO or Game Folder");
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(760.0f, 470.0f), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal("Select Whacked! ISO or Game Folder", &g_fb_open)) {
        return;
    }

    std::error_code ec;
    auto nav_to = [&](const fs::path& target) {
        if (!target.empty() && fs::is_directory(target, ec)) {
            g_fb_dir = target;
            g_fb_selected.clear();
        }
    };

#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (ImGui::SmallButton("Up")) nav_to(g_fb_dir.parent_path());
    ImGui::SameLine();
    if (ImGui::SmallButton("Current")) nav_to(fs::current_path(ec));
    if (home && home[0]) {
        fs::path hp(home);
        ImGui::SameLine(); if (ImGui::SmallButton("Home")) nav_to(hp);
        ImGui::SameLine(); if (ImGui::SmallButton("Downloads")) nav_to(hp / "Downloads");
        ImGui::SameLine(); if (ImGui::SmallButton("Desktop")) nav_to(hp / "Desktop");
        ImGui::SameLine(); if (ImGui::SmallButton("Games")) nav_to(hp / "Games");
    }
#ifdef _WIN32
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (drives & (1u << i)) {
            char dl[8]{};
            std::snprintf(dl, sizeof(dl), "%c:\\", 'A' + i);
            ImGui::SameLine();
            if (ImGui::SmallButton(dl)) nav_to(fs::path(dl));
        }
    }
#else
    ImGui::SameLine(); if (ImGui::SmallButton("/")) nav_to(fs::path("/"));
#endif

    ImGui::TextDisabled("Location: %s", g_fb_dir.string().c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - 220.0f);
    ImGui::Checkbox("Filter ISO / Game Files", &g_fb_iso_only);
    ImGui::Separator();

    struct DirItem { std::string name; std::string full; bool is_dir; uintmax_t size; };
    std::vector<DirItem> dirs, files;
    for (const auto& de : fs::directory_iterator(g_fb_dir, fs::directory_options::skip_permission_denied, ec)) {
        std::string nm = de.path().filename().string();
        if (!nm.empty() && nm[0] == '.') continue;
        bool is_d = de.is_directory(ec);
        if (is_d) {
            dirs.push_back({nm, de.path().string(), true, 0});
        } else {
            std::string low = nm;
            for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool match = (low.ends_with(".iso") || low.ends_with(".xiso") || low == "default.xbe");
            if (!g_fb_iso_only || match) {
                uintmax_t sz = de.file_size(ec);
                files.push_back({nm, de.path().string(), false, ec ? 0 : sz});
            }
        }
    }
    auto cmp_name = [](const DirItem& a, const DirItem& b) { return a.name < b.name; };
    std::sort(dirs.begin(), dirs.end(), cmp_name);
    std::sort(files.begin(), files.end(), cmp_name);

    if (ImGui::BeginChild("FileBrowserList", ImVec2(0, -42.0f), true)) {
        if (g_fb_dir.has_parent_path() && g_fb_dir != g_fb_dir.parent_path()) {
            if (ImGui::Selectable("[DIR] .. (Parent Folder)", false)) {
                nav_to(g_fb_dir.parent_path());
            }
        }
        for (const auto& d : dirs) {
            std::string label = "[DIR]  " + d.name;
            bool sel = (g_fb_selected == d.full);
            if (ImGui::Selectable(label.c_str(), sel, ImGuiSelectableFlags_AllowDoubleClick)) {
                g_fb_selected = d.full;
                if (ImGui::IsMouseDoubleClicked(0)) {
                    nav_to(fs::path(d.full));
                }
            }
        }
        for (const auto& f : files) {
            double mb = static_cast<double>(f.size) / (1024.0 * 1024.0);
            char label[512]{};
            std::snprintf(label, sizeof(label), "[FILE] %s   (%.1f MB)", f.name.c_str(), mb);
            bool sel = (g_fb_selected == f.full);
            if (ImGui::Selectable(label, sel, ImGuiSelectableFlags_AllowDoubleClick)) {
                g_fb_selected = f.full;
                if (ImGui::IsMouseDoubleClicked(0)) {
                    ui_notify_dropped_path(f.full);
                    g_fb_open = false;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
    }
    ImGui::EndChild();

    std::string target = g_fb_selected.empty() ? g_fb_dir.string() : g_fb_selected;
    if (ImGui::Button("Use Selected / Current Folder", ImVec2(240.0f, 0))) {
        if (try_mount_game_source(target)) {
            g_ui_toast = "Verified & mounted Whacked!";
            g_ui_toast_until = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() + 4.0;
            g_fb_open = false;
            ImGui::CloseCurrentPopup();
        } else {
            // If user double/single-clicked a directory without default.xbe, enter it nicely
            if (fs::is_directory(target, ec) && target != g_fb_dir.string()) {
                nav_to(fs::path(target));
            } else {
                g_ui_toast = g_iso_detail;
                g_ui_toast_until = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() + 5.0;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100.0f, 0))) {
        g_fb_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

double now_sec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void set_toast(const std::string& msg, double secs = 3.0) {
    g_ui_toast = msg;
    g_ui_toast_until = now_sec() + secs;
}

void apply_xbox_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding     = ImVec2(18.0f, 16.0f);
    s.FramePadding      = ImVec2(10.0f, 6.0f);
    s.CellPadding       = ImVec2(8.0f, 5.0f);
    s.ItemSpacing       = ImVec2(10.0f, 8.0f);
    s.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
    s.WindowRounding    = 10.0f;
    s.ChildRounding     = 8.0f;
    s.FrameRounding     = 6.0f;
    s.PopupRounding     = 6.0f;
    s.GrabRounding      = 6.0f;
    s.TabRounding       = 6.0f;
    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.FrameBorderSize   = 1.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]             = ImVec4(0.06f, 0.07f, 0.10f, 0.96f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.09f, 0.11f, 0.15f, 0.92f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.08f, 0.10f, 0.14f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(0.18f, 0.24f, 0.30f, 0.80f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.12f, 0.15f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.16f, 0.22f, 0.28f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.12f, 0.42f, 0.20f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.05f, 0.06f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.07f, 0.26f, 0.11f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.18f, 0.85f, 0.38f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.14f, 0.72f, 0.31f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.24f, 0.90f, 0.44f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.13f, 0.36f, 0.20f, 0.90f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.17f, 0.56f, 0.28f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.20f, 0.70f, 0.35f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.13f, 0.44f, 0.22f, 0.70f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.17f, 0.58f, 0.29f, 0.85f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.20f, 0.72f, 0.35f, 1.00f);
    c[ImGuiCol_Tab]                  = ImVec4(0.10f, 0.13f, 0.18f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.15f, 0.46f, 0.24f, 1.00f);
    c[ImGuiCol_TabSelected]          = ImVec4(0.12f, 0.56f, 0.26f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.18f, 0.25f, 0.32f, 0.70f);
}

void sync_buffers_from_config() {
    auto& cfg = app_config();
    std::snprintf(g_iso_buf, sizeof(g_iso_buf), "%s", (g_iso_path.empty() ? cfg.iso_path : g_iso_path).c_str());
    std::snprintf(g_name_buf, sizeof(g_name_buf), "%s", cfg.player_name.c_str());
    std::snprintf(g_join_buf, sizeof(g_join_buf), "%s", cfg.mp_join_code.c_str());
}

void push_gfx_settings_to_renderer(bool resize_window) {
    auto& cfg = app_config();
    auto& gs = gfx_settings();
    gs.widescreen      = cfg.widescreen;
    gs.unstretched_hud = cfg.unstretched_hud;
    gs.msaa_4x         = cfg.msaa_4x;
    gs.aniso_16x       = cfg.aniso_16x;
    gs.bloom_sharpen   = cfg.bloom_sharpen;
    gs.vsync           = cfg.vsync;
    gs.fps_cap         = cfg.fps_cap;
    gs.show_fps        = cfg.show_fps;
    hle_d3d8_set_hd720(cfg.hd720);

    if (resize_window && g_sdl_win && !cfg.fullscreen) {
        SDL_SetWindowSize(g_sdl_win, cfg.width, cfg.height);
        SDL_SetWindowPosition(g_sdl_win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        gfx_resize(cfg.width, cfg.height);
    }
}

void render_graphics_tab() {
    auto& cfg = app_config();

    struct ResPreset { const char* label; int w; int h; };
    static const ResPreset kPresets[] = {
        {"1280 x 720  (HD 720p)",   1280, 720},
        {"1600 x 900  (HD+ 900p)",  1600, 900},
        {"1920 x 1080 (Full HD)",   1920, 1080},
        {"2560 x 1440 (QHD 1440p)", 2560, 1440},
        {"3840 x 2160 (4K UHD)",    3840, 2160},
    };
    int cur_res = 0;
    for (int i = 0; i < 5; ++i) {
        if (kPresets[i].w == cfg.width && kPresets[i].h == cfg.height) cur_res = i;
    }

    ImGui::TextColored(ImVec4(0.24f, 0.90f, 0.45f, 1.0f), "Display & Resolution");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginCombo("Window Resolution", kPresets[cur_res].label)) {
        for (int i = 0; i < 5; ++i) {
            bool sel = (i == cur_res);
            if (ImGui::Selectable(kPresets[i].label, sel)) {
                cfg.width = kPresets[i].w;
                cfg.height = kPresets[i].h;
                push_gfx_settings_to_renderer(true);
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (ImGui::Checkbox("Fullscreen Mode", &cfg.fullscreen)) {
        if (g_sdl_win) {
            SDL_SetWindowFullscreen(g_sdl_win, cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        }
        gfx_set_fullscreen(cfg.fullscreen);
    }
    ImGui::SameLine(280.0f);
    if (ImGui::Checkbox("Show Performance HUD", &cfg.show_fps)) {
        push_gfx_settings_to_renderer(false);
    }

#ifdef _WIN32
    const char* backends[] = {"Auto (Direct3D 11)", "Direct3D 11", "OpenGL 3.3 Core"};
    int b_idx = (cfg.renderer == "d3d11") ? 1 : ((cfg.renderer == "opengl") ? 2 : 0);
    if (ImGui::Combo("Graphics API (Next Launch)", &b_idx, backends, 3)) {
        cfg.renderer = (b_idx == 1) ? "d3d11" : ((b_idx == 2) ? "opengl" : "auto");
        set_toast("Graphics API preference saved (relaunch to switch backend).");
    }
#else
    ImGui::TextDisabled("Active Graphics API: OpenGL 3.3 Core (Linux / WSLg)");
#endif

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.24f, 0.90f, 0.45f, 1.0f), "Widescreen & Xbox Enhancements");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Checkbox("True 16:9 Hor+ Widescreen", &cfg.widescreen)) push_gfx_settings_to_renderer(false);
    ImGui::SameLine(280.0f);
    if (ImGui::Checkbox("Enable 720p Native Flag", &cfg.hd720)) push_gfx_settings_to_renderer(false);

    if (ImGui::Checkbox("Unstretched 4:3 HUD Overlay", &cfg.unstretched_hud)) push_gfx_settings_to_renderer(false);
    ImGui::SameLine(280.0f);
    if (ImGui::Checkbox("4x MSAA Anti-Aliasing", &cfg.msaa_4x)) push_gfx_settings_to_renderer(false);

    if (ImGui::Checkbox("16x Anisotropic Filtering", &cfg.aniso_16x)) push_gfx_settings_to_renderer(false);
    ImGui::SameLine(280.0f);
    if (ImGui::Checkbox("Post-FX Bloom & Sharpen", &cfg.bloom_sharpen)) push_gfx_settings_to_renderer(false);

    if (ImGui::Checkbox("Vertical Sync (VSync)", &cfg.vsync)) push_gfx_settings_to_renderer(false);
    ImGui::SameLine(280.0f);
    ImGui::SetNextItemWidth(200.0f);
    const char* cap_label = (cfg.fps_cap <= 0) ? "Uncapped" : "%d FPS";
    if (ImGui::SliderInt("Frame Rate Cap", &cfg.fps_cap, 0, 144, cap_label)) {
        push_gfx_settings_to_renderer(false);
    }
}

void render_audio_tab() {
    auto& cfg = app_config();
    ImGui::TextColored(ImVec4(0.24f, 0.90f, 0.45f, 1.0f), "Audio Mixer (SDL2 44.1kHz Stereo)");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::SliderFloat("Master Volume", &cfg.master_volume, 0.0f, 1.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
        audio_set_master_volume(cfg.muted ? 0.0f : cfg.master_volume);
    }
    ImGui::SliderFloat("Sound Effects Volume", &cfg.sfx_volume, 0.0f, 1.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Music & Bink Video Volume", &cfg.music_volume, 0.0f, 1.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Spacing();
    if (ImGui::Checkbox("Mute All Audio", &cfg.muted)) {
        audio_set_master_volume(cfg.muted ? 0.0f : cfg.master_volume);
    }
}

void draw_stick_widget(const char* label, int16_t sx, int16_t sy) {
    ImGui::BeginGroup();
    ImGui::TextDisabled("%s", label);
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float sz = 76.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 ctr(p.x + sz * 0.5f, p.y + sz * 0.5f);
    dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), IM_COL32(20, 25, 34, 255), 8.0f);
    dl->AddCircle(ctr, sz * 0.42f, IM_COL32(55, 70, 90, 255), 32, 1.5f);
    dl->AddLine(ImVec2(p.x + 6, ctr.y), ImVec2(p.x + sz - 6, ctr.y), IM_COL32(45, 55, 70, 180));
    dl->AddLine(ImVec2(ctr.x, p.y + 6), ImVec2(ctr.x, p.y + sz - 6), IM_COL32(45, 55, 70, 180));
    float nx = std::clamp(sx / 32767.0f, -1.0f, 1.0f);
    float ny = std::clamp(sy / 32767.0f, -1.0f, 1.0f);
    ImVec2 dot(ctr.x + nx * (sz * 0.38f), ctr.y - ny * (sz * 0.38f));
    dl->AddCircleFilled(dot, 6.0f, IM_COL32(36, 220, 100, 255));
    ImGui::Dummy(ImVec2(sz, sz));
    ImGui::EndGroup();
}

void render_controllers_tab() {
    auto& cfg = app_config();
    hle_input_poll_ui_snapshot();

    ImGui::TextColored(ImVec4(0.24f, 0.90f, 0.45f, 1.0f), "Connected Gamepads & Calibration");
    ImGui::Separator();
    ImGui::Spacing();

    // Port 1..4 Cards
    for (int p = 0; p < 4; ++p) {
        std::string pname;
        int16_t lx = 0, ly = 0, rx = 0, ry = 0;
        uint32_t mask = 0;
        bool connected = hle_input_get_port_status(p, pname, &lx, &ly, &rx, &ry, &mask);
        if (p % 2 == 1) ImGui::SameLine();
        ImGui::BeginChild(("pad_card_" + std::to_string(p)).c_str(), ImVec2(0, 56.0f), true);
        if (connected) {
            ImGui::TextColored(ImVec4(0.25f, 0.92f, 0.48f, 1.0f), "Port %d:", p + 1);
            ImGui::SameLine();
            ImGui::TextUnformatted(pname.c_str());
        } else {
            ImGui::TextDisabled("Port %d: %s", p + 1, pname.c_str());
        }
        ImGui::EndChild();
    }

    // Wait -> Make grid 2 columns for sticks/settings
    ImGui::Spacing();
    std::string p1_name;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint32_t bmask = 0;
    hle_input_get_port_status(0, p1_name, &lx, &ly, &rx, &ry, &bmask);

    ImGui::BeginChild("PadLiveAndCalib", ImVec2(0, 155.0f), true);
    draw_stick_widget("Left Stick", lx, ly);
    ImGui::SameLine(0, 16.0f);
    draw_stick_widget("Right Stick", rx, ry);
    ImGui::SameLine(0, 24.0f);

    ImGui::BeginGroup();
    auto led = [&](const char* txt, uint32_t bit, ImVec4 col) {
        bool on = (bmask & bit) != 0;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? col : ImVec4(0.14f, 0.17f, 0.22f, 1.0f));
        ImGui::SmallButton(txt);
        ImGui::PopStyleColor();
    };
    ImGui::TextDisabled("Port 1 Live Inputs:");
    led("A", 0x100u, ImVec4(0.1f, 0.7f, 0.2f, 1.0f)); ImGui::SameLine();
    led("B", 0x200u, ImVec4(0.8f, 0.2f, 0.2f, 1.0f)); ImGui::SameLine();
    led("X", 0x400u, ImVec4(0.2f, 0.45f, 0.9f, 1.0f)); ImGui::SameLine();
    led("Y", 0x800u, ImVec4(0.85f, 0.7f, 0.15f, 1.0f)); ImGui::SameLine();
    led("BLK", 0x1000u, ImVec4(0.45f, 0.45f, 0.5f, 1.0f)); ImGui::SameLine();
    led("WHT", 0x2000u, ImVec4(0.75f, 0.75f, 0.8f, 1.0f));

    led("UP", 0x01u, ImVec4(0.2f, 0.8f, 0.4f, 1.0f)); ImGui::SameLine();
    led("DN", 0x02u, ImVec4(0.2f, 0.8f, 0.4f, 1.0f)); ImGui::SameLine();
    led("LF", 0x04u, ImVec4(0.2f, 0.8f, 0.4f, 1.0f)); ImGui::SameLine();
    led("RT", 0x08u, ImVec4(0.2f, 0.8f, 0.4f, 1.0f)); ImGui::SameLine();
    led("START", 0x10u, ImVec4(0.2f, 0.8f, 0.4f, 1.0f)); ImGui::SameLine();
    led("BACK", 0x20u, ImVec4(0.2f, 0.8f, 0.4f, 1.0f));

    ImGui::Spacing();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Left Deadzone", &cfg.deadzone_left, 0.0f, 0.45f, "%.2f");
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Right Deadzone", &cfg.deadzone_right, 0.0f, 0.45f, "%.2f");
    const char* presets[] = {"Xbox Standard", "PlayStation DualSense/DS4", "Nintendo Pro (Swap A/B, X/Y)"};
    ImGui::SetNextItemWidth(220.0f);
    ImGui::Combo("Face Button Preset", &cfg.pad_preset, presets, 3);
    ImGui::EndGroup();

    ImGui::SameLine(0, 20.0f);
    ImGui::BeginGroup();
    ImGui::Checkbox("Enable Rumble", &cfg.rumble_enabled);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Rumble Power", &cfg.rumble_strength, 0.0f, 1.5f, "%.0f%%");
    if (ImGui::Button("Test Port 1 Rumble")) {
        hle_input_test_rumble(0);
    }
    ImGui::TextDisabled("In-Game Menu: F1 or Xbox/Guide or L3+R3");
    ImGui::EndGroup();
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.24f, 0.90f, 0.45f, 1.0f), "Keyboard Bindings (Port 1 Fallback)");
    ImGui::Separator();

    struct BindRow { const char* name; int* scancode; };
    BindRow rows[] = {
        {"Move Up (LS)", &cfg.keys.ls_up},       {"Move Down (LS)", &cfg.keys.ls_down},
        {"Move Left (LS)", &cfg.keys.ls_left},   {"Move Right (LS)", &cfg.keys.ls_right},
        {"Cam Up (RS)", &cfg.keys.rs_up},        {"Cam Down (RS)", &cfg.keys.rs_down},
        {"Cam Left (RS)", &cfg.keys.rs_left},    {"Cam Right (RS)", &cfg.keys.rs_right},
        {"A (Jump/Select)", &cfg.keys.a},        {"B (Grab/Back)", &cfg.keys.b},
        {"X (Attack)", &cfg.keys.x},             {"Y (Special)", &cfg.keys.y},
        {"L Trigger", &cfg.keys.l_trig},         {"R Trigger", &cfg.keys.r_trig},
        {"White (LB)", &cfg.keys.white},         {"Black (RB)", &cfg.keys.black},
        {"D-Pad Up", &cfg.keys.up},              {"D-Pad Down", &cfg.keys.down},
        {"D-Pad Left", &cfg.keys.left},          {"D-Pad Right", &cfg.keys.right},
        {"Start", &cfg.keys.start},              {"Back", &cfg.keys.back},
    };

    if (ImGui::BeginTable("KeyBindTbl", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 150.0f))) {
        for (int i = 0; i < 22; ++i) {
            if (i % 2 == 0) ImGui::TableNextRow();
            ImGui::TableSetColumnIndex((i % 2) * 2);
            ImGui::TextUnformatted(rows[i].name);
            ImGui::TableSetColumnIndex((i % 2) * 2 + 1);
            bool capturing = (g_rebinding_key == rows[i].scancode);
            const char* kname = capturing ? "[ Press Key... ]" : SDL_GetScancodeName(static_cast<SDL_Scancode>(*rows[i].scancode));
            std::string btn_id = std::string(kname && kname[0] ? kname : "Unbound") + "##kb_" + std::to_string(i);
            if (ImGui::Button(btn_id.c_str(), ImVec2(125.0f, 0))) {
                g_rebinding_key = rows[i].scancode;
                g_rebinding_label = rows[i].name;
            }
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset Default Keys")) {
        cfg.keys = KeyBindings{};
        g_rebinding_key = nullptr;
        set_toast("Keyboard bindings restored to defaults.");
    }
}

void render_multiplayer_tab() {
    auto& cfg = app_config();
    NetLobbyStatus st = net_get_status();
    std::vector<NetPeerInfo> peers = net_list_peers();

    ImGui::TextColored(ImVec4(0.24f, 0.90f, 0.45f, 1.0f), "Cross-Platform System Link & Online Rooms");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::InputText("Player Handle", g_name_buf, sizeof(g_name_buf))) {
        cfg.player_name = g_name_buf;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Virtual IP: %s  |  LAN IP: %s",
                        st.self_virtual_ip.c_str(),
                        st.local_lan_ip.empty() ? "Detecting..." : st.local_lan_ip.c_str());

    ImGui::Spacing();
    ImGui::BeginChild("NetModeBar", ImVec2(0, 128.0f), true);
    ImGui::Text("Tunnel Status: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.28f, 0.92f, 0.50f, 1.0f), "%s", st.status_text.c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - 220.0f);
    ImGui::TextDisabled("TX: %llu pkts   RX: %llu pkts",
                        static_cast<unsigned long long>(st.tx_packets),
                        static_cast<unsigned long long>(st.rx_packets));

    ImGui::Spacing();
    if (ImGui::RadioButton("LAN / Offline Multicast", cfg.mp_mode == 0)) {
        cfg.mp_mode = 0;
        net_start_offline_lan(cfg.player_name);
    }
    ImGui::SameLine(230.0f);
    if (ImGui::RadioButton("Host Online Room", cfg.mp_mode == 1)) {
        cfg.mp_mode = 1;
    }
    ImGui::SameLine(430.0f);
    if (ImGui::RadioButton("Join Friend's Room", cfg.mp_mode == 2)) {
        cfg.mp_mode = 2;
    }

    ImGui::Spacing();
    if (cfg.mp_mode == 1) {
        ImGui::SetNextItemWidth(110.0f);
        ImGui::InputInt("UDP Port", &cfg.mp_tunnel_port, 0, 0);
        cfg.mp_tunnel_port = std::clamp(cfg.mp_tunnel_port, 1024, 65535);
        ImGui::SameLine();
        ImGui::Checkbox("Auto UPnP", &cfg.mp_upnp);
        ImGui::SameLine();
        if (ImGui::Button("Start / Restart Host")) {
            cfg.player_name = g_name_buf;
            if (!net_start_host(static_cast<uint16_t>(cfg.mp_tunnel_port), cfg.mp_upnp, cfg.player_name)) {
                set_toast("Failed to bind host UDP port!");
            } else {
                set_toast("Online System Link Room hosted!");
            }
        }
        if (st.mode == NetLobbyMode::HostRoom && !st.room_code.empty()) {
            ImGui::TextColored(ImVec4(0.98f, 0.80f, 0.25f, 1.0f), "Room Code: %s", st.room_code.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy Code")) {
                SDL_SetClipboardText(st.room_code.c_str());
                set_toast("Copied room code to clipboard!");
            }
            if (!st.public_ip.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(External: %s:%d%s)", st.public_ip.c_str(), cfg.mp_tunnel_port,
                                    st.upnp_mapped ? " [UPnP OK]" : "");
            } else {
                ImGui::SameLine();
                ImGui::TextDisabled("(LAN Direct: %s)", st.host_endpoint.c_str());
            }
        }
    } else if (cfg.mp_mode == 2) {
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::InputText("Room Code or IP:Port", g_join_buf, sizeof(g_join_buf))) {
            cfg.mp_join_code = g_join_buf;
        }
        ImGui::SameLine();
        if (ImGui::Button("Connect to Room")) {
            cfg.player_name = g_name_buf;
            cfg.mp_join_code = g_join_buf;
            if (net_join_room(cfg.mp_join_code, cfg.player_name)) {
                set_toast("Connecting to System Link room...");
            } else {
                set_toast("Invalid room code or unreachable address.");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Leave Room")) {
            cfg.mp_mode = 0;
            net_start_offline_lan(cfg.player_name);
        }
    } else {
        ImGui::TextDisabled("Zero-config LAN multicast (239.69.69.1:30741). Multiple local/LAN instances discover each other automatically.");
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.24f, 0.90f, 0.45f, 1.0f), "Connected Players (%zu)", peers.size());
    if (ImGui::BeginTable("PeerTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg, ImVec2(0, 120.0f))) {
        ImGui::TableSetupColumn("Player");
        ImGui::TableSetupColumn("Virtual Xbox IP");
        ImGui::TableSetupColumn("Ping");
        ImGui::TableSetupColumn("Role");
        ImGui::TableHeadersRow();
        for (const auto& p : peers) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%s%s", p.player_name.c_str(), p.is_self ? " (You)" : "");
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(p.virtual_ip_str.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::Text("%d ms", p.ping_ms);
            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(p.is_host ? "Host" : "Peer");
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled("Tip: Inside Whacked!, open Multiplayer -> System Link to Host or Join matches over LAN or Online Tunnel.");
}

void render_launcher_window() {
    auto& cfg = app_config();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("WhackedLauncherMain", nullptr, wf)) {
        ImGui::End();
        return;
    }

    // Hero Header Banner
    {
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilledMultiColor(p0, ImVec2(p0.x + w, p0.y + 76.0f),
            IM_COL32(14, 78, 32, 255), IM_COL32(12, 36, 26, 255),
            IM_COL32(10, 16, 24, 255), IM_COL32(12, 28, 22, 255));
        dl->AddRect(p0, ImVec2(p0.x + w, p0.y + 76.0f), IM_COL32(36, 175, 76, 180), 8.0f, 0, 1.5f);

        ImGui::SetCursorScreenPos(ImVec2(p0.x + 18.0f, p0.y + 12.0f));
        ImGui::SetWindowFontScale(1.65f);
        ImGui::TextColored(ImVec4(0.98f, 0.84f, 0.28f, 1.0f), "WHACKED!");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::SameLine(0, 14.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
        ImGui::TextColored(ImVec4(0.75f, 0.94f, 0.80f, 1.0f), "Original Xbox Static Recompilation  |  PC & Linux Edition");

        ImGui::SetCursorScreenPos(ImVec2(p0.x + 18.0f, p0.y + 48.0f));
        ImGui::TextDisabled("Renderer: %s   |   ISO: %s",
                            g_backend_name.c_str(),
                            g_iso_ok ? "Ready (Mounted)" : "Missing / Select XISO");
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + 86.0f));
    }

    float footer_h = 56.0f;
    float body_h = std::max(200.0f, ImGui::GetContentRegionAvail().y - footer_h);

    if (ImGui::BeginChild("LauncherBody", ImVec2(0, body_h), false)) {
        if (ImGui::BeginTabBar("LauncherTabs")) {
            if (ImGui::BeginTabItem("  Play & Setup  ")) {
                ImGui::Spacing();
                ImGui::BeginChild("IsoCard", ImVec2(0, g_iso_ok ? 158.0f : 225.0f), true);
                ImGui::TextColored(ImVec4(0.24f, 0.90f, 0.45f, 1.0f), "Game Source (Whacked! XISO / Redump ISO / Extracted Folder)");
                ImGui::Separator();
                ImGui::Spacing();

                float btn_area = 335.0f;
#ifdef _WIN32
                btn_area = 435.0f;
#endif
                ImGui::SetNextItemWidth(std::max(220.0f, ImGui::GetContentRegionAvail().x - btn_area));
                ImGui::InputText("##IsoPathInput", g_iso_buf, sizeof(g_iso_buf));
                ImGui::SameLine();
                if (ImGui::Button("Browse...", ImVec2(100.0f, 0))) {
                    open_file_browser();
                }
#ifdef _WIN32
                ImGui::SameLine();
                if (ImGui::Button("OS Picker", ImVec2(95.0f, 0))) {
                    OPENFILENAMEA ofn{};
                    char file_out[512]{};
                    std::snprintf(file_out, sizeof(file_out), "%s", g_iso_buf);
                    ofn.lStructSize = sizeof(ofn);
                    ofn.lpstrFilter = "Xbox ISO Images (*.iso;*.xiso)\0*.iso;*.xiso\0All Files\0*.*\0";
                    ofn.lpstrFile = file_out;
                    ofn.nMaxFile = sizeof(file_out);
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                    if (GetOpenFileNameA(&ofn)) {
                        try_mount_game_source(file_out);
                        set_toast(g_iso_ok ? "Verified & mounted Whacked!" : g_iso_detail, 4.0);
                    }
                }
#endif
                ImGui::SameLine();
                if (ImGui::Button("Auto-Find", ImVec2(95.0f, 0))) {
                    auto found = xiso_auto_discover(fs::path(g_userdata_root).parent_path().string());
                    if (!found.empty() && try_mount_game_source(found.front())) {
                        set_toast("Found & mounted: " + found.front(), 4.0);
                    } else {
                        set_toast("No Whacked! copy found in Home/Downloads/Games/Desktop.", 4.0);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Verify", ImVec2(95.0f, 0))) {
                    try_mount_game_source(g_iso_buf);
                    set_toast(g_iso_ok ? "Verified & mounted Whacked!" : g_iso_detail, 4.0);
                }

                ImGui::Spacing();
                if (g_iso_ok) {
                    ImGui::TextColored(ImVec4(0.24f, 0.90f, 0.45f, 1.0f), "[OK] %s",
                                       g_iso_detail.empty() ? "Valid Whacked! XISO mounted and ready to launch." : g_iso_detail.c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.95f, 0.42f, 0.30f, 1.0f), "[!] %s",
                                       g_iso_detail.empty() ? "No Whacked! copy selected yet." : g_iso_detail.c_str());
                    ImGui::Spacing();
                    ImGui::BulletText("Step 1: Drag & drop your legally owned .iso / .xiso.iso or extracted folder onto this window (or click Browse / Auto-Find).");
                    ImGui::BulletText("Step 2: Connect any Xbox, DualSense/DS4, Switch Pro controller or configure keyboard in the Controllers tab.");
                    ImGui::BulletText("Step 3: Click PLAY WHACKED! (Press F1 or Guide anytime in-game for settings & Online Multiplayer).");
                }

                fs::path save_dir = fs::path(g_userdata_root) / "UDATA";
                std::error_code ec;
                bool has_saves = fs::exists(save_dir, ec);
                ImGui::TextDisabled("Data Folder: %s (%s)   |   Tip: Drag & drop any ISO/folder here",
                                    g_userdata_root.c_str(),
                                    has_saves ? "Existing Save Profiles Present" : "Clean Profile Ready");
                ImGui::EndChild();

                render_file_browser_modal();

                ImGui::Spacing();
                ImGui::BeginChild("QuickFeatures", ImVec2(0, 0), true);
                ImGui::TextColored(ImVec4(0.24f, 0.90f, 0.45f, 1.0f), "Recompilation Highlights");
                ImGui::Separator();
                ImGui::Columns(2, nullptr, false);
                ImGui::BulletText("True 16:9 Hor+ Widescreen & HD 720p Support");
                ImGui::BulletText("4x MSAA, 16x Anisotropic Filtering & Post-FX");
                ImGui::BulletText("NV2A Vertex & Pixel Shaders w/ Disk Caching");
                ImGui::NextColumn();
                ImGui::BulletText("4-Player Xbox, PlayStation, Switch & Keyboard");
                ImGui::BulletText("SDL2 Real-Time Stereo Mixing & Bink FMVs");
                ImGui::BulletText("Cross-Platform LAN & Room Code Online Play");
                ImGui::Columns(1);
                ImGui::EndChild();

                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("  Graphics  ")) {
                ImGui::Spacing();
                render_graphics_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("  Audio  ")) {
                ImGui::Spacing();
                render_audio_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("  Controllers  ")) {
                ImGui::Spacing();
                render_controllers_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("  Multiplayer  ")) {
                ImGui::Spacing();
                render_multiplayer_tab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();

    // Footer bar
    ImGui::Separator();
    ImGui::Checkbox("Skip Launcher Next Time", &cfg.skip_launcher);
    ImGui::SameLine();
    if (now_sec() < g_ui_toast_until && !g_ui_toast.empty()) {
        ImGui::TextColored(ImVec4(0.98f, 0.84f, 0.28f, 1.0f), "  |  %s", g_ui_toast.c_str());
    }

    float right_x = ImGui::GetWindowWidth() - 405.0f;
    if (right_x > ImGui::GetCursorPosX() + 10.0f) {
        ImGui::SameLine(right_x);
    } else {
        ImGui::SameLine();
    }

    if (ImGui::Button("Save Settings", ImVec2(115.0f, 34.0f))) {
        cfg.iso_path = g_iso_buf;
        cfg.player_name = g_name_buf;
        app_config_save(g_userdata_root);
        set_toast("Configuration saved!");
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(80.0f, 34.0f))) {
        g_quit_requested = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_iso_ok);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.11f, 0.56f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.72f, 0.32f, 1.0f));
    if (ImGui::Button("PLAY WHACKED!", ImVec2(170.0f, 34.0f))) {
        cfg.iso_path = g_iso_buf;
        cfg.player_name = g_name_buf;
        app_config_save(g_userdata_root);
        push_gfx_settings_to_renderer(false);
        g_launch_requested = true;
    }
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();

    ImGui::End();
}

void render_overlay_window() {
    auto& cfg = app_config();
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    // Subtle FPS / Toast pills when overlay is closed
    if (!g_overlay_open) {
        if (cfg.show_fps) {
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 12.0f, vp->Pos.y + 10.0f), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.45f);
            ImGuiWindowFlags hud_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
            if (ImGui::Begin("WhackedFpsHud", nullptr, hud_flags)) {
                ImGui::Text("%.1f FPS | %s | [F1] Menu", gfx_get_fps(), gfx_is_opengl() ? "OpenGL" : "D3D11");
            }
            ImGui::End();
        }
        if (now_sec() < g_ui_toast_until && !g_ui_toast.empty()) {
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y - 24.0f),
                                    ImGuiCond_Always, ImVec2(0.5f, 1.0f));
            ImGui::SetNextWindowBgAlpha(0.78f);
            ImGuiWindowFlags tflags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
            if (ImGui::Begin("WhackedToastHud", nullptr, tflags)) {
                ImGui::TextColored(ImVec4(0.98f, 0.84f, 0.28f, 1.0f), "%s", g_ui_toast.c_str());
            }
            ImGui::End();
        }
        return;
    }

    // Dimmer backdrop
    ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                  IM_COL32(5, 8, 14, 155));

    ImVec2 win_sz(std::min(880.0f, vp->Size.x * 0.88f), std::min(580.0f, vp->Size.y * 0.88f));
    ImGui::SetNextWindowSize(win_sz, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - win_sz.x) * 0.5f,
                                   vp->Pos.y + (vp->Size.y - win_sz.y) * 0.5f), ImGuiCond_Always);

    ImGuiWindowFlags owf = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Whacked! In-Game Menu   [F1 / Xbox Guide / L3+R3 to Close]", &g_overlay_open, owf)) {
        float body_h = std::max(180.0f, ImGui::GetContentRegionAvail().y - 46.0f);
        if (ImGui::BeginChild("OverlayBody", ImVec2(0, body_h), false)) {
            if (ImGui::BeginTabBar("OverlayTabs")) {
                if (ImGui::BeginTabItem("Graphics")) { render_graphics_tab(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Audio")) { render_audio_tab(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Controllers")) { render_controllers_tab(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Multiplayer")) { render_multiplayer_tab(); ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (ImGui::Button("Resume [F1]", ImVec2(120.0f, 30.0f))) g_overlay_open = false;
        ImGui::SameLine();
        if (ImGui::Button("Screenshot [F12]", ImVec2(140.0f, 30.0f))) {
            fs::path sdir = fs::path(g_userdata_root) / "screenshots";
            std::error_code ec;
            fs::create_directories(sdir, ec);
            auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::string path = (sdir / ("whacked_" + std::to_string(t) + ".png")).string();
            gfx_request_screenshot(path);
            set_toast("Saved screenshot to " + path);
        }
        ImGui::SameLine();
        if (ImGui::Button("Save Settings", ImVec2(120.0f, 30.0f))) {
            app_config_save(g_userdata_root);
            set_toast("Settings saved!");
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit Game", ImVec2(100.0f, 30.0f))) {
            g_quit_requested = true;
        }
        if (now_sec() < g_ui_toast_until && !g_ui_toast.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.98f, 0.84f, 0.28f, 1.0f), "  %s", g_ui_toast.c_str());
        }
    }
    ImGui::End();
}

} // namespace

void ui_set_userdata_root(const std::string& root) { g_userdata_root = root; }
void ui_set_iso_status(const std::string& iso_path, bool ok, const std::string& detail) {
    g_iso_path = iso_path;
    g_iso_ok = ok;
    g_iso_detail = detail;
    sync_buffers_from_config();
}
void ui_notify_dropped_path(const std::string& path) {
    if (path.empty()) return;
    if (try_mount_game_source(path)) {
        set_toast("Loaded Whacked!: " + g_iso_detail, 4.0);
    } else {
        set_toast(g_iso_detail, 5.0);
    }
}
void ui_show_toast(const std::string& msg, double seconds) {
    set_toast(msg, seconds);
}
void ui_set_active_backend_name(const char* name) { g_backend_name = name ? name : "Auto"; }

void ui_init_dx11(void* sdl_win, void* dx_dev, void* dx_ctx) {
#ifdef _WIN32
    if (g_imgui_inited) ui_shutdown();
    g_sdl_win = static_cast<SDL_Window*>(sdl_win);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    apply_xbox_style();
    ImGui_ImplSDL2_InitForD3D(g_sdl_win);
    ImGui_ImplDX11_Init(static_cast<ID3D11Device*>(dx_dev), static_cast<ID3D11DeviceContext*>(dx_ctx));
    g_ui_backend = BackendUiType::DX11;
    g_imgui_inited = true;
    sync_buffers_from_config();
#else
    (void)sdl_win; (void)dx_dev; (void)dx_ctx;
#endif
}

void ui_init_gl3(void* sdl_win, void* gl_ctx) {
    if (g_imgui_inited) ui_shutdown();
    g_sdl_win = static_cast<SDL_Window*>(sdl_win);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    apply_xbox_style();
    ImGui_ImplSDL2_InitForOpenGL(g_sdl_win, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    g_ui_backend = BackendUiType::OpenGL3;
    g_imgui_inited = true;
    sync_buffers_from_config();
}

void ui_shutdown() {
    if (!g_imgui_inited) return;
#ifdef _WIN32
    if (g_ui_backend == BackendUiType::DX11) ImGui_ImplDX11_Shutdown();
#endif
    if (g_ui_backend == BackendUiType::OpenGL3) ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    g_ui_backend = BackendUiType::None;
    g_imgui_inited = false;
}

void ui_process_sdl_event(const void* ev_ptr) {
    if (!ev_ptr) return;
    const auto* ev = static_cast<const SDL_Event*>(ev_ptr);

    if (ev->type == SDL_KEYDOWN && !ev->key.repeat) {
        if (g_rebinding_key) {
            if (ev->key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                g_rebinding_key = nullptr;
            } else {
                *g_rebinding_key = static_cast<int>(ev->key.keysym.scancode);
                set_toast("Bound " + g_rebinding_label + " to " + SDL_GetScancodeName(ev->key.keysym.scancode));
                g_rebinding_key = nullptr;
            }
            return;
        }
        if (ev->key.keysym.scancode == SDL_SCANCODE_F1 && !g_launcher_active) {
            ui_toggle_overlay();
        } else if (ev->key.keysym.scancode == SDL_SCANCODE_F12) {
            fs::path sdir = fs::path(g_userdata_root) / "screenshots";
            std::error_code ec;
            fs::create_directories(sdir, ec);
            auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            gfx_request_screenshot((sdir / ("whacked_" + std::to_string(t) + ".png")).string());
        }
    }

    if (g_imgui_inited) {
        ImGui_ImplSDL2_ProcessEvent(ev);
    }
}

bool ui_wants_keyboard() {
    return g_imgui_inited && ImGui::GetIO().WantCaptureKeyboard;
}

bool ui_wants_mouse() {
    return g_imgui_inited && ImGui::GetIO().WantCaptureMouse;
}

void ui_set_launcher_active(bool active) {
    g_launcher_active = active;
    if (active) g_overlay_open = false;
}
bool ui_is_launcher_active() { return g_launcher_active; }

bool ui_consume_launch_requested() {
    bool v = g_launch_requested;
    g_launch_requested = false;
    return v;
}

bool ui_consume_quit_requested() {
    bool v = g_quit_requested;
    g_quit_requested = false;
    return v;
}

void ui_set_overlay_open(bool open) { g_overlay_open = open; }
void ui_toggle_overlay() {
    if (!g_launcher_active) g_overlay_open = !g_overlay_open;
}
bool ui_is_overlay_open() { return g_overlay_open; }

void ui_render_frame(bool is_opengl) {
    if (!g_imgui_inited) return;

    std::string hp_msg;
    if (hle_input_consume_hotplug_notice(hp_msg)) {
        set_toast(hp_msg, 3.5);
    }

    ImGuiIO& io = ImGui::GetIO();
    if (g_launcher_active || g_overlay_open) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    }

    if (is_opengl) {
        ImGui_ImplOpenGL3_NewFrame();
    } else {
#ifdef _WIN32
        ImGui_ImplDX11_NewFrame();
#endif
    }
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    if (g_launcher_active) {
        render_launcher_window();
    } else {
        render_overlay_window();
    }

    ImGui::Render();
    if (is_opengl) {
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    } else {
#ifdef _WIN32
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
#endif
    }
}
