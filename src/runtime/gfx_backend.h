#pragma once

#include <cstdint>
#include <string>

struct Nv2aCompiledVS;
struct Nv2aCompiledPS;

enum class GfxBackendKind : int {
    Auto = 0,
    D3D11 = 1,
    OpenGL = 2,
};

struct RendererSettings {
    int width = 1280;
    int height = 720;
    bool widescreen = true;
    bool unstretched_hud = false;
    bool msaa_4x = true;
    bool aniso_16x = true;
    bool bloom_sharpen = true;
    bool vsync = true;
    int fps_cap = 60;
    bool show_overlay = false;
    bool show_fps = true;
    GfxBackendKind backend = GfxBackendKind::Auto;
    std::string screenshot_dir;
    std::string cache_dir;
};

struct GpuTextureObj {
    uint32_t guest_va = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t levels = 1;
    uint32_t xb_format = 0;
    uint32_t pitch = 0;
    uint32_t data_va = 0;
    bool is_cube = false;
    bool is_volume = false;
    bool is_render_target = false;
    bool is_depth = false;
    bool dirty = true;

    // D3D11 opaque handles (cast in d3d11_renderer.cpp)
    void* dx_tex = nullptr;
    void* dx_srv = nullptr;
    void* dx_rtv = nullptr;
    void* dx_dsv = nullptr;

    // OpenGL handles
    uint32_t gl_tex = 0;
    uint32_t gl_fbo = 0;
    uint32_t gl_rbo = 0;
    bool     gl_is_cube = false;
};

struct DrawStateSnapshot {
    const Nv2aCompiledVS* vs = nullptr;
    const Nv2aCompiledPS* ps = nullptr;
    bool is_programmable_vs = false;
    bool is_programmable_ps = false;

    uint32_t prim_type = 5; // Xbox D3DPT_TRIANGLELIST
    const void* vb_ptrs[4]{};
    uint32_t vb_strides[4]{};
    uint32_t vb_offsets[4]{};
    uint32_t vb_guest_va[4]{};
    uint32_t vb_total_bytes[4]{};
    uint32_t vb_dirty_seq[4]{};

    const uint16_t* ib_ptr = nullptr;
    uint32_t ib_guest_va = 0;
    uint32_t ib_total_bytes = 0;
    uint32_t ib_dirty_seq = 0;
    uint32_t index_count = 0;
    uint32_t vertex_count = 0;
    int32_t  base_vertex = 0;

    // Constants & transforms
    const float* vs_consts = nullptr; // 192 * 4 floats
    const float* ps_consts = nullptr; // 16 * 4 floats
    const float* world_m = nullptr;   // 16 floats
    const float* view_m = nullptr;
    const float* proj_m = nullptr;
    float mat_diffuse[4]{1,1,1,1};
    float mat_ambient[4]{1,1,1,1};
    float mat_specular[4]{0,0,0,16.0f};
    float mat_emissive[4]{0,0,0,1};
    float global_ambient[4]{0.2f,0.2f,0.2f,1.0f};

    // Up to 4 active fixed-function lights
    float light_pos[4][4]{};   // xyz = pos, w = type (0=off,1=point,2=spot,3=dir)
    float light_dir[4][4]{};   // xyz = dir, w = enabled (0/1)
    float light_diff[4][4]{};
    float light_spec[4][4]{};
    float light_atten[4][4]{}; // range, a0, a1, a2

    float tex_factor[4]{1,1,1,1};
    float fog_color[4]{0,0,0,1};
    bool fog_enable = false;
    uint32_t fog_mode = 3; // LINEAR
    float fog_start = 0.0f;
    float fog_end = 1.0f;
    float fog_density = 1.0f;

    bool lighting_en = false;
    bool specular_en = false;
    bool alpha_blend = false;
    uint32_t src_blend = 2; // ONE
    uint32_t dst_blend = 1; // ZERO
    uint32_t blend_op = 1;  // ADD
    bool z_enable = true;
    bool z_write = true;
    uint32_t z_func = 4; // LESSEQUAL

    bool stencil_enable = false;
    uint32_t stencil_func = 8; // ALWAYS
    uint32_t stencil_ref = 0;
    uint32_t stencil_mask = 0xFFFFFFFFu;
    uint32_t stencil_writemask = 0xFFFFFFFFu;
    uint32_t stencil_fail = 1;  // KEEP
    uint32_t stencil_zfail = 1; // KEEP
    uint32_t stencil_pass = 1;  // KEEP

    uint32_t cull_mode = 3; // CCW
    uint32_t fill_mode = 3; // SOLID
    bool alpha_test = false;
    uint32_t alpha_func = 8;
    float alpha_ref = 0.0f;
    uint32_t color_write_mask = 0xF;

    float vp_x = 0, vp_y = 0, vp_w = 640, vp_h = 480, vp_minz = 0.0f, vp_maxz = 1.0f;
    float xb_fb_w = 640.0f, xb_fb_h = 480.0f;

    // Textures & samplers [0..3]
    GpuTextureObj* tex[4]{};
    uint32_t addr_u[4]{1,1,1,1};
    uint32_t addr_v[4]{1,1,1,1};
    uint32_t min_filter[4]{2,2,2,2};
    uint32_t mag_filter[4]{2,2,2,2};

    uint32_t current_palette_va = 0;
    GpuTextureObj* custom_rt = nullptr;
};

// Unified cross-platform renderer API (backed by D3D11 on Windows or OpenGL 3.3 Core on Linux/Windows)
bool gfx_init(void* sdl_window, const RendererSettings& settings);
void gfx_shutdown();
void gfx_resize(int width, int height);
void gfx_set_fullscreen(bool fs);
void gfx_release_context_for_game();
bool gfx_is_opengl();
RendererSettings& gfx_settings();
double gfx_get_fps();
int gfx_get_last_draw_calls();

void gfx_clear(uint32_t flags, uint32_t color_argb, float z, uint32_t stencil, GpuTextureObj* custom_rt = nullptr);
void gfx_draw(const DrawStateSnapshot& st);
void gfx_upload_texture(GpuTextureObj* t, uint32_t palette_va);
void gfx_blit_rgba_rect(const void* bgra, int src_w, int src_h, int dst_x, int dst_y, int dst_w, int dst_h);
void gfx_present();
void gfx_request_screenshot(const std::string& filename);

// Portable PNG writer (implemented via stb_image_write)
bool gfx_save_png_rgba(const std::string& path, int w, int h, const uint8_t* rgba, int stride_bytes);
