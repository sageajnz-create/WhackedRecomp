#include "launcher_ui.h"
#include "app_config.h"
#include "gfx_backend.h"
#include "hle_common.h"
#include "net_lobby.h"
#include "xbox_mem.h"
#include "xiso_vfs.h"
#include "rl_achievements.h"

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
#include <shellapi.h>
#include <d3d11.h>
#include "imgui_impl_dx11.h"
#else
#include <cstdlib>
#endif

#include <algorithm>
#include <atomic>
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
std::atomic<bool> g_imgui_inited{false};
std::atomic<bool> g_ui_wants_kb{false};
std::atomic<bool> g_ui_wants_mouse{false};
SDL_Window* g_sdl_win = nullptr;

std::string g_userdata_root = "userdata";
std::string g_iso_path;
bool g_iso_ok = false;
std::string g_iso_detail;
std::string g_backend_name = "Auto";

std::atomic<bool> g_launcher_active{false};
std::atomic<bool> g_overlay_open{false};
bool g_launch_requested = false;
bool g_quit_requested = false;

void* g_logo_tex = nullptr;
int   g_logo_w = 0, g_logo_h = 0;
void* g_backdrop_tex = nullptr;
int   g_backdrop_w = 0, g_backdrop_h = 0;
bool  g_art_tried = false;
float g_ui_scale = 1.0f;
float g_applied_scale = 0.0f;

float dp(float px) { return px * g_ui_scale; }

void ensure_ui_art_loaded() {
    if (g_art_tried || g_ui_backend == BackendUiType::None) return;
    g_art_tried = true;
    g_logo_tex = gfx_load_ui_texture("assets/logo.png", &g_logo_w, &g_logo_h);
    g_backdrop_tex = gfx_load_ui_texture("assets/backdrop.jpg", &g_backdrop_w, &g_backdrop_h);
}

void destroy_ui_art_textures() {
    if (g_logo_tex) { gfx_destroy_ui_texture(g_logo_tex); g_logo_tex = nullptr; }
    if (g_backdrop_tex) { gfx_destroy_ui_texture(g_backdrop_tex); g_backdrop_tex = nullptr; }
    g_logo_w = g_logo_h = g_backdrop_w = g_backdrop_h = 0;
    g_art_tried = false;
}

void open_folder_in_os(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string p = dir.string();
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open \"" + p + "\" >/dev/null 2>&1 &";
    std::system(cmd.c_str());
#endif
}

int clear_shader_cache_files(const std::string& userdata_root) {
    int removed = 0;
    std::error_code ec;
    fs::path cdir = fs::path(userdata_root) / "cache";
    if (!fs::exists(cdir, ec)) return 0;
    for (const auto& entry : fs::directory_iterator(cdir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string ext = entry.path().extension().string();
        if (ext == ".dxbc" || ext == ".glbin") {
            if (fs::remove(entry.path(), ec)) ++removed;
        }
    }
    return removed;
}

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

void apply_xbox_style(float scale = 1.0f) {
    static ImGuiStyle s_base_style;
    static bool s_base_captured = false;
    ImGuiStyle& s = ImGui::GetStyle();
    if (!s_base_captured) {
        ImGui::StyleColorsDark(&s_base_style);
        s_base_style.WindowPadding     = ImVec2(18.0f, 16.0f);
        s_base_style.FramePadding      = ImVec2(10.0f, 6.0f);
        s_base_style.CellPadding       = ImVec2(8.0f, 5.0f);
        s_base_style.ItemSpacing       = ImVec2(10.0f, 8.0f);
        s_base_style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
        s_base_style.WindowRounding    = 10.0f;
        s_base_style.ChildRounding     = 8.0f;
        s_base_style.FrameRounding     = 6.0f;
        s_base_style.PopupRounding     = 6.0f;
        s_base_style.GrabRounding      = 6.0f;
        s_base_style.TabRounding       = 6.0f;
        s_base_style.WindowBorderSize  = 1.0f;
        s_base_style.ChildBorderSize   = 1.0f;
        s_base_style.FrameBorderSize   = 1.0f;

        ImVec4* c = s_base_style.Colors;
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
        s_base_captured = true;
    }
    scale = std::clamp(scale, 1.0f, 2.25f);
    s = s_base_style;
    s.ScaleAllSizes(scale);
    ImGui::GetIO().FontGlobalScale = scale;
    g_ui_scale = scale;
    g_applied_scale = scale;
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
    ImGui::SetNextItemWidth(260.0f);
    const char* cap_label = (cfg.fps_cap <= 0) ? "Uncapped" : "%d FPS";
    if (ImGui::SliderInt("Frame Rate Cap", &cfg.fps_cap, 0, 240, cap_label)) {
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
    int pad_type = hle_input_get_primary_pad_type();
    const char* l_a = (pad_type == 1) ? "Cross (A)" : (pad_type == 2 ? "B (A)" : "A");
    const char* l_b = (pad_type == 1) ? "Circle (B)" : (pad_type == 2 ? "A (B)" : "B");
    const char* l_x = (pad_type == 1) ? "Square (X)" : (pad_type == 2 ? "Y (X)" : "X");
    const char* l_y = (pad_type == 1) ? "Triangle (Y)" : (pad_type == 2 ? "X (Y)" : "Y");
    const char* l_wht = (pad_type == 1) ? "L1 (WHT)" : (pad_type == 2 ? "L (WHT)" : "WHT");
    const char* l_blk = (pad_type == 1) ? "R1 (BLK)" : (pad_type == 2 ? "R (BLK)" : "BLK");

    char p0_name[128]{};
    bool p0_conn = false;
    hle_input_get_port_info(0, &p0_conn, p0_name, sizeof(p0_name));
    if (p0_conn && p0_name[0]) {
        const char* fam_tag = (pad_type == 1) ? "PlayStation" : (pad_type == 2 ? "Nintendo" : "Xbox");
        ImGui::TextColored(ImVec4(0.32f, 0.92f, 0.50f, 1.0f), "Port 1: %s [%s Glyphs]", p0_name, fam_tag);
    } else {
        ImGui::TextDisabled("Port 1 Live Inputs (Keyboard + Gamepad):");
    }
    led(l_a,   0x100u, ImVec4(0.1f, 0.7f, 0.2f, 1.0f)); ImGui::SameLine();
    led(l_b,   0x200u, ImVec4(0.8f, 0.2f, 0.2f, 1.0f)); ImGui::SameLine();
    led(l_x,   0x400u, ImVec4(0.2f, 0.45f, 0.9f, 1.0f)); ImGui::SameLine();
    led(l_y,   0x800u, ImVec4(0.85f, 0.7f, 0.15f, 1.0f));
    led(l_blk, 0x1000u, ImVec4(0.45f, 0.45f, 0.5f, 1.0f)); ImGui::SameLine();
    led(l_wht, 0x2000u, ImVec4(0.75f, 0.75f, 0.8f, 1.0f)); ImGui::SameLine();

    led("UP", 0x01u, ImVec4(0.2f, 0.8f, 0.4f, 1.0f)); ImGui::SameLine();
    led("DN", 0x02u, ImVec4(0.2f, 0.8f, 0.4f, 1.0f)); ImGui::SameLine();
    led("LF", 0x04u, ImVec4(0.2f, 0.8f, 0.4f, 1.0f)); ImGui::SameLine();
    led("RT", 0x08u, ImVec4(0.2f, 0.8f, 0.4f, 1.0f));
    led("START", 0x10u, ImVec4(0.2f, 0.8f, 0.4f, 1.0f)); ImGui::SameLine();
    led("BACK", 0x20u, ImVec4(0.2f, 0.8f, 0.4f, 1.0f));

    ImGui::Spacing();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Left Deadzone", &cfg.deadzone_left, 0.0f, 0.45f, "%.2f");
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Right Deadzone", &cfg.deadzone_right, 0.0f, 0.45f, "%.2f");
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Right Stick Sens", &cfg.cam_sensitivity, 0.25f, 2.50f, "%.2fx");
    ImGui::Checkbox("Invert Camera Y Axis", &cfg.invert_cam_y);
    const char* presets[] = {"Auto (Hardware Detect)", "PlayStation (Cross/Circle)", "Nintendo (Swap A/B, X/Y)"};
    ImGui::SetNextItemWidth(220.0f);
    ImGui::Combo("Glyph / Button Preset", &cfg.pad_preset, presets, 3);
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

void render_system_tools_tab() {
    ImGui::TextColored(ImVec4(0.24f, 0.90f, 0.45f, 1.0f), "Runtime Diagnostics & Quick Utilities");
    ImGui::Separator();
    ImGui::Spacing();

    double mem_mb = static_cast<double>(xbox_mem_used_bytes()) / (1024.0 * 1024.0);
    int draw_calls = gfx_get_last_draw_calls();

    if (ImGui::BeginChild("DiagMetrics", ImVec2(0, dp(122.0f)), true)) {
        ImGui::Columns(2, nullptr, false);
        ImGui::Text("Active Backend:   %s", g_backend_name.c_str());
        ImGui::Text("UI Scale Factor:  %.2fx", g_ui_scale);
        ImGui::Text("Draw Calls/Frame: %d", draw_calls);
        ImGui::NextColumn();
        ImGui::Text("Guest Arena RAM:  %.1f MB / 208.0 MB", mem_mb);
        ImGui::Text("UserData Root:    %s", g_userdata_root.c_str());
        if (rl_ach_connected()) {
            ImGui::TextColored(ImVec4(0.32f, 0.92f, 0.50f, 1.0f), "RecompAchievements: Connected");
        } else {
            ImGui::TextDisabled("RecompAchievements: Standalone Mode");
        }
        ImGui::Columns(1);
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.24f, 0.90f, 0.45f, 1.0f), "Folders & Shader Cache");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Open Saves / UDATA", ImVec2(dp(170.0f), dp(30.0f)))) {
        open_folder_in_os(fs::path(g_userdata_root) / "UDATA");
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Screenshots", ImVec2(dp(160.0f), dp(30.0f)))) {
        open_folder_in_os(fs::path(g_userdata_root) / "screenshots");
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Logs", ImVec2(dp(120.0f), dp(30.0f)))) {
        open_folder_in_os(fs::path(g_userdata_root) / "logs");
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Shader Cache", ImVec2(dp(165.0f), dp(30.0f)))) {
        int removed = clear_shader_cache_files(g_userdata_root);
        set_toast("Cleared " + std::to_string(removed) + " cached shader files.", 3.5);
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Hotkeys: [F1] / [Guide] / [L3+R3] Menu  |  [F11] Fullscreen  |  [F12] Screenshot");
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

    // Hero Header Banner (with shipped artwork backdrop + logo when present)
    {
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        float banner_h = dp(92.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (g_backdrop_tex && g_backdrop_w > 0 && g_backdrop_h > 0) {
            float target_aspect = w / std::max(1.0f, banner_h);
            float src_aspect = static_cast<float>(g_backdrop_w) / static_cast<float>(g_backdrop_h);
            float u0 = 0.0f, u1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
            if (src_aspect < target_aspect) {
                float vh = src_aspect / target_aspect;
                v0 = 0.5f - vh * 0.5f;
                v1 = 0.5f + vh * 0.5f;
            } else {
                float uw = target_aspect / src_aspect;
                u0 = 0.5f - uw * 0.5f;
                u1 = 0.5f + uw * 0.5f;
            }
            dl->AddImage((ImTextureID)(uintptr_t)g_backdrop_tex, p0, ImVec2(p0.x + w, p0.y + banner_h),
                         ImVec2(u0, v0), ImVec2(u1, v1), IM_COL32(255, 255, 255, 115));
        }
        dl->AddRectFilledMultiColor(p0, ImVec2(p0.x + w, p0.y + banner_h),
            IM_COL32(12, 74, 30, 195), IM_COL32(10, 32, 22, 180),
            IM_COL32(8, 12, 18, 232), IM_COL32(10, 24, 18, 220));
        dl->AddRect(p0, ImVec2(p0.x + w, p0.y + banner_h), IM_COL32(42, 190, 84, 195), dp(8.0f), 0, 1.5f);

        float text_x = p0.x + dp(18.0f);
        if (g_logo_tex && g_logo_w > 0 && g_logo_h > 0) {
            float lh = banner_h - dp(14.0f);
            float lw = lh * (static_cast<float>(g_logo_w) / static_cast<float>(g_logo_h));
            lw = std::min(lw, dp(220.0f));
            ImVec2 lp0(p0.x + dp(16.0f), p0.y + (banner_h - lh) * 0.5f);
            dl->AddImage((ImTextureID)(uintptr_t)g_logo_tex, lp0, ImVec2(lp0.x + lw, lp0.y + lh));
            text_x = lp0.x + lw + dp(18.0f);
            ImGui::SetCursorScreenPos(ImVec2(text_x, p0.y + dp(18.0f)));
            ImGui::TextColored(ImVec4(0.84f, 0.96f, 0.88f, 1.0f), "Original Xbox Static Recompilation  |  PC & Linux Edition");
        } else {
            ImGui::SetCursorScreenPos(ImVec2(text_x, p0.y + dp(14.0f)));
            ImGui::SetWindowFontScale(1.65f);
            ImGui::TextColored(ImVec4(0.98f, 0.84f, 0.28f, 1.0f), "WHACKED!");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::SameLine(0, dp(14.0f));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + dp(8.0f));
            ImGui::TextColored(ImVec4(0.75f, 0.94f, 0.80f, 1.0f), "Original Xbox Static Recompilation  |  PC & Linux Edition");
        }

        ImGui::SetCursorScreenPos(ImVec2(text_x, p0.y + dp(52.0f)));
        ImGui::TextColored(ImVec4(0.72f, 0.82f, 0.78f, 1.0f), "Renderer: %s   |   ISO: %s",
                           g_backend_name.c_str(),
                           g_iso_ok ? "Ready (Mounted)" : "Missing / Select XISO");
        if (rl_ach_connected()) {
            ImGui::SameLine(0, dp(16.0f));
            ImGui::TextColored(ImVec4(0.32f, 0.92f, 0.50f, 1.0f), "|  RecompAchievements: Active");
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + banner_h + dp(10.0f)));
    }

    float footer_h = dp(58.0f);
    float body_h = std::max(dp(200.0f), ImGui::GetContentRegionAvail().y - footer_h);

    if (ImGui::BeginChild("LauncherBody", ImVec2(0, body_h), false)) {
        if (ImGui::BeginTabBar("LauncherTabs")) {
            if (ImGui::BeginTabItem("  Play & Setup  ")) {
                ImGui::Spacing();
                ImGui::BeginChild("IsoCard", ImVec2(0, dp(g_iso_ok ? 174.0f : 238.0f)), true);
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
                ImGui::TextDisabled("Data Folder: %s (%s)",
                                    g_userdata_root.c_str(),
                                    has_saves ? "Existing Save Profiles Present" : "Clean Profile Ready");
                ImGui::TextDisabled("Tip: Drag & drop any .iso / .xiso.iso disc image or extracted folder onto this window.");
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
            if (ImGui::BeginTabItem("  Tools & Diagnostics  ")) {
                ImGui::Spacing();
                render_system_tools_tab();
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
    } else {
        int pt = hle_input_get_primary_pad_type();
        const char* nav_hint = (pt == 1)
            ? "  |  [D-Pad] Move  [Cross] Select  [Circle] Back"
            : (pt == 2 ? "  |  [D-Pad] Move  [B] Select  [A] Back"
                       : "  |  [D-Pad] Move  [A] Select  [B] Back");
        ImGui::TextDisabled("%s", nav_hint);
    }

    float right_x = ImGui::GetWindowWidth() - dp(415.0f);
    if (right_x > ImGui::GetCursorPosX() + dp(10.0f)) {
        ImGui::SameLine(right_x);
    } else {
        ImGui::SameLine();
    }

    if (ImGui::Button("Save Settings", ImVec2(dp(120.0f), dp(34.0f)))) {
        cfg.iso_path = g_iso_buf;
        cfg.player_name = g_name_buf;
        app_config_save(g_userdata_root);
        set_toast("Configuration saved!");
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(dp(82.0f), dp(34.0f)))) {
        g_quit_requested = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_iso_ok);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.11f, 0.56f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.72f, 0.32f, 1.0f));
    if (ImGui::Button("PLAY WHACKED!", ImVec2(dp(175.0f), dp(34.0f)))) {
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

    ImVec2 win_sz(std::min(dp(880.0f), vp->Size.x * 0.88f), std::min(dp(580.0f), vp->Size.y * 0.88f));
    ImGui::SetNextWindowSize(win_sz, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - win_sz.x) * 0.5f,
                                   vp->Pos.y + (vp->Size.y - win_sz.y) * 0.5f), ImGuiCond_Always);

    ImGuiWindowFlags owf = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    bool overlay_open = g_overlay_open.load(std::memory_order_acquire);
    bool win_shown = ImGui::Begin("Whacked! In-Game Menu   [F1 / Xbox Guide / L3+R3 to Close]", &overlay_open, owf);
    g_overlay_open.store(overlay_open, std::memory_order_release);
    if (win_shown) {
        float body_h = std::max(dp(180.0f), ImGui::GetContentRegionAvail().y - dp(46.0f));
        if (ImGui::BeginChild("OverlayBody", ImVec2(0, body_h), false)) {
            if (ImGui::BeginTabBar("OverlayTabs")) {
                if (ImGui::BeginTabItem("Graphics")) { render_graphics_tab(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Audio")) { render_audio_tab(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Controllers")) { render_controllers_tab(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Multiplayer")) { render_multiplayer_tab(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("System & Cache")) { render_system_tools_tab(); ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (ImGui::Button("Resume [F1]", ImVec2(dp(120.0f), dp(30.0f)))) g_overlay_open = false;
        ImGui::SameLine();
        if (ImGui::Button("Screenshot [F12]", ImVec2(dp(140.0f), dp(30.0f)))) {
            fs::path sdir = fs::path(g_userdata_root) / "screenshots";
            std::error_code ec;
            fs::create_directories(sdir, ec);
            auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::string path = (sdir / ("whacked_" + std::to_string(t) + ".png")).string();
            gfx_request_screenshot(path);
            set_toast("Saved screenshot to " + path);
        }
        ImGui::SameLine();
        if (ImGui::Button("Save Settings", ImVec2(dp(120.0f), dp(30.0f)))) {
            app_config_save(g_userdata_root);
            set_toast("Settings saved!");
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit Game", ImVec2(dp(100.0f), dp(30.0f)))) {
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
    if (!g_imgui_inited.exchange(false, std::memory_order_acq_rel)) return;
    g_ui_wants_kb.store(false, std::memory_order_release);
    g_ui_wants_mouse.store(false, std::memory_order_release);
    destroy_ui_art_textures();
#ifdef _WIN32
    if (g_ui_backend == BackendUiType::DX11) ImGui_ImplDX11_Shutdown();
#endif
    if (g_ui_backend == BackendUiType::OpenGL3) ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    g_ui_backend = BackendUiType::None;
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
    return g_imgui_inited.load(std::memory_order_acquire) && g_ui_wants_kb.load(std::memory_order_acquire);
}

bool ui_wants_mouse() {
    return g_imgui_inited.load(std::memory_order_acquire) && g_ui_wants_mouse.load(std::memory_order_acquire);
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
    if (!g_launcher_active.load(std::memory_order_acquire)) {
        g_overlay_open.store(!g_overlay_open.load(std::memory_order_acquire), std::memory_order_release);
    }
}
bool ui_is_overlay_open() { return g_overlay_open; }

void ui_render_frame(bool is_opengl) {
    if (!g_imgui_inited) return;
    ensure_ui_art_loaded();

    std::string hp_msg;
    if (hle_input_consume_hotplug_notice(hp_msg)) {
        set_toast(hp_msg, 3.5);
    }

    if (g_sdl_win) {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(g_sdl_win, &ww, &wh);
        float want_scale = (ww > 0 && wh > 0)
            ? std::clamp(std::min(static_cast<float>(ww) / 1280.0f, static_cast<float>(wh) / 720.0f), 1.0f, 2.25f)
            : 1.0f;
        if (std::fabs(want_scale - g_applied_scale) > 0.05f) {
            apply_xbox_style(want_scale);
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    if (g_launcher_active || g_overlay_open) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    }
    g_ui_wants_kb.store((g_launcher_active || g_overlay_open) && io.WantCaptureKeyboard, std::memory_order_release);
    g_ui_wants_mouse.store((g_launcher_active || g_overlay_open) && io.WantCaptureMouse, std::memory_order_release);

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
