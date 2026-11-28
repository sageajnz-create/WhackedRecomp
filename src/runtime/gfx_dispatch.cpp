#include "gfx_backend.h"
#include "nv2a_shaders.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "d3d11_renderer.h"
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

extern bool gl_init(void* sdl_win, const RendererSettings& settings);
extern void gl_shutdown();
extern void gl_resize(int width, int height);
extern void gl_set_fullscreen(bool fs);
extern void gl_release_context();
extern void gl_ensure_current();
extern RendererSettings& gl_settings();
extern double gl_get_fps();
extern int gl_get_last_draw_calls();
extern void gl_clear(uint32_t flags, uint32_t color_argb, float z, uint32_t stencil, GpuTextureObj* custom_rt);
extern void gl_draw(const DrawStateSnapshot& st);
extern void gl_upload_texture(GpuTextureObj* t, uint32_t palette_va);
extern void gl_blit_rgba_rect(const void* bgra, int src_w, int src_h, int dst_x, int dst_y, int dst_w, int dst_h);
extern void gl_present();
extern void gl_request_screenshot(const std::string& filename);
extern void gl_retire_texture(GpuTextureObj* tex);
extern void gl_retire_buffer(uint32_t obj_va);
extern void* gl_create_ui_texture_rgba(const uint8_t* rgba, int w, int h);
extern void  gl_destroy_ui_texture(void* tex_handle);

namespace {
bool g_active_is_gl = false;
RendererSettings g_fallback_cfg{};
std::atomic<bool> g_shutting_down{false};
std::atomic<int>  g_gfx_busy{0};

#ifdef _WIN32
using TimePeriodFn = UINT(WINAPI*)(UINT);
void set_win32_timer_period_1ms(bool enable) {
    static HMODULE g_winmm = LoadLibraryW(L"winmm.dll");
    if (!g_winmm) return;
    const char* name = enable ? "timeBeginPeriod" : "timeEndPeriod";
    if (auto fn = reinterpret_cast<TimePeriodFn>(GetProcAddress(g_winmm, name))) {
        fn(1);
    }
}
#endif

struct GfxBusyGuard {
    bool entered = false;
    GfxBusyGuard() {
        g_gfx_busy.fetch_add(1, std::memory_order_acq_rel);
        entered = !g_shutting_down.load(std::memory_order_acquire);
    }
    ~GfxBusyGuard() {
        g_gfx_busy.fetch_sub(1, std::memory_order_release);
    }
};
} // namespace

bool gfx_init(void* sdl_window, const RendererSettings& settings) {
#ifdef _WIN32
    set_win32_timer_period_1ms(true);
    if (settings.backend == GfxBackendKind::OpenGL) {
        g_active_is_gl = true;
        return gl_init(sdl_window, settings);
    }
    g_active_is_gl = false;
    return d3d11_init(sdl_window, settings);
#else
    g_active_is_gl = true;
    return gl_init(sdl_window, settings);
#endif
}

void gfx_shutdown() {
    g_shutting_down.store(true, std::memory_order_release);
    for (int i = 0; i < 200 && g_gfx_busy.load(std::memory_order_acquire) > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#ifdef _WIN32
    if (!g_active_is_gl) d3d11_shutdown();
    else gl_shutdown();
    set_win32_timer_period_1ms(false);
#else
    gl_shutdown();
#endif
}

void gfx_resize(int width, int height) {
#ifdef _WIN32
    if (!g_active_is_gl) { d3d11_resize(width, height); return; }
#endif
    gl_resize(width, height);
}

void gfx_set_fullscreen(bool fs) {
#ifdef _WIN32
    if (!g_active_is_gl) { d3d11_set_fullscreen(fs); return; }
#endif
    gl_set_fullscreen(fs);
}

void gfx_release_context_for_game() {
    if (g_active_is_gl) gl_release_context();
}

void gfx_ensure_gl_current() {
    if (g_active_is_gl && !g_shutting_down.load(std::memory_order_acquire)) gl_ensure_current();
}

bool gfx_is_opengl() {
    return g_active_is_gl;
}

RendererSettings& gfx_settings() {
#ifdef _WIN32
    if (!g_active_is_gl) return d3d11_settings();
#endif
    return gl_settings();
}

double gfx_get_fps() {
#ifdef _WIN32
    if (!g_active_is_gl) return d3d11_get_fps();
#endif
    return gl_get_fps();
}

int gfx_get_last_draw_calls() {
#ifdef _WIN32
    if (!g_active_is_gl) return d3d11_get_last_draw_calls();
#endif
    return gl_get_last_draw_calls();
}

void gfx_clear(uint32_t flags, uint32_t color_argb, float z, uint32_t stencil, GpuTextureObj* custom_rt) {
    GfxBusyGuard guard;
    if (!guard.entered) return;
#ifdef _WIN32
    if (!g_active_is_gl) { d3d11_clear(flags, color_argb, z, stencil, custom_rt); return; }
#endif
    gl_clear(flags, color_argb, z, stencil, custom_rt);
}

void gfx_draw(const DrawStateSnapshot& st) {
    GfxBusyGuard guard;
    if (!guard.entered) return;
#ifdef _WIN32
    if (!g_active_is_gl) { d3d11_draw(st); return; }
#endif
    gl_draw(st);
}

void gfx_upload_texture(GpuTextureObj* t, uint32_t palette_va) {
    GfxBusyGuard guard;
    if (!guard.entered) return;
#ifdef _WIN32
    if (!g_active_is_gl) { d3d11_upload_texture(t, palette_va); return; }
#endif
    gl_upload_texture(t, palette_va);
}

void gfx_blit_rgba_rect(const void* bgra, int src_w, int src_h, int dst_x, int dst_y, int dst_w, int dst_h) {
    GfxBusyGuard guard;
    if (!guard.entered) return;
#ifdef _WIN32
    if (!g_active_is_gl) { d3d11_blit_rgba_rect(bgra, src_w, src_h, dst_x, dst_y, dst_w, dst_h); return; }
#endif
    gl_blit_rgba_rect(bgra, src_w, src_h, dst_x, dst_y, dst_w, dst_h);
}

void gfx_present() {
    GfxBusyGuard guard;
    if (!guard.entered) return;
#ifdef _WIN32
    if (!g_active_is_gl) { d3d11_present(); return; }
#endif
    gl_present();
}

void gfx_request_screenshot(const std::string& filename) {
#ifdef _WIN32
    if (!g_active_is_gl) { d3d11_request_screenshot(filename); return; }
#endif
    gl_request_screenshot(filename);
}

void gfx_retire_texture(GpuTextureObj* tex) {
    if (!tex) return;
    if (g_shutting_down.load(std::memory_order_acquire)) { delete tex; return; }
#ifdef _WIN32
    if (!g_active_is_gl) { d3d11_retire_texture(tex); return; }
#endif
    gl_retire_texture(tex);
}

void gfx_retire_buffer(uint32_t obj_va) {
    if (!obj_va || g_shutting_down.load(std::memory_order_acquire)) return;
#ifdef _WIN32
    if (!g_active_is_gl) { d3d11_retire_buffer(obj_va); return; }
#endif
    gl_retire_buffer(obj_va);
}

bool gfx_upscale_texture(const DrawStateSnapshot& st, int stage) {
    const auto* texture = st.tex[stage];
    return texture && texture->width > 1 && texture->height > 1 &&
           !texture->is_cube && !texture->is_volume &&
           (texture->is_bink_frame ||
            (!st.custom_rt && st.vs && st.vs->is_screenspace && !texture->is_render_target));
}

GfxTransformParams gfx_compute_transform_params(const DrawStateSnapshot& st, float rt_w, float rt_h,
                                                bool widescreen_cfg, bool unstretched_hud_cfg) {
    GfxTransformParams out{};
    if (st.custom_rt) {
        out.ss_w = static_cast<float>(std::max<uint32_t>(1u, st.custom_rt->width));
        out.ss_h = static_cast<float>(std::max<uint32_t>(1u, st.custom_rt->height));
        out.scale_x = 1.0f;
        return out;
    }

    float target_asp = rt_w / std::max(1.0f, rt_h);
    bool is_ss = (st.vs && st.vs->is_screenspace);

    if (is_ss) {
        // FrontEnd / menu / loading cards draw in virtual 640x480, while in-game combat HUD
        // using the same programmable shader emits coordinates scaled to guest FB (e.g. 1280x720).
        bool virtual_480 = st.is_programmable_vs && !st.ss_prog_use_fb;
        out.ss_w = virtual_480 ? 640.0f : st.xb_fb_w;
        out.ss_h = virtual_480 ? 480.0f : st.xb_fb_h;
        bool ss_16x9 = !virtual_480 && (st.xb_widescreen || (out.ss_w / std::max(1.0f, out.ss_h)) > 1.5f);
        float ui_base_asp = ss_16x9 ? (16.0f / 9.0f) : (4.0f / 3.0f);
        out.scale_x = unstretched_hud_cfg ? std::min(1.0f, ui_base_asp / std::max(0.5f, target_asp)) : 1.0f;
        return out;
    }

    out.ss_w = st.xb_fb_w;
    out.ss_h = st.xb_fb_h;
    bool guest_16x9 = st.xb_widescreen || (st.xb_fb_w / std::max(1.0f, st.xb_fb_h)) > 1.5f;
    float guest_3d_asp = guest_16x9 ? (16.0f / 9.0f) : (4.0f / 3.0f);
    if (widescreen_cfg) {
        out.scale_x = guest_3d_asp / std::max(0.5f, target_asp);
    } else {
        out.scale_x = (4.0f / 3.0f) / std::max(0.5f, target_asp);
    }
    return out;
}

void gfx_pace_frame(int fps_cap) {
    using clock = std::chrono::steady_clock;
    static clock::time_point s_last_release{};
    if (fps_cap <= 0) {
        s_last_release = clock::time_point{};
        return;
    }

    auto period = std::chrono::nanoseconds(1000000000LL / std::max(1, fps_cap));
    auto now = clock::now();
    const auto deadline = s_last_release == clock::time_point{} ? now : s_last_release + period;
    while (now < deadline) {
        auto rem_us = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
        if (rem_us > 2000) {
            std::this_thread::sleep_for(std::chrono::milliseconds((rem_us - 1500) / 1000));
        } else {
            std::this_thread::yield();
        }
        now = clock::now();
    }
    s_last_release = now;
}

bool gfx_save_png_rgba(const std::string& path, int w, int h, const uint8_t* rgba, int stride_bytes) {
    if (!rgba || w <= 0 || h <= 0) return false;
    std::error_code ec;
    fs::path fp(path);
    if (fp.has_parent_path()) fs::create_directories(fp.parent_path(), ec);
    return stbi_write_png(path.c_str(), w, h, 4, rgba, stride_bytes) != 0;
}

void* gfx_load_ui_texture(const std::string& rel_path, int* out_w, int* out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (rel_path.empty()) return nullptr;
    std::error_code ec;
    fs::path found;
    std::vector<fs::path> probe{fs::path(rel_path)};
    if (const char* base = SDL_GetBasePath()) {
        fs::path bp(base);
        probe.push_back(bp / rel_path);
        probe.push_back(bp / ".." / rel_path);
        probe.push_back(bp / ".." / ".." / rel_path);
    }
    probe.push_back(fs::current_path(ec) / rel_path);
    probe.push_back(fs::current_path(ec) / ".." / rel_path);
    for (const auto& p : probe) {
        if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
            found = p;
            break;
        }
    }
    if (found.empty()) return nullptr;
    int w = 0, h = 0, ch = 0;
    unsigned char* rgba = stbi_load(found.string().c_str(), &w, &h, &ch, 4);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) stbi_image_free(rgba);
        return nullptr;
    }
    void* handle = nullptr;
#ifdef _WIN32
    if (!g_active_is_gl) {
        handle = d3d11_create_ui_texture_rgba(rgba, w, h);
    } else
#endif
    {
        handle = gl_create_ui_texture_rgba(rgba, w, h);
    }
    stbi_image_free(rgba);
    if (handle) {
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
    }
    return handle;
}

void gfx_destroy_ui_texture(void* tex_handle) {
    if (!tex_handle) return;
#ifdef _WIN32
    if (!g_active_is_gl) {
        d3d11_destroy_ui_texture(tex_handle);
        return;
    }
#endif
    gl_destroy_ui_texture(tex_handle);
}
