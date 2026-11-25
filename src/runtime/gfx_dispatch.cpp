#include "gfx_backend.h"
#ifdef _WIN32
#include "d3d11_renderer.h"
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
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
extern RendererSettings& gl_settings();
extern double gl_get_fps();
extern int gl_get_last_draw_calls();
extern void gl_clear(uint32_t flags, uint32_t color_argb, float z, uint32_t stencil, GpuTextureObj* custom_rt);
extern void gl_draw(const DrawStateSnapshot& st);
extern void gl_upload_texture(GpuTextureObj* t, uint32_t palette_va);
extern void gl_blit_rgba_rect(const void* bgra, int src_w, int src_h, int dst_x, int dst_y, int dst_w, int dst_h);
extern void gl_present();
extern void gl_request_screenshot(const std::string& filename);

namespace {
bool g_active_is_gl = false;
RendererSettings g_fallback_cfg{};
std::atomic<bool> g_shutting_down{false};
std::atomic<int>  g_gfx_busy{0};

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
    if (!g_active_is_gl) { d3d11_shutdown(); return; }
#endif
    gl_shutdown();
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

bool gfx_save_png_rgba(const std::string& path, int w, int h, const uint8_t* rgba, int stride_bytes) {
    if (!rgba || w <= 0 || h <= 0) return false;
    std::error_code ec;
    fs::path fp(path);
    if (fp.has_parent_path()) fs::create_directories(fp.parent_path(), ec);
    return stbi_write_png(path.c_str(), w, h, 4, rgba, stride_bytes) != 0;
}
