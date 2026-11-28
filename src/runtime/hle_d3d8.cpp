#include "hle_common.h"
#include "launcher_ui.h"
#include "gfx_backend.h"
#include "nv2a_shaders.h"
#include "rl_achievements.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// External frame counter / automation hooks in main.cpp
extern void app_on_frame_presented(uint64_t frame_num);
extern void fn_000a0d10(CpuContext& ctx);

namespace {

// Well-known Xbox Whacked! D3D8 globals in guest RAM
constexpr uint32_t kGuestDevicePtrVA   = 0x002529B0u;
constexpr uint32_t kGuestIndexBaseVA   = 0x002529B8u;
constexpr uint32_t kGuestDirtyFlagsVA  = 0x002529BCu;
constexpr uint32_t kGuestTSSBaseVA     = 0x002529C0u; // 4 stages x 32 DWORDs
constexpr uint32_t kGuestRSBaseVA      = 0x00252BC0u; // 170 DWORDs (0..169)
constexpr uint32_t kGuestVSConstVA     = 0x00253DD8u; // 192 * 4 floats

// Xbox D3D Render State IDs verified against D3D__RenderStateMap (0x2C3630 & 0x235100..0x236FF0)
enum XbRenderState : uint32_t {
    XBRS_ZFUNC              = 57,  // 0x0354
    XBRS_ALPHAFUNC          = 58,  // 0x033C
    XBRS_ALPHABLENDENABLE   = 59,  // 0x0304
    XBRS_ALPHATESTENABLE    = 60,  // 0x0300
    XBRS_ALPHAREF           = 61,  // 0x0340
    XBRS_SRCBLEND           = 62,  // 0x0344
    XBRS_DESTBLEND          = 63,  // 0x0348
    XBRS_ZWRITEENABLE       = 64,  // 0x035C
    XBRS_DITHERENABLE       = 65,  // 0x0310
    XBRS_SHADEMODE          = 66,  // 0x037C
    XBRS_COLORWRITEENABLE   = 67,  // 0x0358
    XBRS_STENCILZFAIL       = 68,  // 0x0374
    XBRS_STENCILPASS        = 69,  // 0x0378
    XBRS_STENCILFUNC        = 70,  // 0x0364
    XBRS_STENCILREF         = 71,  // 0x0368
    XBRS_STENCILMASK        = 72,  // 0x036C
    XBRS_STENCILWRITEMASK   = 73,  // 0x0360
    XBRS_BLENDOP            = 74,  // 0x0350
    XBRS_POLYGONOFFSETZSLOPESCALE = 77, // 0x4D -> 0x0384
    XBRS_POLYGONOFFSETZOFFSET     = 78, // 0x4E -> 0x0388
    XBRS_POINTOFFSETENABLE        = 79, // 0x4F -> 0x0330
    XBRS_WIREFRAMEOFFSETENABLE    = 80, // 0x50 -> 0x0334
    XBRS_SOLIDOFFSETENABLE        = 81, // 0x51 -> 0x0338
    XBRS_SPECULARENABLE     = 82,  // 0x1D78
    XBRS_LIGHTING           = 136, // 0x88
    XBRS_STENCILENABLE      = 137, // 0x89
    XBRS_TEXTUREFACTOR      = 138, // 0x8A
    XBRS_FILLMODE           = 139, // 0x8B -> 0x038C (front polygon mode)
    XBRS_BACKFILLMODE       = 140, // 0x8C -> 0x0390 (back polygon mode)
    XBRS_TWOSIDEDLIGHTING   = 141, // 0x8D
    XBRS_NORMALIZENORMALS   = 142, // 0x8E -> 0x03A4
    XBRS_ZENABLE            = 143, // 0x8F
    XBRS_FOGENABLE          = 144, // 0x90
    XBRS_FOGTABLEMODE       = 145, // 0x91
    XBRS_FRONTFACE          = 146, // 0x92 -> 0x03A0
    XBRS_CULLMODE           = 147, // 0x93 -> 0x0308 + 0x039C
    XBRS_SPECULARENABLE_RS  = 151, // 0x97
    XBRS_FOGCOLOR           = 156, // 0x9C
    XBRS_FOGDENSITY         = 157, // 0x9D
    XBRS_YUVENABLE          = 159, // 0x9F
    XBRS_FOGSTART           = 160, // 0xA0
    XBRS_FOGEND             = 161, // 0xA1
    XBRS_STENCILFAIL        = 166, // explicit NV097 0x0370 slot (distinct from 0x8B..0xA5)
};

// Xbox D3D Texture Stage State IDs observed at 0x0011A0C0
enum XbTextureStageState : uint32_t {
    XBTSS_ADDRESSU          = 0,
    XBTSS_ADDRESSV          = 1,
    XBTSS_ADDRESSW          = 2,
    XBTSS_MAGFILTER         = 3,
    XBTSS_MINFILTER         = 4,
    XBTSS_MIPFILTER         = 5,
    XBTSS_BORDERCOLOR       = 9,
    XBTSS_COLOROP           = 12,
    XBTSS_COLORARG0         = 13,
    XBTSS_COLORARG1         = 14,
    XBTSS_COLORARG2         = 15,
    XBTSS_ALPHAOP           = 16,
    XBTSS_ALPHAARG0         = 17,
    XBTSS_ALPHAARG1         = 18,
    XBTSS_ALPHAARG2         = 19,
    XBTSS_RESULTARG         = 20,
    XBTSS_TEXTURETRANSFORMFLAGS = 21,
};

struct GuestSurface {
    uint32_t surf_va = 0;
    uint32_t parent_tex_va = 0;
    GpuTextureObj* gpu = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t xb_format = 0x1Eu;
    uint32_t pitch = 0;
    uint32_t data_va = 0;
    uint32_t level = 0;
    bool     owns_gpu = false;
    bool     locked_ever = false;
    uint64_t last_upload_frame = 0;
};

struct GuestTexture {
    uint32_t tex_va = 0;
    GpuTextureObj* gpu = nullptr;
    uint32_t levels = 1;
    std::vector<uint32_t> level_surf_vas; // [face*levels + mip]
    bool     locked_ever = false;
    uint64_t last_check_frame = 0;
    uint64_t next_check_frame = 0;
    uint32_t unchanged_checks = 0;
    uint32_t last_checked_pal_va = 0;
    uint32_t data_bytes = 0;
    std::vector<uint8_t> last_checked_data;
};

struct GuestBuffer {
    uint32_t obj_va = 0;
    uint32_t data_va = 0;
    uint32_t size_bytes = 0;
    uint32_t dirty_seq = 1;
};

struct GpuLight {
    bool enabled = false;
    uint32_t type = 3; // 1=POINT, 2=SPOT, 3=DIRECTIONAL
    float diffuse[4]{1, 1, 1, 1};
    float specular[4]{0, 0, 0, 1};
    float ambient[4]{0, 0, 0, 1};
    float pos[3]{0, 0, 0};
    float dir[3]{0, 0, 1};
    float range = 1000.0f;
    float falloff = 1.0f;
    float a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;
    float theta = 0.5f, phi = 1.0f;
};

struct GuestVS {
    uint32_t handle = 0;      // obj_va | 1
    uint32_t obj_va = 0;
    Nv2aVertexDecl decl;
    std::vector<uint8_t> microcode;
    bool has_microcode = false;
    uint32_t fvf = 0;
};

struct GuestPS {
    uint32_t handle = 0;
    uint32_t psdef[60]{};
    const Nv2aCompiledPS* compiled = nullptr;
    bool compiled_done = false;
};

std::mutex g_d3d_mtx;

uint32_t g_dev_va = 0;
uint32_t g_pushbuf_va = 0;
uint32_t g_pushbuf_end_va = 0;

GpuTextureObj g_front_gpu{};
GpuTextureObj g_back_gpu{};
GpuTextureObj g_depth_gpu{};
uint32_t g_front_surf_va = 0;
uint32_t g_back_surf_va = 0;
uint32_t g_depth_surf_va = 0;
uint32_t g_cur_rt_va = 0;
uint32_t g_cur_ds_va = 0;

uint32_t g_xb_fb_w = 640;
uint32_t g_xb_fb_h = 480;
bool     g_xb_widescreen_mode = false;

std::unordered_map<uint32_t, GuestTexture> g_textures;
std::unordered_map<uint32_t, GuestSurface> g_surfaces;
std::unordered_map<uint32_t, GuestBuffer>  g_vbs;
std::unordered_map<uint32_t, GuestBuffer>  g_ibs;
std::unordered_map<uint32_t, GuestVS>      g_vshaders;
std::unordered_map<uint32_t, GuestPS>      g_pshaders;
std::unordered_map<uint32_t, uint32_t>     g_palettes; // pal_va -> entries data VA

// Pipeline state mirrors
uint32_t g_cur_vs_handle = 0;
uint32_t g_cur_ps_handle = 0;
uint32_t g_bound_tex_va[4]{};
uint32_t g_vb_stream_obj[4]{};
uint32_t g_vb_stream_stride[4]{};
uint32_t g_cur_ib_va = 0;
int32_t  g_base_vertex_index = 0;
uint32_t g_cur_palette_va = 0;

float g_vs_consts[192 * 4]{};
float g_ps_consts[16 * 4]{};

// Transform matrices (D3DTS_VIEW=0, D3DTS_PROJECTION=1, D3DTS_TEXTURE0..3=2..5, D3DTS_WORLD=256->slot6)
float g_matrices[8][16]{};
float g_mat_diffuse[4]  = {1.0f, 1.0f, 1.0f, 1.0f};
float g_mat_ambient[4]  = {1.0f, 1.0f, 1.0f, 1.0f};
float g_mat_specular[4] = {0.0f, 0.0f, 0.0f, 16.0f};
float g_mat_emissive[4] = {0.0f, 0.0f, 0.0f, 1.0f};
GpuLight g_lights[64]{};

float g_vp_x = 0, g_vp_y = 0, g_vp_w = 640, g_vp_h = 480, g_vp_minz = 0.0f, g_vp_maxz = 1.0f;
bool     g_ss_prog_fb_mode = false;
bool     g_cur_frame_had_prog_ss = false;
bool     g_cur_frame_saw_hd_ss = false;
uint32_t g_cur_frame_3d_draws = 0;
std::atomic<bool> g_cur_frame_start_screen{false};

void draw_frontend_screen(CpuContext& ctx) {
    // This guest routine draws the frontend screen selected at 0x0037EF9C.
    // Screen 1 is the root MainMenu. Submenus use other screen IDs.
    if (rd32(0x0037EF9Cu) == 1u) {
        g_cur_frame_start_screen.store(true, std::memory_order_relaxed);
    }
    fn_000a0d10(ctx);
}
std::atomic<uint64_t> g_frame_counter{0};
uint32_t g_vis_counter = 1;

// Helpers
uint32_t bits_per_pixel_xb(uint32_t fmt) {
    switch (fmt) {
        case 0x06: case 0x1E: case 0x07: case 0x12: case 0x13:
        case 0x2A: case 0x2E: // A8R8G8B8, LIN_A8R8G8B8, X8R8G8B8, D24S8
            return 32;
        case 0x02: case 0x03: case 0x04: case 0x05: case 0x10:
        case 0x11: case 0x15: case 0x16: case 0x1A: case 0x2C: case 0x32:
            return 16;
        case 0x00: case 0x0B: case 0x19: case 0x1B: // L8, P8, A8
            return 8;
        case 0x0C: return 4; // DXT1
        case 0x0E: case 0x0F: return 8; // DXT3, DXT5
        default: return 32;
    }
}

bool is_compressed_xb(uint32_t fmt) {
    return fmt == 0x0C || fmt == 0x0E || fmt == 0x0F;
}

uint32_t calc_pitch_xb(uint32_t w, uint32_t fmt) {
    if (fmt == 0x0C) return std::max(1u, (w + 3u) / 4u) * 8u;
    if (fmt == 0x0E || fmt == 0x0F) return std::max(1u, (w + 3u) / 4u) * 16u;
    uint32_t bpp = bits_per_pixel_xb(fmt);
    return ((w * bpp / 8u) + 63u) & ~63u;
}

uint32_t calc_surf_bytes(uint32_t w, uint32_t h, uint32_t fmt) {
    if (is_compressed_xb(fmt)) {
        uint32_t bw = std::max(1u, (w + 3u) / 4u);
        uint32_t bh = std::max(1u, (h + 3u) / 4u);
        return bw * bh * (fmt == 0x0C ? 8u : 16u);
    }
    return calc_pitch_xb(w, fmt) * std::max(1u, h);
}

void set_identity(float m[16]) {
    std::memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void write_surface_struct(uint32_t surf_va, uint32_t data_va, uint32_t fmt, uint32_t w, uint32_t h, uint32_t pitch, uint32_t type_val) {
    // D3DResource / D3DSurface layout verified at 0x258F41 & 0x240D16 & 0x239D29
    wr32(surf_va + 0x00u, (type_val << 16) | 1u);
    wr32(surf_va + 0x04u, data_va);
    wr32(surf_va + 0x08u, 0u);
    uint32_t fmt_dword = ((fmt & 0xFFu) << 8) | 0x00020021u;
    wr32(surf_va + 0x0Cu, fmt_dword);
    wr32(surf_va + 0x10u, pitch);
    wr32(surf_va + 0x14u, ((w - 1u) & 0xFFFu) | (((h - 1u) & 0xFFFu) << 12));
}

void init_render_state_defaults() {
    std::memset(mem_ptr(kGuestTSSBaseVA), 0, 4 * 32 * 4);
    std::memset(mem_ptr(kGuestRSBaseVA), 0, 170 * 4);

    wr32(kGuestRSBaseVA + XBRS_ZENABLE * 4u, 1u);
    wr32(kGuestRSBaseVA + XBRS_ZWRITEENABLE * 4u, 1u);
    wr32(kGuestRSBaseVA + XBRS_ZFUNC * 4u, 4u); // D3DCMP_LESSEQUAL
    wr32(kGuestRSBaseVA + XBRS_ALPHABLENDENABLE * 4u, 0u);
    wr32(kGuestRSBaseVA + XBRS_ALPHATESTENABLE * 4u, 0u);
    wr32(kGuestRSBaseVA + XBRS_ALPHAFUNC * 4u, 8u); // ALWAYS
    wr32(kGuestRSBaseVA + XBRS_ALPHAREF * 4u, 0u);
    wr32(kGuestRSBaseVA + XBRS_SRCBLEND * 4u, 2u); // ONE
    wr32(kGuestRSBaseVA + XBRS_DESTBLEND * 4u, 1u); // ZERO
    wr32(kGuestRSBaseVA + XBRS_BLENDOP * 4u, 0x8006u); // GL FUNC_ADD or 1
    wr32(kGuestRSBaseVA + XBRS_FILLMODE * 4u, 0x1B02u); // GL_FILL
    wr32(kGuestRSBaseVA + XBRS_BACKFILLMODE * 4u, 0x1B02u);
    wr32(kGuestRSBaseVA + XBRS_FRONTFACE * 4u, 0x0900u); // CW
    wr32(kGuestRSBaseVA + XBRS_CULLMODE * 4u, 0x0901u);  // CCW
    wr32(kGuestRSBaseVA + XBRS_COLORWRITEENABLE * 4u, 0x01010101u);
    wr32(kGuestRSBaseVA + XBRS_STENCILFAIL * 4u, 0x1E00u);      // GL_KEEP
    wr32(kGuestRSBaseVA + XBRS_STENCILZFAIL * 4u, 0x1E00u);     // GL_KEEP
    wr32(kGuestRSBaseVA + XBRS_STENCILPASS * 4u, 0x1E00u);      // GL_KEEP
    wr32(kGuestRSBaseVA + XBRS_STENCILFUNC * 4u, 0x0207u);      // GL_ALWAYS
    wr32(kGuestRSBaseVA + XBRS_STENCILREF * 4u, 0u);
    wr32(kGuestRSBaseVA + XBRS_STENCILMASK * 4u, 0xFFFFFFFFu);
    wr32(kGuestRSBaseVA + XBRS_STENCILWRITEMASK * 4u, 0xFFFFFFFFu);
    wr32(kGuestRSBaseVA + XBRS_LIGHTING * 4u, 1u);

    for (uint32_t s = 0; s < 4; ++s) {
        uint32_t base = kGuestTSSBaseVA + s * 0x80u;
        wr32(base + XBTSS_ADDRESSU * 4u, 1u); // WRAP
        wr32(base + XBTSS_ADDRESSV * 4u, 1u);
        wr32(base + XBTSS_MAGFILTER * 4u, 2u); // LINEAR
        wr32(base + XBTSS_MINFILTER * 4u, 2u);
        wr32(base + XBTSS_COLOROP * 4u, s == 0 ? 4u : 1u); // MODULATE stage 0, DISABLE stages 1..3
        wr32(base + XBTSS_COLORARG1 * 4u, 2u); // TEXTURE
        wr32(base + XBTSS_COLORARG2 * 4u, 0u); // DIFFUSE
        wr32(base + XBTSS_ALPHAOP * 4u, s == 0 ? 4u : 1u);
        wr32(base + XBTSS_ALPHAARG1 * 4u, 2u);
        wr32(base + XBTSS_ALPHAARG2 * 4u, 0u);
    }

    for (int i = 0; i < 8; ++i) set_identity(g_matrices[i]);
    std::memset(g_vs_consts, 0, sizeof(g_vs_consts));
    std::memset(g_ps_consts, 0, sizeof(g_ps_consts));
    std::memset(mem_ptr(kGuestVSConstVA), 0, sizeof(g_vs_consts));
}

void ensure_guest_device_allocated() {
    if (g_dev_va) return;
    g_dev_va = xbox_guest_alloc(0x4800u, 64u, true);
    g_pushbuf_va = xbox_guest_alloc(0x10000u, 64u, true);
    g_pushbuf_end_va = g_pushbuf_va + 0x10000u;

    wr32(kGuestDevicePtrVA, g_dev_va);
    wr32(g_dev_va + 0x00u, g_pushbuf_va);
    wr32(g_dev_va + 0x04u, g_pushbuf_end_va);
    wr32(g_dev_va + 0x374u, 1u);
    wr32(g_dev_va + 0x37Cu, 1u);

    uint32_t fb_bytes = calc_surf_bytes(1280, 720, 0x1Eu);
    g_front_surf_va = xbox_guest_alloc(0x20u, 16u, true);
    g_back_surf_va  = xbox_guest_alloc(0x20u, 16u, true);
    g_depth_surf_va = xbox_guest_alloc(0x20u, 16u, true);

    uint32_t front_buf = xbox_guest_alloc(fb_bytes, 64u, true);
    uint32_t back_buf  = xbox_guest_alloc(fb_bytes, 64u, true);
    uint32_t depth_buf = xbox_guest_alloc(fb_bytes, 64u, true);

    write_surface_struct(g_front_surf_va, front_buf, 0x1Eu, g_xb_fb_w, g_xb_fb_h, calc_pitch_xb(g_xb_fb_w, 0x1Eu), 1u);
    write_surface_struct(g_back_surf_va,  back_buf,  0x1Eu, g_xb_fb_w, g_xb_fb_h, calc_pitch_xb(g_xb_fb_w, 0x1Eu), 1u);
    write_surface_struct(g_depth_surf_va, depth_buf, 0x2Au, g_xb_fb_w, g_xb_fb_h, calc_pitch_xb(g_xb_fb_w, 0x2Au), 1u);

    g_front_gpu = {g_front_surf_va, g_xb_fb_w, g_xb_fb_h, 1, 0x1Eu, calc_pitch_xb(g_xb_fb_w, 0x1Eu), front_buf};
    g_back_gpu  = {g_back_surf_va,  g_xb_fb_w, g_xb_fb_h, 1, 0x1Eu, calc_pitch_xb(g_xb_fb_w, 0x1Eu), back_buf};
    g_depth_gpu = {g_depth_surf_va, g_xb_fb_w, g_xb_fb_h, 1, 0x2Au, calc_pitch_xb(g_xb_fb_w, 0x2Au), depth_buf};

    g_surfaces[g_front_surf_va] = {g_front_surf_va, 0, &g_front_gpu, g_xb_fb_w, g_xb_fb_h, 0x1Eu, g_front_gpu.pitch, front_buf, 0, false, true, 0};
    g_surfaces[g_back_surf_va]  = {g_back_surf_va,  0, &g_back_gpu,  g_xb_fb_w, g_xb_fb_h, 0x1Eu, g_back_gpu.pitch,  back_buf,  0, false, true, 0};
    g_surfaces[g_depth_surf_va] = {g_depth_surf_va, 0, &g_depth_gpu, g_xb_fb_w, g_xb_fb_h, 0x2Au, g_depth_gpu.pitch, depth_buf, 0, false, false, 0};

    g_cur_rt_va = g_back_surf_va;
    g_cur_ds_va = g_depth_surf_va;

    wr32(g_dev_va + 0x21B4u, g_cur_rt_va);
    wr32(g_dev_va + 0x21B8u, g_cur_ds_va);
    wr32(g_dev_va + 0x21C0u, g_front_surf_va);
    wr32(g_dev_va + 0x21C4u, g_back_surf_va);
    init_render_state_defaults();
}

// Convert NV097 Kelvin GL blend/func/cull enums OR PC D3D8 enums cleanly to D3D8 values expected by d3d11_renderer.cpp
uint32_t normalize_blend_factor(uint32_t v) {
    // Xbox D3DBLEND uses OpenGL constants in RS / NV097:
    // GL_ZERO=0, GL_ONE=1, SRC_COLOR=0x300, ONE_MINUS_SRC_COLOR=0x301,
    // SRC_ALPHA=0x302, ONE_MINUS_SRC_ALPHA=0x303, DST_ALPHA=0x304, ONE_MINUS_DST_ALPHA=0x305,
    // DST_COLOR=0x306, ONE_MINUS_DST_COLOR=0x307, SRC_ALPHA_SATURATE=0x308
    if (v <= 13u) {
        // Also support 0->1(ZERO), 1->2(ONE) or D3DBLEND(1..11)
        if (v == 0u) return 1u; // ZERO
        if (v == 1u) return 2u; // ONE
        return v;
    }
    switch (v) {
        case 0x0300u: return 3u;  // SRCCOLOR
        case 0x0301u: return 4u;  // INVSRCCOLOR
        case 0x0302u: return 5u;  // SRCALPHA
        case 0x0303u: return 6u;  // INVSRCALPHA
        case 0x0304u: return 7u;  // DESTALPHA
        case 0x0305u: return 8u;  // INVDESTALPHA
        case 0x0306u: return 9u;  // DESTCOLOR
        case 0x0307u: return 10u; // INVDESTCOLOR
        case 0x0308u: return 11u; // SRCALPHASAT
        default: return 2u;
    }
}

uint32_t normalize_cmp_func(uint32_t v) {
    // Xbox D3DCMPFUNC uses OpenGL 0x200..0x207 OR 1..8
    if (v >= 0x0200u && v <= 0x0207u) return (v - 0x0200u) + 1u;
    if (v >= 1u && v <= 8u) return v;
    return 4u; // LESSEQUAL
}

uint32_t normalize_blend_op(uint32_t v) {
    switch (v) {
        case 1u: case 0x8006u: return 1u; // ADD
        case 2u: case 0x800Au: return 2u; // SUBTRACT
        case 3u: case 0x800Bu: return 3u; // REVSUBTRACT
        case 4u: case 0x8007u: return 4u; // MIN
        case 5u: case 0x8008u: return 5u; // MAX
        default: return 1u;
    }
}

uint32_t normalize_cull(uint32_t v) {
    // Xbox D3DCULL: NONE=0, CW=0x900?, CCW=0x901?, or D3DCULL_NONE=1, CW=2, CCW=3
    if (v == 0u || v == 1u) return 1u; // NONE
    if (v == 2u || v == 0x0404u || v == 0x0900u) return 2u; // CW/FRONT
    if (v == 3u || v == 0x0405u || v == 0x0901u) return 3u; // CCW/BACK
    return 3u;
}

uint32_t normalize_tex_addr(uint32_t v) {
    // D3DTADDRESS: WRAP=1, MIRROR=2, CLAMP=3, BORDER=4, MIRRORONCE=5
    if (v >= 1u && v <= 5u) return v;
    switch (v) {
        case 0x2901u: return 1u; // REPEAT
        case 0x812Fu: case 0x812Du: return 3u; // CLAMP_TO_EDGE / BORDER
        default: return 1u;
    }
}

void unpack_argb_to_f4(uint32_t c, float out[4]) {
    out[0] = ((c >> 16) & 0xFFu) / 255.0f;
    out[1] = ((c >>  8) & 0xFFu) / 255.0f;
    out[2] = ((c >>  0) & 0xFFu) / 255.0f;
    out[3] = ((c >> 24) & 0xFFu) / 255.0f;
}

// ---------- RenderState & NV097 Sync ----------
void hle_D3DDevice_SetRenderState_idx(uint32_t rs, uint32_t val) {
    ensure_guest_device_allocated();
    if (rs < 170u) {
        wr32(kGuestRSBaseVA + rs * 4u, val);
    }
}

void apply_nv097_method(uint32_t method, uint32_t val) {
    uint32_t m = method & 0xFFFFu;
    switch (m) {
        case 0x0300u: wr32(kGuestRSBaseVA + XBRS_ALPHATESTENABLE * 4u, val); break;
        case 0x0304u: wr32(kGuestRSBaseVA + XBRS_ALPHABLENDENABLE * 4u, val); break;
        case 0x0308u:
            if (!val) wr32(kGuestRSBaseVA + XBRS_CULLMODE * 4u, 0u);
            else if (rd32(kGuestRSBaseVA + XBRS_CULLMODE * 4u) <= 1u) wr32(kGuestRSBaseVA + XBRS_CULLMODE * 4u, 0x0901u);
            break;
        case 0x030Cu: wr32(kGuestRSBaseVA + XBRS_ZENABLE * 4u, val); break;
        case 0x0328u: wr32(kGuestRSBaseVA + XBRS_STENCILENABLE * 4u, val); break;
        case 0x032Cu: wr32(kGuestRSBaseVA + XBRS_FOGENABLE * 4u, val); break;
        case 0x0330u: wr32(kGuestRSBaseVA + XBRS_POINTOFFSETENABLE * 4u, val); break;
        case 0x0334u: wr32(kGuestRSBaseVA + XBRS_WIREFRAMEOFFSETENABLE * 4u, val); break;
        case 0x0338u: wr32(kGuestRSBaseVA + XBRS_SOLIDOFFSETENABLE * 4u, val); break;
        case 0x033Cu: wr32(kGuestRSBaseVA + XBRS_ALPHAFUNC * 4u, val); break;
        case 0x0340u: wr32(kGuestRSBaseVA + XBRS_ALPHAREF * 4u, val); break;
        case 0x0344u: wr32(kGuestRSBaseVA + XBRS_SRCBLEND * 4u, val); break;
        case 0x0348u: wr32(kGuestRSBaseVA + XBRS_DESTBLEND * 4u, val); break;
        case 0x0350u: wr32(kGuestRSBaseVA + XBRS_BLENDOP * 4u, val); break;
        case 0x0354u: wr32(kGuestRSBaseVA + XBRS_ZFUNC * 4u, val); break;
        case 0x0358u: wr32(kGuestRSBaseVA + XBRS_COLORWRITEENABLE * 4u, val); break;
        case 0x035Cu: wr32(kGuestRSBaseVA + XBRS_ZWRITEENABLE * 4u, val); break;
        case 0x0360u: wr32(kGuestRSBaseVA + XBRS_STENCILWRITEMASK * 4u, val); break;
        case 0x0364u: wr32(kGuestRSBaseVA + XBRS_STENCILFUNC * 4u, val); break;
        case 0x0368u: wr32(kGuestRSBaseVA + XBRS_STENCILREF * 4u, val); break;
        case 0x036Cu: wr32(kGuestRSBaseVA + XBRS_STENCILMASK * 4u, val); break;
        case 0x0370u: wr32(kGuestRSBaseVA + XBRS_STENCILFAIL * 4u, val); break;
        case 0x0374u: wr32(kGuestRSBaseVA + XBRS_STENCILZFAIL * 4u, val); break;
        case 0x0378u: wr32(kGuestRSBaseVA + XBRS_STENCILPASS * 4u, val); break;
        case 0x0384u: wr32(kGuestRSBaseVA + XBRS_POLYGONOFFSETZSLOPESCALE * 4u, val); break;
        case 0x0388u: wr32(kGuestRSBaseVA + XBRS_POLYGONOFFSETZOFFSET * 4u, val); break;
        case 0x038Cu: wr32(kGuestRSBaseVA + XBRS_FILLMODE * 4u, val); break;
        case 0x0390u: wr32(kGuestRSBaseVA + XBRS_BACKFILLMODE * 4u, val); break;
        case 0x039Cu: {
            uint32_t ff = rd32(kGuestRSBaseVA + XBRS_FRONTFACE * 4u);
            if (ff != 0x0900u && ff != 0x0901u) ff = 0x0900u;
            uint32_t cm = (val == 0x0404u) ? ff : (ff == 0x0900u ? 0x0901u : 0x0900u);
            wr32(kGuestRSBaseVA + XBRS_CULLMODE * 4u, cm);
            break;
        }
        case 0x03A0u: wr32(kGuestRSBaseVA + XBRS_FRONTFACE * 4u, val); break;
        case 0x03A4u: wr32(kGuestRSBaseVA + XBRS_NORMALIZENORMALS * 4u, val); break;
        case 0x02A8u: wr32(kGuestRSBaseVA + XBRS_TEXTUREFACTOR * 4u, val); break;
        case 0x1D78u: wr32(kGuestRSBaseVA + XBRS_SPECULARENABLE * 4u, val); break;
        case 0x1E6Cu: wr32(kGuestRSBaseVA + XBRS_FOGCOLOR * 4u, val); break;
        default: break;
    }
}

void sync_and_draw(uint32_t prim_type, uint32_t vertex_count, uint32_t index_count,
                   const void* inline_verts, uint32_t inline_stride, const uint16_t* indices, int32_t base_vtx) {
    if (hle_kernel_is_shutting_down()) return;
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    ensure_guest_device_allocated();

    uint64_t frame_id = g_frame_counter.load(std::memory_order_relaxed);
    DrawStateSnapshot st{};
    st.prim_type = prim_type;
    st.vertex_count = vertex_count;
    st.index_count = index_count;
    st.base_vertex = base_vtx;
    st.ib_ptr = indices;
    st.xb_fb_w = static_cast<float>(g_xb_fb_w);
    st.xb_fb_h = static_cast<float>(g_xb_fb_h);
    st.xb_widescreen = g_xb_widescreen_mode;
    st.vp_x = g_vp_x; st.vp_y = g_vp_y; st.vp_w = g_vp_w; st.vp_h = g_vp_h;
    st.vp_minz = g_vp_minz; st.vp_maxz = g_vp_maxz;

    // Sync vertex shader & decl (ensure GL context is current on this thread before compiling shaders)
    gfx_ensure_gl_current();
    uint32_t guest_vs = rd32(g_dev_va + 0x380u);
    if (guest_vs) g_cur_vs_handle = guest_vs;

    Nv2aVertexDecl decl{};
    const GuestVS* gvs = nullptr;
    if (g_cur_vs_handle & 1u) {
        auto it = g_vshaders.find(g_cur_vs_handle);
        if (it != g_vshaders.end()) gvs = &it->second;
        else {
            auto it2 = g_vshaders.find(g_cur_vs_handle - 1u);
            if (it2 != g_vshaders.end()) gvs = &it2->second;
        }
    }
    if (gvs) {
        decl = gvs->decl;
        if (gvs->has_microcode && !gvs->microcode.empty()) {
            st.vs = nv2a_get_xvu_vs(decl, gvs->microcode.data(), gvs->microcode.size());
            st.is_programmable_vs = (st.vs != nullptr);
        }
        if (!st.vs) {
            st.vs = nv2a_get_ff_vs(decl);
            st.is_programmable_vs = false;
        }
    } else {
        uint32_t fvf = g_cur_vs_handle ? g_cur_vs_handle : 0x104u;
        decl = nv2a_decl_from_fvf(fvf);
        st.vs = nv2a_get_ff_vs(decl);
        st.is_programmable_vs = false;
    }

    // Streams (static GPU VB caching for bound guest VBs)
    if (inline_verts) {
        st.vb_ptrs[0] = inline_verts;
        st.vb_strides[0] = inline_stride ? inline_stride : std::max(16u, decl.stream_strides[0]);
    } else {
        for (uint32_t s = 0; s < 4; ++s) {
            if (g_vb_stream_obj[s]) {
                auto it = g_vbs.find(g_vb_stream_obj[s]);
                if (it != g_vbs.end() && it->second.data_va) {
                    st.vb_ptrs[s] = mem_ptr(it->second.data_va);
                    st.vb_guest_va[s] = it->second.obj_va;
                    st.vb_total_bytes[s] = it->second.size_bytes;
                    st.vb_dirty_seq[s] = it->second.dirty_seq;
                } else {
                    uint32_t d = rd32(g_vb_stream_obj[s] + 4u);
                    if (d && d < XBOX_MEM_SIZE) st.vb_ptrs[s] = mem_ptr(d);
                }
                st.vb_strides[s] = g_vb_stream_stride[s] ? g_vb_stream_stride[s] : decl.stream_strides[s];
            }
        }
    }

    // Render states directly from guest RAM 0x00252BC0
    st.alpha_test = rd32(kGuestRSBaseVA + XBRS_ALPHATESTENABLE * 4u) != 0;
    st.alpha_func = normalize_cmp_func(rd32(kGuestRSBaseVA + XBRS_ALPHAFUNC * 4u));
    uint32_t aref_raw = rd32(kGuestRSBaseVA + XBRS_ALPHAREF * 4u);
    st.alpha_ref = (aref_raw & 0xFFu) / 255.0f;
    st.alpha_blend = rd32(kGuestRSBaseVA + XBRS_ALPHABLENDENABLE * 4u) != 0;
    st.src_blend = normalize_blend_factor(rd32(kGuestRSBaseVA + XBRS_SRCBLEND * 4u));
    st.dst_blend = normalize_blend_factor(rd32(kGuestRSBaseVA + XBRS_DESTBLEND * 4u));
    st.blend_op = normalize_blend_op(rd32(kGuestRSBaseVA + XBRS_BLENDOP * 4u));
    st.z_enable = rd32(kGuestRSBaseVA + XBRS_ZENABLE * 4u) != 0;
    st.z_write = rd32(kGuestRSBaseVA + XBRS_ZWRITEENABLE * 4u) != 0;
    st.z_func = normalize_cmp_func(rd32(kGuestRSBaseVA + XBRS_ZFUNC * 4u));
    st.cull_mode = normalize_cull(rd32(kGuestRSBaseVA + XBRS_CULLMODE * 4u));
    uint32_t fm = rd32(kGuestRSBaseVA + XBRS_FILLMODE * 4u);
    st.fill_mode = (fm == 0x1B01u || fm == 2u) ? 2u : 3u;
    st.poly_offset_fill = rd32(kGuestRSBaseVA + XBRS_SOLIDOFFSETENABLE * 4u) != 0;
    st.poly_offset_scale = rdf32(kGuestRSBaseVA + XBRS_POLYGONOFFSETZSLOPESCALE * 4u);
    st.poly_offset_bias = rdf32(kGuestRSBaseVA + XBRS_POLYGONOFFSETZOFFSET * 4u);
    uint32_t cwe = rd32(kGuestRSBaseVA + XBRS_COLORWRITEENABLE * 4u);
    st.color_write_mask = (cwe > 0xFu)
        ? (((cwe & 0xFF000000u) ? 8u : 0u) | ((cwe & 0x00FF0000u) ? 1u : 0u) | ((cwe & 0x0000FF00u) ? 2u : 0u) | ((cwe & 0x000000FFu) ? 4u : 0u))
        : (cwe & 0xFu);
    if (cwe == 0) st.color_write_mask = 0;

    // Stencil
    st.stencil_enable = rd32(kGuestRSBaseVA + XBRS_STENCILENABLE * 4u) != 0;
    st.stencil_func = normalize_cmp_func(rd32(kGuestRSBaseVA + XBRS_STENCILFUNC * 4u));
    st.stencil_ref = rd32(kGuestRSBaseVA + XBRS_STENCILREF * 4u);
    st.stencil_mask = rd32(kGuestRSBaseVA + XBRS_STENCILMASK * 4u);
    st.stencil_writemask = rd32(kGuestRSBaseVA + XBRS_STENCILWRITEMASK * 4u);
    uint32_t sf = rd32(kGuestRSBaseVA + XBRS_STENCILFAIL * 4u);
    st.stencil_fail = sf ? sf : 1u;
    uint32_t szf = rd32(kGuestRSBaseVA + XBRS_STENCILZFAIL * 4u);
    st.stencil_zfail = szf ? szf : 1u;
    uint32_t sp = rd32(kGuestRSBaseVA + XBRS_STENCILPASS * 4u);
    st.stencil_pass = sp ? sp : 1u;

    // Lighting, Specular & Material
    st.lighting_en = rd32(kGuestRSBaseVA + XBRS_LIGHTING * 4u) != 0;
    st.specular_en = (rd32(kGuestRSBaseVA + XBRS_SPECULARENABLE * 4u) != 0) ||
                     (rd32(kGuestRSBaseVA + XBRS_SPECULARENABLE_RS * 4u) != 0);
    unpack_argb_to_f4(rd32(kGuestRSBaseVA + XBRS_TEXTUREFACTOR * 4u), st.tex_factor);
    std::memcpy(st.mat_diffuse, g_mat_diffuse, 16);
    std::memcpy(st.mat_ambient, g_mat_ambient, 16);
    std::memcpy(st.mat_specular, g_mat_specular, 16);
    std::memcpy(st.mat_emissive, g_mat_emissive, 16);

    int active_slot = 0;
    for (int i = 0; i < 64 && active_slot < 4; ++i) {
        if (!g_lights[i].enabled) continue;
        const auto& L = g_lights[i];
        st.light_pos[active_slot][0] = L.pos[0];
        st.light_pos[active_slot][1] = L.pos[1];
        st.light_pos[active_slot][2] = L.pos[2];
        st.light_pos[active_slot][3] = static_cast<float>(L.type);
        st.light_dir[active_slot][0] = L.dir[0];
        st.light_dir[active_slot][1] = L.dir[1];
        st.light_dir[active_slot][2] = L.dir[2];
        st.light_dir[active_slot][3] = 1.0f;
        std::memcpy(st.light_diff[active_slot], L.diffuse, 16);
        std::memcpy(st.light_spec[active_slot], L.specular, 16);
        st.light_atten[active_slot][0] = L.range;
        st.light_atten[active_slot][1] = L.a0;
        st.light_atten[active_slot][2] = L.a1;
        st.light_atten[active_slot][3] = L.a2;
        ++active_slot;
    }

    // Fog
    st.fog_enable = rd32(kGuestRSBaseVA + XBRS_FOGENABLE * 4u) != 0;
    uint32_t fmode = rd32(kGuestRSBaseVA + XBRS_FOGTABLEMODE * 4u);
    st.fog_mode = (fmode >= 1u && fmode <= 3u) ? fmode : 3u;
    st.fog_start = rdf32(kGuestRSBaseVA + XBRS_FOGSTART * 4u);
    st.fog_end = rdf32(kGuestRSBaseVA + XBRS_FOGEND * 4u);
    st.fog_density = rdf32(kGuestRSBaseVA + XBRS_FOGDENSITY * 4u);
    if (st.fog_end <= st.fog_start) { st.fog_start = 0.0f; st.fog_end = 100.0f; }
    unpack_argb_to_f4(rd32(kGuestRSBaseVA + XBRS_FOGCOLOR * 4u), st.fog_color);

    st.vs_consts = g_vs_consts;
    st.ps_consts = g_ps_consts;
    st.world_m = g_matrices[6];
    st.view_m = g_matrices[0];
    st.proj_m = g_matrices[1];
    st.current_palette_va = g_cur_palette_va;

    uint8_t tex_mask = 0;
    uint8_t cube_mask = 0;
    for (uint32_t s = 0; s < 4; ++s) {
        uint32_t tbase = kGuestTSSBaseVA + s * 0x80u;
        st.addr_u[s] = normalize_tex_addr(rd32(tbase + XBTSS_ADDRESSU * 4u));
        st.addr_v[s] = normalize_tex_addr(rd32(tbase + XBTSS_ADDRESSV * 4u));
        st.min_filter[s] = rd32(tbase + XBTSS_MINFILTER * 4u);
        st.mag_filter[s] = rd32(tbase + XBTSS_MAGFILTER * 4u);

        uint32_t tva = g_bound_tex_va[s];
        if (!tva) tva = rd32(g_dev_va + 0xB38u + s * 4u);
        if (tva) {
            auto it = g_textures.find(tva);
            if (it != g_textures.end() && it->second.gpu) {
                auto& texture = it->second;
                auto* gpu = texture.gpu;
                if (texture.locked_ever && !gpu->is_render_target && texture.last_check_frame != frame_id) {
                    if (gpu->xb_format == 0x0Bu && texture.last_checked_pal_va != st.current_palette_va) {
                        texture.last_checked_pal_va = st.current_palette_va;
                        texture.unchanged_checks = 0;
                        texture.next_check_frame = 0;
                        gpu->dirty = true;
                    }
                    if (frame_id >= texture.next_check_frame) {
                        texture.last_check_frame = frame_id;
                        // A lock can leave the game holding a writable pointer across frames.
                        // Snapshot initial bytes, then back off checks for static textures while
                        // explicit locks and BinkCopyToBuffer reset next_check_frame immediately.
                        const auto* data = mem_ptr(gpu->data_va);
                        if (!gpu->data_va || !data || !texture.data_bytes) {
                            gpu->dirty = true;
                            texture.unchanged_checks = 0;
                            texture.next_check_frame = frame_id + 15ull;
                        } else if (texture.last_checked_data.size() != texture.data_bytes ||
                                   std::memcmp(texture.last_checked_data.data(), data, texture.data_bytes) != 0) {
                            texture.last_checked_data.assign(data, data + texture.data_bytes);
                            texture.unchanged_checks = 0;
                            texture.next_check_frame = frame_id + 1ull;
                            gpu->dirty = true;
                        } else {
                            ++texture.unchanged_checks;
                            if (texture.unchanged_checks < 4u) {
                                texture.next_check_frame = frame_id + 1ull;
                            } else if (texture.unchanged_checks < 16u) {
                                uint64_t stagger = (texture.unchanged_checks == 4u) ? ((texture.tex_va >> 4) % 15u) : 0ull;
                                texture.next_check_frame = frame_id + 15ull + stagger;
                            } else {
                                texture.next_check_frame = frame_id + 60ull;
                            }
                        }
                    }
                }
                st.tex[s] = it->second.gpu;
                tex_mask |= static_cast<uint8_t>(1u << s);
                if (it->second.gpu->is_cube) cube_mask |= static_cast<uint8_t>(1u << s);
            }
        }
    }

    if (g_cur_rt_va && g_cur_rt_va != g_back_surf_va && g_cur_rt_va != g_front_surf_va) {
        auto sit = g_surfaces.find(g_cur_rt_va);
        if (sit != g_surfaces.end() && sit->second.gpu) {
            sit->second.gpu->is_render_target = true;
            st.custom_rt = sit->second.gpu;
        }
    }

    uint32_t guest_ps = rd32(g_dev_va + 0x370u);
    if (guest_ps != 0xFFFFFFFFu && guest_ps != 0) g_cur_ps_handle = guest_ps;
    if (g_cur_ps_handle) {
        auto pit = g_pshaders.find(g_cur_ps_handle);
        if (pit != g_pshaders.end()) {
            if (!pit->second.compiled_done) {
                pit->second.compiled = nv2a_get_xpu_ps(pit->second.psdef);
                pit->second.compiled_done = true;
            }
            st.ps = pit->second.compiled;
            st.is_programmable_ps = (st.ps != nullptr);
        }
    }
    if (!st.ps) {
        Nv2aFFPSKey ff{};
        ff.tex_bound_mask = tex_mask;
        ff.tex_is_cube_mask = cube_mask;
        ff.alpha_test_enable = st.alpha_test ? 1 : 0;
        ff.alpha_func = static_cast<uint8_t>(st.alpha_func);
        ff.fog_enable = st.fog_enable ? 1 : 0;
        ff.lighting_enable = st.lighting_en ? 1 : 0;
        ff.has_diffuse = 1;
        ff.has_specular = st.specular_en ? 1 : 0;
        for (uint32_t s = 0; s < 4; ++s) {
            uint32_t tbase = kGuestTSSBaseVA + s * 0x80u;
            ff.stage_color_op[s]   = rd32(tbase + XBTSS_COLOROP * 4u);
            ff.stage_color_arg1[s] = rd32(tbase + XBTSS_COLORARG1 * 4u);
            ff.stage_color_arg2[s] = rd32(tbase + XBTSS_COLORARG2 * 4u);
            ff.stage_alpha_op[s]   = rd32(tbase + XBTSS_ALPHAOP * 4u);
            ff.stage_alpha_arg1[s] = rd32(tbase + XBTSS_ALPHAARG1 * 4u);
            ff.stage_alpha_arg2[s] = rd32(tbase + XBTSS_ALPHAARG2 * 4u);
        }
        st.ps = nv2a_get_ff_ps(ff);
        st.is_programmable_ps = false;
    }

    if (st.vs && st.vs->is_screenspace && st.is_programmable_vs && !st.custom_rt) {
        g_cur_frame_had_prog_ss = true;
        if (st.vb_ptrs[0] && st.vb_strides[0] >= 8) {
            uint32_t vc = st.vertex_count ? std::min(st.vertex_count, 32u) : 4u;
            const uint8_t* vb = static_cast<const uint8_t*>(st.vb_ptrs[0]);
            for (uint32_t i = 0; i < vc; ++i) {
                const float* xy = reinterpret_cast<const float*>(vb + i * st.vb_strides[0]);
                if (xy[0] > 642.0f || xy[1] > 482.0f) {
                    g_cur_frame_saw_hd_ss = true;
                    g_ss_prog_fb_mode = true;
                    break;
                }
            }
        }
    }
    if (!(st.vs && st.vs->is_screenspace) && !st.custom_rt) {
        ++g_cur_frame_3d_draws;
    }
    st.ss_prog_use_fb = g_ss_prog_fb_mode;

    gfx_draw(st);
}

// ---------- Hook Implementations (all 91 D3D + 3 XGRPH) ----------
void hle_D3D_SetLighting(CpuContext& ctx) {
    uint32_t val = hle_arg(ctx, 0);
    ensure_guest_device_allocated();
    wr32(g_dev_va + 0x37Cu, val);
    hle_D3DDevice_SetRenderState_idx(XBRS_LIGHTING, val);
    hle_ret(ctx, 4);
}

void hle_D3D_FastMethod(CpuContext& ctx) {
    // 0x00235130: __fastcall (ecx = NV097 method from D3D__RenderStateMap[rs], edx = val)
    ensure_guest_device_allocated();
    apply_nv097_method(ctx.ecx, ctx.edx);
    hle_ret(ctx, 0);
}

#define DEF_RS_HOOK(NAME, RS_IDX, NV_M) \
void NAME(CpuContext& ctx) { \
    uint32_t val = hle_arg(ctx, 0); \
    hle_D3DDevice_SetRenderState_idx(RS_IDX, val); \
    if (NV_M) apply_nv097_method(NV_M, val); \
    hle_ret(ctx, 4); \
}

DEF_RS_HOOK(hle_RS_89_StencilEnable, 0x89u, 0x0328u)
DEF_RS_HOOK(hle_RS_8A_TexFactor,     0x8Au, 0x02A8u)
DEF_RS_HOOK(hle_RS_8B_PolyBias,      0x8Bu, 0x038Cu)
DEF_RS_HOOK(hle_RS_8C_ZBias,         0x8Cu, 0x0390u)
DEF_RS_HOOK(hle_RS_8D_PolyOffset,    0x8Du, 0u)
DEF_RS_HOOK(hle_RS_8E_CullMode,      0x8Eu, 0x03A4u)
DEF_RS_HOOK(hle_RS_8F_ZEnable,       0x8Fu, 0x030Cu)
DEF_RS_HOOK(hle_RS_90_FogEnable,     0x90u, 0x032Cu)
DEF_RS_HOOK(hle_RS_91_FogTableMode,  0x91u, 0u)
DEF_RS_HOOK(hle_RS_92_FrontFace,     0x92u, 0x03A0u)
DEF_RS_HOOK(hle_RS_93_FillCullEn,    0x93u, 0u)
DEF_RS_HOOK(hle_RS_94_PSFlags,       0x94u, 0u)
DEF_RS_HOOK(hle_RS_95_Wrap,          0x95u, 0u)
DEF_RS_HOOK(hle_RS_96_TexGen,        0x96u, 0u)
DEF_RS_HOOK(hle_RS_97_Specular,      0x97u, 0u)
DEF_RS_HOOK(hle_RS_98_LineWidth,     0x98u, 0u)
DEF_RS_HOOK(hle_RS_99_Antialias,     0x99u, 0u)
DEF_RS_HOOK(hle_RS_9A_EdgeAA,        0x9Au, 0u)
DEF_RS_HOOK(hle_RS_9B_SampleAlpha,   0x9Bu, 0u)
DEF_RS_HOOK(hle_RS_9C_FogColor,      0x9Cu, 0x1E6Cu)
DEF_RS_HOOK(hle_RS_9D_FogDensity,    0x9Du, 0u)
DEF_RS_HOOK(hle_RS_9E_Multisample,   0x9Eu, 0u)
DEF_RS_HOOK(hle_RS_9F_YUV,           0x9Fu, 0u)
DEF_RS_HOOK(hle_RS_A0_FogStart,      0xA0u, 0u)
DEF_RS_HOOK(hle_RS_A1_FogEnd,        0xA1u, 0u)
DEF_RS_HOOK(hle_RS_A2_RangeFog,      0xA2u, 0u)
DEF_RS_HOOK(hle_RS_A3_Shadow,        0xA3u, 0u)
DEF_RS_HOOK(hle_RS_A4_ShadowA,       0xA4u, 0u)
DEF_RS_HOOK(hle_RS_A5_ShadowB,       0xA5u, 0u)

void hle_TSS_Stage28(CpuContext& ctx) {
    uint32_t s = hle_arg(ctx, 0), v = hle_arg(ctx, 1);
    if (s < 4u) wr32(kGuestTSSBaseVA + s * 0x80u + 28u * 4u, v);
    hle_ret(ctx, 8);
}
void hle_TSS_Generic(CpuContext& ctx) {
    uint32_t s = hle_arg(ctx, 0), t = hle_arg(ctx, 1), v = hle_arg(ctx, 2);
    if (s < 4u && t < 32u) wr32(kGuestTSSBaseVA + s * 0x80u + t * 4u, v);
    hle_ret(ctx, 12);
}
void hle_TSS_Stage29(CpuContext& ctx) {
    uint32_t s = hle_arg(ctx, 0), v = hle_arg(ctx, 1);
    if (s < 4u) wr32(kGuestTSSBaseVA + s * 0x80u + 29u * 4u, v);
    hle_ret(ctx, 8);
}
void hle_TSS_Stage30(CpuContext& ctx) {
    uint32_t s = hle_arg(ctx, 0), v = hle_arg(ctx, 1);
    if (s < 4u) wr32(kGuestTSSBaseVA + s * 0x80u + 30u * 4u, v);
    hle_ret(ctx, 8);
}

void hle_D3D_StubRet8(CpuContext& ctx) { hle_ret(ctx, 8); }
void hle_D3D_StubRet4(CpuContext& ctx) { hle_ret(ctx, 4); }
void hle_D3D_StubRet0(CpuContext& ctx) { hle_ret(ctx, 0); }
void hle_D3D_StubRet4_1(CpuContext& ctx) { hle_ret_u32(ctx, 1u, 4); }
void hle_D3D_StubRet4_0(CpuContext& ctx) { hle_ret_u32(ctx, 0u, 4); }

void hle_D3D_SetTransform(CpuContext& ctx) {
    uint32_t state = hle_arg(ctx, 0);
    uint32_t pm = hle_arg(ctx, 1);
    if (pm) {
        int slot = -1;
        if (state == 0u || state == 2u) slot = 0; // VIEW
        else if (state == 1u || state == 3u) slot = 1; // PROJ
        else if (state >= 256u && state < 260u) slot = 6; // WORLD
        else if (state >= 16u && state < 20u) slot = static_cast<int>(2u + (state - 16u));
        else if (state >= 4u && state < 8u) slot = static_cast<int>(state - 2u);
        if (slot >= 0 && slot < 8) {
            std::memcpy(g_matrices[slot], mem_ptr(pm), 64);
        }
    }
    hle_ret(ctx, 8);
}

void hle_D3D_SetRenderTarget(CpuContext& ctx) {
    // 0x002370B0: (pRenderTarget, pZStencil) -> ret 8
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    ensure_guest_device_allocated();
    uint32_t prt = hle_arg(ctx, 0);
    uint32_t pds = hle_arg(ctx, 1);
    if (prt) {
        g_cur_rt_va = prt;
        auto sit = g_surfaces.find(prt);
        if (sit != g_surfaces.end()) {
            if (sit->second.gpu) sit->second.gpu->is_render_target = true;
            g_vp_x = 0.0f;
            g_vp_y = 0.0f;
            g_vp_w = static_cast<float>(std::max(1u, sit->second.width));
            g_vp_h = static_cast<float>(std::max(1u, sit->second.height));
            g_vp_minz = 0.0f;
            g_vp_maxz = 1.0f;
            if (g_hle_trace) {
                std::printf("[D3D8] SetRenderTarget prt=0x%08X pds=0x%08X (%ux%u)\n",
                            prt, pds, sit->second.width, sit->second.height);
            }
        }
    }
    g_cur_ds_va = pds;
    hle_ret_u32(ctx, 0u, 8);
}

void hle_D3D_BlockUntilVerticalBlank(CpuContext& ctx) {
    // 0x00237680: () -> ret 0
    hle_kernel_notify_frame();
    hle_ret(ctx, 0);
}

void hle_D3D_SetLight(CpuContext& ctx) {
    // 0x002387B0: (Index, pLight [0x68 bytes]) -> ret 8
    uint32_t idx = hle_arg(ctx, 0);
    uint32_t pl  = hle_arg(ctx, 1);
    if (idx < 64u && pl) {
        auto& L = g_lights[idx];
        L.type = rd32(pl + 0x00u);
        std::memcpy(L.diffuse,  mem_ptr(pl + 0x04u), 16);
        std::memcpy(L.specular, mem_ptr(pl + 0x14u), 16);
        std::memcpy(L.ambient,  mem_ptr(pl + 0x24u), 16);
        std::memcpy(L.pos,      mem_ptr(pl + 0x34u), 12);
        std::memcpy(L.dir,      mem_ptr(pl + 0x40u), 12);
        L.range   = rdf32(pl + 0x4Cu);
        L.falloff = rdf32(pl + 0x50u);
        L.a0      = rdf32(pl + 0x54u);
        L.a1      = rdf32(pl + 0x58u);
        L.a2      = rdf32(pl + 0x5Cu);
        L.theta   = rdf32(pl + 0x60u);
        L.phi     = rdf32(pl + 0x64u);
    }
    hle_ret_u32(ctx, 0u, 8);
}

void hle_D3D_LightEnable(CpuContext& ctx) {
    // 0x00238990: (Index, Enable) -> ret 8
    uint32_t idx = hle_arg(ctx, 0);
    uint32_t en  = hle_arg(ctx, 1);
    if (idx < 64u) g_lights[idx].enabled = (en != 0);
    hle_ret_u32(ctx, 0u, 8);
}

void hle_D3D_SetMaterial(CpuContext& ctx) {
    uint32_t pm = hle_arg(ctx, 0);
    if (pm) {
        std::memcpy(g_mat_diffuse,  mem_ptr(pm + 0), 16);
        std::memcpy(g_mat_ambient,  mem_ptr(pm + 16), 16);
        std::memcpy(g_mat_specular, mem_ptr(pm + 32), 12);
        g_mat_specular[3] = rdf32(pm + 64);
        std::memcpy(g_mat_emissive, mem_ptr(pm + 48), 16);
    }
    hle_ret(ctx, 4);
}

void hle_D3D_GetVisibilityTestResult(CpuContext& ctx) {
    uint32_t pres = hle_arg(ctx, 1);
    uint32_t ptime = hle_arg(ctx, 2);
    if (pres) wr32(pres, 1u); // visible!
    if (ptime) wr64(ptime, 0ull);
    hle_ret_u32(ctx, 0u, 12);
}

void hle_D3D_GetBackBuffer(CpuContext& ctx) {
    ensure_guest_device_allocated();
    int32_t idx = static_cast<int32_t>(hle_arg(ctx, 0));
    uint32_t surf = (idx == -1) ? g_front_surf_va : g_back_surf_va;
    hle_ret_u32(ctx, surf, 4);
}

void hle_D3D_GetRenderTarget2(CpuContext& ctx) {
    ensure_guest_device_allocated();
    hle_ret_u32(ctx, g_cur_rt_va, 0);
}

void hle_D3D_GetDepthStencilSurface2(CpuContext& ctx) {
    ensure_guest_device_allocated();
    hle_ret_u32(ctx, g_cur_ds_va, 0);
}

void hle_D3D_SetViewport(CpuContext& ctx) {
    uint32_t pvp = hle_arg(ctx, 0);
    if (pvp) {
        g_vp_x = static_cast<float>(rd32(pvp + 0));
        g_vp_y = static_cast<float>(rd32(pvp + 4));
        g_vp_w = static_cast<float>(rd32(pvp + 8));
        g_vp_h = static_cast<float>(rd32(pvp + 12));
        g_vp_minz = rdf32(pvp + 16);
        g_vp_maxz = rdf32(pvp + 20);
        if (g_hle_trace) {
            std::printf("[D3D8] SetViewport(%.0f, %.0f, %.0f, %.0f, z=[%.2f, %.2f]) fb=%ux%u\n",
                        g_vp_x, g_vp_y, g_vp_w, g_vp_h, g_vp_minz, g_vp_maxz, g_xb_fb_w, g_xb_fb_h);
        }
    }
    hle_ret(ctx, 4);
}

void hle_D3D_SetTexture(CpuContext& ctx) {
    uint32_t stage = hle_arg(ctx, 0);
    uint32_t ptex = hle_arg(ctx, 1);
    if (stage < 4u) {
        g_bound_tex_va[stage] = ptex;
        if (g_dev_va) wr32(g_dev_va + 0xB38u + stage * 4u, ptex);
    }
    hle_ret(ctx, 8);
}

void hle_D3D_SetIndices(CpuContext& ctx) {
    uint32_t pib = hle_arg(ctx, 0);
    int32_t base_vtx = static_cast<int32_t>(hle_arg(ctx, 1));
    g_cur_ib_va = pib;
    g_base_vertex_index = base_vtx;
    if (g_dev_va) {
        wr32(g_dev_va + 0x38Cu, pib);
        wr32(g_dev_va + 0x1Cu, static_cast<uint32_t>(base_vtx));
    }
    uint32_t data_ptr = 0;
    if (pib) {
        auto it = g_ibs.find(pib);
        data_ptr = (it != g_ibs.end()) ? it->second.data_va : rd32(pib + 4u);
    }
    wr32(kGuestIndexBaseVA, data_ptr);
    hle_ret(ctx, 8);
}

void hle_D3D_EndVisibilityTest(CpuContext& ctx) {
    // Xbox D3D8: HRESULT D3DDevice::EndVisibilityTest(DWORD Index) takes Index by value, not pointer.
    uint32_t idx = hle_arg(ctx, 0);
    g_vis_counter = idx;
    hle_ret_u32(ctx, 0u, 4);
}

void hle_D3D_GetDisplayFieldStatus(CpuContext& ctx) {
    uint32_t p = hle_arg(ctx, 0);
    if (p) { wr32(p + 0, 0); wr32(p + 4, 1); }
    hle_ret_u32(ctx, 0u, 4);
}

void hle_D3D_CreateTexture2(CpuContext& ctx) {
    // (Width, Height, Depth, Levels, Usage, Format, ResourceType) -> ret 28
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    ensure_guest_device_allocated();
    uint32_t w = std::max(1u, hle_arg(ctx, 0));
    uint32_t h = std::max(1u, hle_arg(ctx, 1));
    uint32_t d = std::max(1u, hle_arg(ctx, 2));
    uint32_t levels = std::max(1u, hle_arg(ctx, 3));
    uint32_t usage = hle_arg(ctx, 4);
    uint32_t fmt = hle_arg(ctx, 5);
    uint32_t rtype = hle_arg(ctx, 6); // 3=2D, 4=Vol, 5=Cube

    uint32_t pitch = calc_pitch_xb(w, fmt);
    uint32_t total_data = 0;
    uint32_t mw = w, mh = h, md = (rtype == 4u) ? d : 1u;
    for (uint32_t l = 0; l < levels; ++l) {
        total_data += (calc_surf_bytes(mw, mh, fmt) * md + 63u) & ~63u;
        if (mw > 1) mw >>= 1;
        if (mh > 1) mh >>= 1;
        if (md > 1) md >>= 1;
    }
    uint32_t faces = (rtype == 5u) ? 6u : 1u;
    uint32_t data_bytes = std::max(64u, total_data * faces);
    uint32_t data_va = xbox_guest_alloc(data_bytes, 64u, true);
    uint32_t tex_va  = xbox_guest_alloc(0x20u, 16u, true);

    write_surface_struct(tex_va, data_va, fmt, w, h, pitch, (rtype == 5u) ? 5u : ((rtype == 4u) ? 4u : 3u));

    auto* gpu = new GpuTextureObj();
    gpu->guest_va = tex_va;
    gpu->width = w;
    gpu->height = h;
    gpu->levels = levels;
    gpu->xb_format = fmt;
    gpu->pitch = pitch;
    gpu->data_va = data_va;
    gpu->is_cube = (rtype == 5u);
    gpu->is_render_target = (usage & 1u) != 0;
    gpu->dirty = true;

    GuestTexture gt{};
    gt.tex_va = tex_va;
    gt.gpu = gpu;
    gt.levels = levels;
    gt.data_bytes = data_bytes;
    uint32_t off = 0;
    for (uint32_t f = 0; f < faces; ++f) {
        mw = w; mh = h;
        for (uint32_t l = 0; l < levels; ++l) {
            uint32_t lp = calc_pitch_xb(mw, fmt);
            uint32_t s_va = xbox_guest_alloc(0x20u, 16u, true);
            write_surface_struct(s_va, data_va + off, fmt, mw, mh, lp, 1u);
            g_surfaces[s_va] = {s_va, tex_va, gpu, mw, mh, fmt, lp, data_va + off, l, false, false, 0};
            gt.level_surf_vas.push_back(s_va);
            if (is_compressed_xb(fmt)) {
                uint32_t bw = std::max(1u, (mw + 3u) / 4u);
                uint32_t bh = std::max(1u, (mh + 3u) / 4u);
                off += bw * bh * (fmt == 0x0Cu ? 8u : 16u);
            } else {
                bool is_lin = (fmt == 0x12u || fmt == 0x1Eu || fmt == 0x1Fu || fmt == 0x10u ||
                               fmt == 0x11u || fmt == 0x16u || fmt == 0x17u || fmt == 0x1Du ||
                               fmt == 0x19u || fmt == 0x1Au);
                uint32_t bpp_bytes = std::max(1u, bits_per_pixel_xb(fmt) / 8u);
                uint32_t row_p = (is_lin && l == 0) ? lp : (mw * bpp_bytes);
                off += mh * row_p;
            }
            if (mw > 1) mw >>= 1;
            if (mh > 1) mh >>= 1;
        }
        off = (off + 63u) & ~63u;
    }
    g_textures[tex_va] = std::move(gt);
    hle_ret_u32(ctx, tex_va, 28);
}

void hle_D3D_GetSurfaceLevel2(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    uint32_t tva = hle_arg(ctx, 0);
    uint32_t lvl = hle_arg(ctx, 1);
    auto it = g_textures.find(tva);
    if (it != g_textures.end() && lvl < it->second.level_surf_vas.size()) {
        hle_ret_u32(ctx, it->second.level_surf_vas[lvl], 8);
    } else {
        hle_ret_u32(ctx, g_back_surf_va, 8);
    }
}

void hle_D3D_GetCubeMapSurface2(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    uint32_t tva = hle_arg(ctx, 0);
    uint32_t face = std::min(5u, hle_arg(ctx, 1));
    uint32_t lvl = hle_arg(ctx, 2);
    auto it = g_textures.find(tva);
    if (it != g_textures.end()) {
        uint32_t idx = face * it->second.levels + lvl;
        if (idx < it->second.level_surf_vas.size()) {
            hle_ret_u32(ctx, it->second.level_surf_vas[idx], 12);
            return;
        }
    }
    hle_ret_u32(ctx, g_back_surf_va, 12);
}

void hle_D3D_CreateImageSurface(CpuContext& ctx) {
    // (Width, Height, MultisampleType, Format) -> ret 16
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    ensure_guest_device_allocated();
    uint32_t w = std::max(1u, hle_arg(ctx, 0));
    uint32_t h = std::max(1u, hle_arg(ctx, 1));
    uint32_t fmt = hle_arg(ctx, 3);
    if (fmt == 0) fmt = 0x1Eu;
    uint32_t pitch = calc_pitch_xb(w, fmt);
    uint32_t bytes = std::max(64u, calc_surf_bytes(w, h, fmt));
    uint32_t data_va = xbox_guest_alloc(bytes, 64u, true);
    uint32_t surf_va = xbox_guest_alloc(0x20u, 16u, true);
    write_surface_struct(surf_va, data_va, fmt, w, h, pitch, 1u);

    auto* gpu = new GpuTextureObj();
    gpu->guest_va = surf_va;
    gpu->width = w; gpu->height = h; gpu->levels = 1;
    gpu->xb_format = fmt; gpu->pitch = pitch; gpu->data_va = data_va; gpu->dirty = true;

    g_surfaces[surf_va] = {surf_va, 0, gpu, w, h, fmt, pitch, data_va, 0, true, false, 0};
    hle_ret_u32(ctx, surf_va, 16);
}

void hle_D3D_SurfaceLockRect(CpuContext& ctx) {
    // (pSurf, pLockedRect, pRect, Flags) -> ret 16
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    uint32_t sva = hle_arg(ctx, 0);
    uint32_t plock = hle_arg(ctx, 1);
    uint32_t prect = hle_arg(ctx, 2);
    uint32_t pbits = 0, pitch = 0;

    auto sit = g_surfaces.find(sva);
    if (sit != g_surfaces.end()) {
        auto& s = sit->second;
        s.locked_ever = true;
        if (s.gpu) s.gpu->dirty = true;
        if (s.parent_tex_va) {
            auto tit = g_textures.find(s.parent_tex_va);
            if (tit != g_textures.end()) {
                tit->second.locked_ever = true;
                tit->second.unchanged_checks = 0;
                tit->second.next_check_frame = 0;
            }
        }
        pitch = s.pitch;
        pbits = s.data_va;
        if (prect && !is_compressed_xb(s.xb_format)) {
            uint32_t rx = rd32(prect + 0), ry = rd32(prect + 4);
            pbits += ry * pitch + rx * (bits_per_pixel_xb(s.xb_format) / 8u);
        }
    } else if (sva) {
        pbits = rd32(sva + 4u);
        pitch = rd32(sva + 0x10u);
    }
    if (plock) {
        wr32(plock + 0, pitch);
        wr32(plock + 4, pbits);
    }
    hle_ret_u32(ctx, 0u, 16);
}

void hle_D3D_TextureLockRect_Vol(CpuContext& ctx) {
    // 0x00239C90: IDirect3DVolumeTexture8::LockBox (pVolTex, Level, pLockedBox, pBox, Flags) -> ret 20
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    uint32_t tva = hle_arg(ctx, 0);
    uint32_t lvl = hle_arg(ctx, 1);
    uint32_t plock = hle_arg(ctx, 2);
    uint32_t pbox = hle_arg(ctx, 3);
    auto it = g_textures.find(tva);
    if (it != g_textures.end() && lvl < it->second.level_surf_vas.size()) {
        it->second.locked_ever = true;
        it->second.unchanged_checks = 0;
        it->second.next_check_frame = 0;
        auto sit = g_surfaces.find(it->second.level_surf_vas[lvl]);
        if (sit != g_surfaces.end() && plock) {
            const auto& s = sit->second;
            if (s.gpu) s.gpu->dirty = true;
            uint32_t slice_pitch = calc_surf_bytes(s.width, s.height, s.xb_format);
            uint32_t ptr = s.data_va;
            if (pbox) {
                uint32_t left = rd32(pbox + 0);
                uint32_t top = rd32(pbox + 4);
                uint32_t front = rd32(pbox + 16);
                bool dxt = (s.xb_format == 0x0Cu || s.xb_format == 0x0Du || s.xb_format == 0x0Eu || s.xb_format == 0x0Fu);
                if (dxt) {
                    uint32_t blk_b = (s.xb_format == 0x0Cu) ? 8u : 16u;
                    ptr += front * slice_pitch + (top / 4u) * s.pitch + (left / 4u) * blk_b;
                } else {
                    uint32_t bpp_b = std::max(1u, bits_per_pixel_xb(s.xb_format) / 8u);
                    ptr += front * slice_pitch + top * s.pitch + left * bpp_b;
                }
            }
            wr32(plock + 0, s.pitch);      // RowPitch
            wr32(plock + 4, slice_pitch);  // SlicePitch
            wr32(plock + 8, ptr);          // pBits
        }
    }
    hle_ret_u32(ctx, 0u, 20);
}

void hle_D3D_SurfaceGetDesc(CpuContext& ctx) {
    // 0x0023CDB0: (pSurf, pDesc) -> ret 8
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    uint32_t sva = hle_arg(ctx, 0);
    uint32_t pdesc = hle_arg(ctx, 1);
    if (pdesc) {
        auto sit = g_surfaces.find(sva);
        if (sit != g_surfaces.end()) {
            auto& s = sit->second;
            wr32(pdesc + 0x00u, s.xb_format);
            wr32(pdesc + 0x04u, 1u);
            wr32(pdesc + 0x08u, 0u);
            wr32(pdesc + 0x0Cu, calc_surf_bytes(s.width, s.height, s.xb_format));
            wr32(pdesc + 0x10u, 0u);
            wr32(pdesc + 0x14u, s.width);
            wr32(pdesc + 0x18u, s.height);
        } else {
            wr32(pdesc + 0x00u, 0x1Eu);
            wr32(pdesc + 0x04u, 1u);
            wr32(pdesc + 0x08u, 0u);
            wr32(pdesc + 0x0Cu, 0u);
            wr32(pdesc + 0x10u, 0u);
            wr32(pdesc + 0x14u, 640u);
            wr32(pdesc + 0x18u, 480u);
        }
    }
    hle_ret(ctx, 8);
}

void hle_D3D_CopyRects(CpuContext& ctx) {
    // 0x00238340: (pSrcSurf, pSrcRects, cRects, pDstSurf, pDstPoints) -> ret 20
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    uint32_t src_va = hle_arg(ctx, 0);
    uint32_t dst_va = hle_arg(ctx, 3);
    auto sit = g_surfaces.find(src_va);
    auto dit = g_surfaces.find(dst_va);
    if (sit != g_surfaces.end() && dit != g_surfaces.end()) {
        auto& s = sit->second;
        auto& d = dit->second;
        uint32_t rows = std::min(s.height, d.height);
        uint32_t copy_b = std::min(s.pitch, d.pitch);
        if (s.data_va && d.data_va && copy_b) {
            for (uint32_t y = 0; y < rows; ++y) {
                std::memcpy(mem_ptr(d.data_va + y * d.pitch), mem_ptr(s.data_va + y * s.pitch), copy_b);
            }
        }
        d.locked_ever = true;
        if (d.gpu) d.gpu->dirty = true;
        if (d.parent_tex_va) {
            auto tit = g_textures.find(d.parent_tex_va);
            if (tit != g_textures.end()) {
                tit->second.locked_ever = true;
                tit->second.unchanged_checks = 0;
                tit->second.next_check_frame = 0;
            }
        }
    }
    hle_ret_u32(ctx, 0u, 20);
}

void hle_D3D_Clear(CpuContext& ctx) {
    // 0x00239D10: (Count, pRects, Flags, Color, Z, Stencil) -> ret 24
    // Xbox D3DCLEAR flags differ from PC D3D8 / gfx_clear flags:
    //   Xbox: D3DCLEAR_ZBUFFER = 0x01, D3DCLEAR_STENCIL = 0x02, D3DCLEAR_TARGET* = 0xF0 (RGBA bits 4..7)
    //   gfx_clear: 1 = color target, 2 = depth, 4 = stencil
    uint32_t xb_flags = hle_arg(ctx, 2);
    uint32_t color = hle_arg(ctx, 3);
    uint32_t z_u32 = hle_arg(ctx, 4);
    uint32_t stencil = hle_arg(ctx, 5);
    float z = 1.0f;
    std::memcpy(&z, &z_u32, 4);
    uint32_t gfx_flags = 0;
    if (xb_flags & 0xF0u) gfx_flags |= 1u;
    if (xb_flags & 0x01u) gfx_flags |= 2u;
    if (xb_flags & 0x02u) gfx_flags |= 4u;

    GpuTextureObj* custom_rt = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_d3d_mtx);
        if (g_cur_rt_va && g_cur_rt_va != g_back_surf_va && g_cur_rt_va != g_front_surf_va) {
            auto sit = g_surfaces.find(g_cur_rt_va);
            if (sit != g_surfaces.end() && sit->second.gpu) {
                sit->second.gpu->is_render_target = true;
                custom_rt = sit->second.gpu;
            }
        }
    }
    gfx_clear(gfx_flags, color, z, stencil, custom_rt);
    hle_ret(ctx, 24);
}

void hle_D3D_DrawVerticesUP(CpuContext& ctx) {
    // 0x0023A3F0: (PrimitiveType, VertexCount, pVerts, Stride) -> ret 16
    uint32_t prim_type = hle_arg(ctx, 0);
    uint32_t vtx_count = hle_arg(ctx, 1);
    uint32_t verts_va  = hle_arg(ctx, 2);
    uint32_t stride    = hle_arg(ctx, 3);
    if (vtx_count && verts_va) {
        sync_and_draw(prim_type, vtx_count, 0u, mem_ptr(verts_va), stride, nullptr, 0);
    }
    hle_ret(ctx, 16);
}

void hle_D3D_DrawVertices(CpuContext& ctx) {
    // 0x0023A550: (PrimitiveType, StartVertex, VertexCount) -> ret 12
    uint32_t prim_type = hle_arg(ctx, 0);
    int32_t start_vtx = static_cast<int32_t>(hle_arg(ctx, 1));
    uint32_t vtx_count = hle_arg(ctx, 2);
    if (vtx_count) {
        sync_and_draw(prim_type, vtx_count, 0u, nullptr, 0u, nullptr, start_vtx);
    }
    hle_ret(ctx, 12);
}

void hle_D3D_DrawIndexedVertices(CpuContext& ctx) {
    // 0x0023A730: (PrimitiveType, IndexCount, pIndices) -> ret 12
    uint32_t prim_type = hle_arg(ctx, 0);
    uint32_t idx_count = hle_arg(ctx, 1);
    uint32_t pindices  = hle_arg(ctx, 2);
    if (!pindices) pindices = rd32(kGuestIndexBaseVA);
    if (idx_count && pindices && pindices < XBOX_MEM_SIZE) {
        const uint16_t* idx_ptr = reinterpret_cast<const uint16_t*>(mem_ptr(pindices));
        sync_and_draw(prim_type, 0u, idx_count, nullptr, 0u, idx_ptr, g_base_vertex_index);
    }
    hle_ret(ctx, 12);
}

void hle_D3D_CreateVertexShader(CpuContext& ctx) {
    // 0x0023B200: (pDeclaration, pFunction, pHandle, Usage) -> ret 16
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    uint32_t decl_va = hle_arg(ctx, 0);
    uint32_t fn_va   = hle_arg(ctx, 1);
    uint32_t phandle = hle_arg(ctx, 2);

    uint32_t obj_va = xbox_guest_alloc(0x120u, 16u, true);
    uint32_t handle = obj_va | 1u;

    GuestVS gvs{};
    gvs.handle = handle;
    gvs.obj_va = obj_va;
    if (decl_va) gvs.decl = nv2a_parse_declaration(decl_va);
    if (fn_va) {
        uint16_t magic = rd16(fn_va + 0);
        uint16_t n_insns = rd16(fn_va + 2);
        size_t bytes = 4u + static_cast<size_t>(n_insns) * 16u;
        if (bytes >= 4u && bytes <= 136u * 16u + 4u) {
            gvs.microcode.resize(bytes);
            std::memcpy(gvs.microcode.data(), mem_ptr(fn_va), bytes);
            gvs.has_microcode = true;
        }
        (void)magic;
    }
    wr32(obj_va + 0, 1u);
    g_vshaders[handle] = std::move(gvs);
    if (phandle) wr32(phandle, handle);
    hle_ret_u32(ctx, 0u, 16);
}

void copy_vs_consts(uint32_t reg, uint32_t src_va, uint32_t vec4_count) {
    if (!src_va || reg >= 192u) return;
    uint32_t n = std::min(vec4_count, 192u - reg);
    std::memcpy(&g_vs_consts[reg * 4u], mem_ptr(src_va), n * 16u);
    std::memcpy(mem_ptr(kGuestVSConstVA + reg * 16u), mem_ptr(src_va), n * 16u);
}

void hle_D3D_SetVSConstant1(CpuContext& ctx) {
    // 0x0023B360: __fastcall (ecx = reg+96, edx = pData)
    copy_vs_consts(ctx.ecx, ctx.edx, 1u);
    hle_ret(ctx, 0);
}
void hle_D3D_SetVSConstant4(CpuContext& ctx) {
    // 0x0023B410: __fastcall (ecx = reg+96, edx = pData)
    copy_vs_consts(ctx.ecx, ctx.edx, 4u);
    hle_ret(ctx, 0);
}
void hle_D3D_SetVSConstantN(CpuContext& ctx) {
    // 0x0023B5A0: __fastcall + stdcall stack arg DWORD count (ret 4)
    uint32_t dwords = hle_arg(ctx, 0);
    copy_vs_consts(ctx.ecx, ctx.edx, (dwords + 3u) / 4u);
    hle_ret(ctx, 4);
}

void hle_D3D_SetStreamSource(CpuContext& ctx) {
    uint32_t stream = hle_arg(ctx, 0);
    uint32_t pvb    = hle_arg(ctx, 1);
    uint32_t stride = hle_arg(ctx, 2);
    if (stream < 4u) {
        g_vb_stream_obj[stream] = pvb;
        g_vb_stream_stride[stream] = stride;
        if (g_dev_va) {
            wr32(0x00252E80u + stream * 12u, stride);
            wr32(0x00252E88u + stream * 12u, pvb);
        }
    }
    hle_ret(ctx, 12);
}

void hle_D3D_SetVertexShader(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    g_cur_vs_handle = h;
    if (g_dev_va) wr32(g_dev_va + 0x380u, h);
    hle_ret(ctx, 4);
}

void hle_D3D_CreatePixelShader(CpuContext& ctx) {
    // 0x0023E570: (pShaderDef, pHandle) -> ret 8
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    uint32_t pdef = hle_arg(ctx, 0);
    uint32_t ph   = hle_arg(ctx, 1);
    uint32_t handle = xbox_guest_alloc(0x100u, 16u, true);
    GuestPS gps{};
    gps.handle = handle;
    if (pdef) {
        std::memcpy(gps.psdef, mem_ptr(pdef), 240);
    }
    wr32(handle + 0, 1u);
    g_pshaders[handle] = gps;
    if (ph) wr32(ph, handle);
    hle_ret_u32(ctx, 0u, 8);
}

void hle_D3D_SetPixelShader(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    g_cur_ps_handle = h;
    if (g_dev_va) wr32(g_dev_va + 0x370u, h);
    hle_ret(ctx, 4);
}

void hle_D3D_SetPixelShaderConstant(CpuContext& ctx) {
    // 0x0023EC20: (Register, pData, ConstantCount) -> ret 12
    uint32_t reg = hle_arg(ctx, 0);
    uint32_t pd  = hle_arg(ctx, 1);
    uint32_t cnt = hle_arg(ctx, 2);
    if (pd && reg < 16u) {
        uint32_t n = std::min(cnt, 16u - reg);
        std::memcpy(&g_ps_consts[reg * 4u], mem_ptr(pd), n * 16u);
    }
    hle_ret(ctx, 12);
}

void hle_D3D_CreateVertexBuffer2(CpuContext& ctx) {
    // 0x0023E400: (Length) -> returns D3DVertexBuffer* in EAX, ret 4
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    uint32_t len = std::max(16u, hle_arg(ctx, 0));
    uint32_t obj_va = xbox_guest_alloc(0x10u, 16u, true);
    uint32_t data_va = xbox_guest_alloc(len, 64u, true);
    wr32(obj_va + 0, 0x00010001u);
    wr32(obj_va + 4, data_va);
    g_vbs[obj_va] = {obj_va, data_va, len};
    hle_ret_u32(ctx, obj_va, 4);
}

void hle_D3D_CreateIndexBuffer2(CpuContext& ctx) {
    // 0x0023E350: (Length) -> returns D3DIndexBuffer* in EAX, ret 4
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    uint32_t len = std::max(16u, hle_arg(ctx, 0));
    uint32_t obj_va = xbox_guest_alloc(0x10u, 16u, true);
    uint32_t data_va = xbox_guest_alloc(len, 64u, true);
    wr32(obj_va + 0, 0x00020001u);
    wr32(obj_va + 4, data_va);
    g_ibs[obj_va] = {obj_va, data_va, len};
    hle_ret_u32(ctx, obj_va, 4);
}

void hle_D3D_VertexBufferLock(CpuContext& ctx) {
    // 0x0023E450: (pVB, Flags) -> returns Data VA WITHOUT 0x80000000 uncached bit! ret 8
    uint32_t pvb = hle_arg(ctx, 0);
    if (pvb) {
        std::lock_guard<std::mutex> lk(g_d3d_mtx);
        auto vit = g_vbs.find(pvb);
        if (vit != g_vbs.end()) vit->second.dirty_seq++;
        auto iit = g_ibs.find(pvb);
        if (iit != g_ibs.end()) iit->second.dirty_seq++;
    }
    uint32_t ptr = pvb ? rd32(pvb + 4u) : 0u;
    hle_ret_u32(ctx, ptr, 8);
}

} // namespace
static bool g_hd720_req = false;
void hle_d3d8_set_hd720(bool enable) { g_hd720_req = enable; }
void hle_d3d8_mark_bink_frame(uint32_t va) {
    if (!va) return;
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    for (auto& [tva, gt] : g_textures) {
        (void)tva;
        if (!gt.gpu || !gt.gpu->data_va || !gt.data_bytes) continue;
        uint32_t start = gt.gpu->data_va;
        uint32_t end = start + gt.data_bytes;
        if (va >= start && va < end) {
            gt.gpu->dirty = true;
            gt.gpu->is_bink_frame = true;
            gt.locked_ever = true;
            gt.unchanged_checks = 0;
            gt.next_check_frame = 0;
        }
    }
}
namespace {

void hle_D3D_GetDeviceCaps(CpuContext& ctx) {
    // 0x0023CE20: (Adapter, Type, pCaps) -> copy from XBE template at 0x00248E90 (0x10B dwords)
    uint32_t adapter = hle_arg(ctx, 0);
    uint32_t pcaps = hle_arg(ctx, 2);
    if (adapter != 0) { hle_ret_u32(ctx, 0x8876086Cu, 12); return; }
    if (pcaps) std::memcpy(mem_ptr(pcaps), mem_ptr(0x00248E90u), 0x10Bu * 4u);
    if (g_hd720_req) {
        wr32(0x0037D900u, 4u);
    } else if (gfx_settings().widescreen) {
        wr32(0x0037D900u, 1u); // True 16:9 Widescreen 640x480 mode cleanly scaled to host HD
    }
    hle_ret_u32(ctx, 0u, 12);
}

void hle_D3D_GetAdapterModeCount(CpuContext& ctx) {
    // 0x0023CE50: (Adapter) -> return 3 modes when widescreen (480p 4:3, 480p 16:9, 720p 16:9)
    (void)hle_arg(ctx, 0);
    hle_ret_u32(ctx, gfx_settings().widescreen ? 3u : 1u, 4);
}

void hle_D3D_EnumAdapterModes(CpuContext& ctx) {
    // 0x0023CEF0: (Adapter, ModeIndex, pMode) -> ret 12
    uint32_t adapter = hle_arg(ctx, 0);
    uint32_t idx = hle_arg(ctx, 1);
    uint32_t pmode = hle_arg(ctx, 2);
    if (adapter != 0 || !pmode) { hle_ret_u32(ctx, 0x8876086Cu, 12); return; }

    bool ws = gfx_settings().widescreen;
    if (idx == 0) {
        wr32(pmode + 0x00u, 640u);
        wr32(pmode + 0x04u, 480u);
        wr32(pmode + 0x08u, 60u);
        wr32(pmode + 0x0Cu, 0x40u); // PROGRESSIVE (mode 2: 4:3)
        wr32(pmode + 0x10u, 0x1Eu); // D3DFMT_LIN_A8R8G8B8
        hle_ret_u32(ctx, 0u, 12);
    } else if (ws && idx == 1) {
        wr32(pmode + 0x00u, 640u);
        wr32(pmode + 0x04u, 480u);
        wr32(pmode + 0x08u, 60u);
        wr32(pmode + 0x0Cu, 0x50u); // PROGRESSIVE | WIDESCREEN (mode 1: 16:9 480p!)
        wr32(pmode + 0x10u, 0x1Eu);
        hle_ret_u32(ctx, 0u, 12);
    } else if (ws && idx == 2) {
        wr32(pmode + 0x00u, 1280u);
        wr32(pmode + 0x04u, 720u);
        wr32(pmode + 0x08u, 60u);
        wr32(pmode + 0x0Cu, 0x50u); // PROGRESSIVE | WIDESCREEN (mode 4: 720p)
        wr32(pmode + 0x10u, 0x1Eu);
        hle_ret_u32(ctx, 0u, 12);
    } else {
        hle_ret_u32(ctx, 0x8876086Cu, 12);
    }
}

void hle_D3D_CheckDeviceMultiSampleType(CpuContext& ctx) {
    uint32_t adapter = hle_arg(ctx, 0);
    uint32_t devtype = hle_arg(ctx, 1);
    if (adapter != 0) { hle_ret_u32(ctx, 0x8876086Cu, 12); return; }
    if (devtype != 1u) { hle_ret_u32(ctx, 0x8876086Bu, 12); return; }
    hle_ret_u32(ctx, 0u, 12);
}

void hle_D3D_CreateDevice(CpuContext& ctx) {
    // 0x0023D340: (Adapter, DevType, hWnd, BehaviorFlags, pPP, ppDev) -> ret 24
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    uint32_t ppp = hle_arg(ctx, 4);
    uint32_t ppdev = hle_arg(ctx, 5);
    if (ppp) {
        uint32_t w = rd32(ppp + 0x00u);
        uint32_t h = rd32(ppp + 0x04u);
        uint32_t flags = rd32(ppp + 0x28u);
        if (w >= 320u && w <= 1920u) g_xb_fb_w = w;
        if (h >= 240u && h <= 1080u) g_xb_fb_h = h;
        g_xb_widescreen_mode = (flags & 0x10u) != 0;
        std::printf("[D3D8] CreateDevice guest FB=%ux%u (Flags=0x%X, Widescreen=%d)\n",
                    g_xb_fb_w, g_xb_fb_h, flags, g_xb_widescreen_mode ? 1 : 0);
        hle_log_note("[D3D8] Guest framebuffer: %ux%u (flags 0x%X)", g_xb_fb_w, g_xb_fb_h, flags);
    }
    ensure_guest_device_allocated();
    g_front_gpu.width = g_back_gpu.width = g_depth_gpu.width = g_xb_fb_w;
    g_front_gpu.height = g_back_gpu.height = g_depth_gpu.height = g_xb_fb_h;
    g_front_gpu.pitch = g_back_gpu.pitch = calc_pitch_xb(g_xb_fb_w, 0x1Eu);
    g_depth_gpu.pitch = calc_pitch_xb(g_xb_fb_w, 0x2Au);
    for (uint32_t sva : {g_front_surf_va, g_back_surf_va, g_depth_surf_va}) {
        auto sit = g_surfaces.find(sva);
        if (sit != g_surfaces.end()) {
            sit->second.width = g_xb_fb_w;
            sit->second.height = g_xb_fb_h;
            sit->second.pitch = (sva == g_depth_surf_va) ? g_depth_gpu.pitch : g_back_gpu.pitch;
            write_surface_struct(sva, sit->second.data_va, sit->second.xb_format, g_xb_fb_w, g_xb_fb_h, sit->second.pitch, 1u);
        }
    }
    g_vp_x = 0.0f; g_vp_y = 0.0f;
    g_vp_w = static_cast<float>(g_xb_fb_w);
    g_vp_h = static_cast<float>(g_xb_fb_h);
    if (ppdev) wr32(ppdev, g_dev_va);
    hle_ret_u32(ctx, 0u, 24);
}

void hle_D3D_GetSuperSampleScale(CpuContext& ctx) {
    uint32_t px = hle_arg(ctx, 0), py = hle_arg(ctx, 1);
    if (px) wrf32(px, 1.0f);
    if (py) wrf32(py, 1.0f);
    hle_ret(ctx, 8);
}

void hle_D3D_Swap(CpuContext& ctx) {
    // 0x0023E090: Swap(Flags) -> ret 4
    if (hle_kernel_is_shutting_down()) throw GuestPowerOff{};
    bool has_3d = false;
    {
        std::lock_guard<std::mutex> lk(g_d3d_mtx);
        if (g_cur_frame_had_prog_ss && !g_cur_frame_saw_hd_ss) {
            g_ss_prog_fb_mode = false;
        }
        g_cur_frame_had_prog_ss = false;
        g_cur_frame_saw_hd_ss = false;
        has_3d = (g_cur_frame_3d_draws > 0);
        g_cur_frame_3d_draws = 0;
        ui_set_start_screen(g_cur_frame_start_screen.exchange(false, std::memory_order_relaxed));
    }
    hle_kernel_notify_frame();
    uint64_t f = g_frame_counter.fetch_add(1ull) + 1ull;
    app_on_frame_presented(f);
    gfx_present();
    rl_ach_frame();
    {
        static int s_last_mode = -1;
        static int s_pending_mode = -1;
        static int s_pending_streak = 0;
        int cur_mode = has_3d ? 1 : 0;
        if (cur_mode != s_pending_mode) {
            s_pending_mode = cur_mode;
            s_pending_streak = 1;
        } else {
            ++s_pending_streak;
        }
        if (cur_mode != s_last_mode && s_pending_streak >= 45) {
            s_last_mode = cur_mode;
            rl_ach_presence(cur_mode == 1 ? "In 3D Arena / Studio Show" : "Menus / Cutscene");
        }
    }
    hle_kernel_update_ticks();
    if (hle_kernel_is_shutting_down()) throw GuestPowerOff{};
    hle_ret_u32(ctx, 0u, 4);
}

void hle_D3D_CreatePalette2(CpuContext& ctx) {
    // 0x00239C40 (ret 8): (Type, Unused?)
    uint32_t p = xbox_guest_alloc(0x410u, 16u, true);
    wr32(p + 0, 1u);
    hle_ret_u32(ctx, p, 8);
}

void hle_D3D_StaticInit(CpuContext& ctx) {
    wr32(0x0037B06Cu, rd32(0x00248B10u));
    wr32(0x0037B070u, rd32(0x00248B0Cu));
    wr32(0x0037B074u, rd32(0x00248B08u));
    wr32(0x0037B078u, rd32(0x00248B04u));
    hle_ret(ctx, 0);
}

void release_standalone_surface_unlocked(uint32_t surf_va) {
    auto sit = g_surfaces.find(surf_va);
    if (sit == g_surfaces.end()) return;
    GuestSurface surf = sit->second;
    g_surfaces.erase(sit);
    if (g_cur_rt_va == surf_va) g_cur_rt_va = g_back_surf_va;
    if (g_cur_ds_va == surf_va) g_cur_ds_va = g_depth_surf_va;
    if (surf.owns_gpu && surf.gpu) gfx_retire_texture(surf.gpu);
    if (surf.data_va) xbox_guest_free(surf.data_va);
    xbox_guest_free(surf_va);
}

void hle_D3D_ReleaseVS(CpuContext& ctx) {
    // 0x0023BC00: handle is obj_va | 1
    uint32_t h = hle_arg(ctx, 0);
    uint32_t rem = 0;
    if (h) {
        std::lock_guard<std::mutex> lk(g_d3d_mtx);
        uint32_t obj_va = (h & 1u) ? (h - 1u) : h;
        uint32_t ref = rd32(obj_va);
        if (ref > 1u) {
            rem = ref - 1u;
            wr32(obj_va, rem);
        } else {
            wr32(obj_va, 0u);
            auto vit = g_vshaders.find(h);
            if (vit != g_vshaders.end()) {
                uint32_t ova = vit->second.obj_va;
                g_vshaders.erase(vit);
                if (g_cur_vs_handle == h) g_cur_vs_handle = 0;
                if (ova) xbox_guest_free(ova);
            }
        }
    }
    hle_ret_u32(ctx, rem, 4);
}

void hle_D3D_ReleasePS(CpuContext& ctx) {
    // 0x0023E5C0
    uint32_t h = hle_arg(ctx, 0);
    uint32_t rem = 0;
    if (h) {
        std::lock_guard<std::mutex> lk(g_d3d_mtx);
        uint32_t ref = rd32(h);
        if (ref > 1u) {
            rem = ref - 1u;
            wr32(h, rem);
        } else {
            wr32(h, 0u);
            auto pit = g_pshaders.find(h);
            if (pit != g_pshaders.end()) {
                g_pshaders.erase(pit);
                if (g_cur_ps_handle == h) g_cur_ps_handle = 0;
                xbox_guest_free(h);
            }
        }
    }
    hle_ret_u32(ctx, rem, 4);
}

void hle_D3D_ReleaseResource(CpuContext& ctx) {
    // 0x0023C760: D3DResource::Release (VertexBuffer, IndexBuffer, Texture, Surface, Palette)
    uint32_t va = hle_arg(ctx, 0);
    uint32_t rem = 0;
    if (!va) { hle_ret_u32(ctx, 0u, 4); return; }
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    if (va == g_front_surf_va || va == g_back_surf_va || va == g_depth_surf_va) {
        hle_ret_u32(ctx, 1u, 4);
        return;
    }
    uint32_t common = rd32(va);
    uint32_t ref = common & 0xFFFFu;
    if (ref > 1u) {
        rem = ref - 1u;
        wr32(va, (common & ~0xFFFFu) | rem);
        hle_ret_u32(ctx, rem, 4);
        return;
    }
    wr32(va, common & ~0xFFFFu);

    auto vbit = g_vbs.find(va);
    if (vbit != g_vbs.end()) {
        uint32_t data_va = vbit->second.data_va;
        g_vbs.erase(vbit);
        for (int s = 0; s < 4; ++s) {
            if (g_vb_stream_obj[s] == va) g_vb_stream_obj[s] = 0;
        }
        gfx_retire_buffer(va);
        if (data_va) xbox_guest_free(data_va);
        xbox_guest_free(va);
        hle_ret_u32(ctx, 0u, 4);
        return;
    }
    auto ibit = g_ibs.find(va);
    if (ibit != g_ibs.end()) {
        uint32_t data_va = ibit->second.data_va;
        g_ibs.erase(ibit);
        if (g_cur_ib_va == va) g_cur_ib_va = 0;
        gfx_retire_buffer(va);
        if (data_va) xbox_guest_free(data_va);
        xbox_guest_free(va);
        hle_ret_u32(ctx, 0u, 4);
        return;
    }
    auto tit = g_textures.find(va);
    if (tit != g_textures.end()) {
        GuestTexture gt = std::move(tit->second);
        g_textures.erase(tit);
        for (int s = 0; s < 4; ++s) {
            if (g_bound_tex_va[s] == va) g_bound_tex_va[s] = 0;
        }
        for (uint32_t sva : gt.level_surf_vas) {
            if (g_cur_rt_va == sva) g_cur_rt_va = g_back_surf_va;
            if (g_cur_ds_va == sva) g_cur_ds_va = g_depth_surf_va;
            g_surfaces.erase(sva);
            xbox_guest_free(sva);
        }
        uint32_t data_va = gt.gpu ? gt.gpu->data_va : 0;
        if (gt.gpu) gfx_retire_texture(gt.gpu);
        if (data_va) xbox_guest_free(data_va);
        xbox_guest_free(va);
        hle_ret_u32(ctx, 0u, 4);
        return;
    }
    auto sit = g_surfaces.find(va);
    if (sit != g_surfaces.end()) {
        if (sit->second.parent_tex_va == 0) {
            release_standalone_surface_unlocked(va);
        }
        hle_ret_u32(ctx, 0u, 4);
        return;
    }
    auto pit = g_palettes.find(va);
    if (pit != g_palettes.end()) {
        g_palettes.erase(pit);
        if (g_cur_palette_va == va) g_cur_palette_va = 0;
        xbox_guest_free(va);
    }
    hle_ret_u32(ctx, 0u, 4);
}

// ---------- XGRPH (Texture Swizzle / Compress / Header) ----------
static uint32_t spread_bits(uint32_t v, uint32_t mask) {
    // Helper for Morton encoding
    uint32_t out = 0;
    for (uint32_t bit = 0; mask; mask &= mask - 1, ++bit) {
        if ((v >> bit) & 1u) out |= mask & (~mask + 1u);
    }
    return out;
}

static uint32_t morton_mask_x(uint32_t w, uint32_t h) {
    uint32_t m = 0, bit = 1;
    uint32_t tw = w, th = h;
    while (tw > 1 || th > 1) {
        if (tw > 1) { m |= bit; bit <<= 1; tw >>= 1; }
        if (th > 1) { bit <<= 1; th >>= 1; }
    }
    return m;
}

void hle_XGSwizzleRect(CpuContext& ctx) {
    // 0x00256F77: (pSource, Pitch, pRect, pDest, Width, Height, pPoint, BytesPerPixel) -> ret 32
    uint32_t src_va = hle_arg(ctx, 0);
    uint32_t pitch  = hle_arg(ctx, 1);
    uint32_t prect  = hle_arg(ctx, 2);
    uint32_t dst_va = hle_arg(ctx, 3);
    uint32_t w      = hle_arg(ctx, 4);
    uint32_t h      = hle_arg(ctx, 5);
    uint32_t ppt    = hle_arg(ctx, 6);
    uint32_t bpp    = hle_arg(ctx, 7);

    if (src_va && dst_va && w && h && bpp) {
        uint32_t sx0 = 0, sy0 = 0, rw = w, rh = h;
        if (prect) {
            sx0 = rd32(prect + 0); sy0 = rd32(prect + 4);
            uint32_t sx1 = rd32(prect + 8), sy1 = rd32(prect + 12);
            if (sx1 > sx0) rw = sx1 - sx0;
            if (sy1 > sy0) rh = sy1 - sy0;
        }
        uint32_t dx0 = ppt ? rd32(ppt + 0) : 0;
        uint32_t dy0 = ppt ? rd32(ppt + 4) : 0;
        if (!pitch) pitch = w * bpp;

        uint32_t mx = morton_mask_x(w, h);
        uint32_t my = (w * h - 1u) ^ mx;
        const uint8_t* src = mem_ptr(src_va);
        uint8_t* dst = mem_ptr(dst_va);
        for (uint32_t y = 0; y < rh && (dy0 + y) < h; ++y) {
            uint32_t sy_off = (sy0 + y) * pitch;
            uint32_t ym = spread_bits(dy0 + y, my);
            for (uint32_t x = 0; x < rw && (dx0 + x) < w; ++x) {
                uint32_t xm = spread_bits(dx0 + x, mx);
                std::memcpy(dst + (ym | xm) * bpp, src + sy_off + (sx0 + x) * bpp, bpp);
            }
        }
    }
    hle_ret(ctx, 32);
}

static uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void extract_rgba_from_xb(const uint8_t* p, uint32_t fmt, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    if (fmt == 0x06u || fmt == 0x12u || fmt == 0x07u || fmt == 0x1Eu) { // ARGB / XRGB
        b = p[0]; g = p[1]; r = p[2]; a = (fmt == 0x07u || fmt == 0x1Eu) ? 255 : p[3];
    } else if (fmt == 0x05u || fmt == 0x11u) { // R5G6B5
        uint16_t v = p[0] | (uint16_t(p[1]) << 8);
        r = static_cast<uint8_t>(((v >> 11) & 31) * 255 / 31);
        g = static_cast<uint8_t>(((v >> 5) & 63) * 255 / 63);
        b = static_cast<uint8_t>((v & 31) * 255 / 31);
        a = 255;
    } else {
        b = p[0]; g = p[1]; r = p[2]; a = p[3];
    }
}

void hle_XGCompressRect(CpuContext& ctx) {
    // 0x002583E3: (pDest, DstFmt, DstPitch, W, H, pSrc, SrcFmt, SrcPitch, AlphaRef, Flags) -> ret 40
    uint32_t dst_va  = hle_arg(ctx, 0);
    uint32_t dst_fmt = hle_arg(ctx, 1);
    uint32_t dst_p   = hle_arg(ctx, 2);
    uint32_t w       = hle_arg(ctx, 3);
    uint32_t h       = hle_arg(ctx, 4);
    uint32_t src_va  = hle_arg(ctx, 5);
    uint32_t src_fmt = hle_arg(ctx, 6);
    uint32_t src_p   = hle_arg(ctx, 7);

    if (dst_va && src_va && w && h) {
        uint32_t sbpp = bits_per_pixel_xb(src_fmt) / 8u;
        if (!sbpp) sbpp = 4;
        if (!src_p) src_p = w * sbpp;
        const uint8_t* src = mem_ptr(src_va);
        uint8_t* dst = mem_ptr(dst_va);

        if (dst_fmt == 0x0Cu || dst_fmt == 0x0Eu || dst_fmt == 0x0Fu) {
            uint32_t bw = std::max(1u, (w + 3u) / 4u);
            uint32_t bh = std::max(1u, (h + 3u) / 4u);
            uint32_t blk_sz = (dst_fmt == 0x0Cu) ? 8u : 16u;
            uint32_t dp = dst_p ? dst_p : (bw * blk_sz);
            for (uint32_t by = 0; by < bh; ++by) {
                for (uint32_t bx = 0; bx < bw; ++bx) {
                    uint8_t rmin = 255, gmin = 255, bmin = 255;
                    uint8_t rmax = 0, gmax = 0, bmax = 0;
                    uint8_t px[16][4]{};
                    for (uint32_t py = 0; py < 4; ++py) {
                        uint32_t sy = std::min(by * 4 + py, h - 1);
                        for (uint32_t px_x = 0; px_x < 4; ++px_x) {
                            uint32_t sx = std::min(bx * 4 + px_x, w - 1);
                            uint8_t r, g, b, a;
                            extract_rgba_from_xb(src + sy * src_p + sx * sbpp, src_fmt, r, g, b, a);
                            px[py * 4 + px_x][0] = r; px[py * 4 + px_x][1] = g;
                            px[py * 4 + px_x][2] = b; px[py * 4 + px_x][3] = a;
                            rmin = std::min(rmin, r); gmin = std::min(gmin, g); bmin = std::min(bmin, b);
                            rmax = std::max(rmax, r); gmax = std::max(gmax, g); bmax = std::max(bmax, b);
                        }
                    }
                    uint8_t* blk = dst + by * dp + bx * blk_sz;
                    if (dst_fmt != 0x0Cu) {
                        // Explicit alpha (DXT3) or interpolated (DXT5 fallback)
                        for (int i = 0; i < 8; ++i) {
                            blk[i] = static_cast<uint8_t>(((px[i * 2][3] >> 4) & 0xF) | ((px[i * 2 + 1][3] >> 4) << 4));
                        }
                        if (dst_fmt == 0x0Fu) {
                            blk[0] = 255; blk[1] = 255;
                            std::memset(blk + 2, 0, 6);
                        }
                        blk += 8;
                    }
                    uint16_t c0 = pack_rgb565(rmax, gmax, bmax);
                    uint16_t c1 = pack_rgb565(rmin, gmin, bmin);
                    if (c0 < c1) std::swap(c0, c1);
                    if (c0 == c1 && dst_fmt == 0x0Cu) { if (c0 > 0) --c1; else ++c0; }
                    std::memcpy(blk + 0, &c0, 2);
                    std::memcpy(blk + 2, &c1, 2);
                    uint32_t bits = 0;
                    int lmax = int(rmax) + int(gmax) * 2 + int(bmax);
                    int lmin = int(rmin) + int(gmin) * 2 + int(bmin);
                    int span = std::max(1, lmax - lmin);
                    for (int i = 0; i < 16; ++i) {
                        int lum = int(px[i][0]) + int(px[i][1]) * 2 + int(px[i][2]);
                        int t = ((lum - lmin) * 3 + span / 2) / span;
                        uint32_t code = (t == 3) ? 0u : (t == 2) ? 2u : (t == 1) ? 3u : 1u;
                        bits |= (code << (i * 2));
                    }
                    std::memcpy(blk + 4, &bits, 4);
                }
            }
        } else {
            uint32_t copy_b = std::min(src_p, dst_p ? dst_p : src_p);
            for (uint32_t y = 0; y < h; ++y) {
                std::memcpy(dst + y * (dst_p ? dst_p : src_p), src + y * src_p, copy_b);
            }
        }
    }
    hle_ret_u32(ctx, 0u, 40);
}

void hle_XGSetTextureHeader(CpuContext& ctx) {
    // 0x00258F41: Disassembly confirmed XGSetSurfaceHeader(Width, Height, Format, pSurface, DataOffset, Pitch) -> ret 24
    std::lock_guard<std::mutex> lk(g_d3d_mtx);
    uint32_t w        = std::max(1u, hle_arg(ctx, 0));
    uint32_t h        = std::max(1u, hle_arg(ctx, 1));
    uint32_t fmt      = hle_arg(ctx, 2);
    uint32_t surf_va  = hle_arg(ctx, 3);
    uint32_t data_off = hle_arg(ctx, 4);
    uint32_t pitch    = hle_arg(ctx, 5);
    if (!pitch) pitch = calc_pitch_xb(w, fmt);

    if (surf_va) {
        write_surface_struct(surf_va, data_off, fmt, w, h, pitch, 5u);
        auto sit = g_surfaces.find(surf_va);
        if (sit == g_surfaces.end()) {
            auto* gpu = new GpuTextureObj();
            gpu->guest_va = surf_va;
            gpu->width = w; gpu->height = h; gpu->levels = 1u;
            gpu->xb_format = fmt; gpu->pitch = pitch; gpu->data_va = data_off;
            gpu->is_render_target = true;
            gpu->dirty = (data_off != 0);
            g_surfaces[surf_va] = {surf_va, 0, gpu, w, h, fmt, pitch, data_off, 0, true, (data_off != 0), 0};
        } else {
            sit->second.width = w;
            sit->second.height = h;
            sit->second.xb_format = fmt;
            sit->second.pitch = pitch;
            sit->second.data_va = data_off;
            if (sit->second.gpu) {
                sit->second.gpu->width = w;
                sit->second.gpu->height = h;
                sit->second.gpu->xb_format = fmt;
                sit->second.gpu->pitch = pitch;
                sit->second.gpu->data_va = data_off;
                sit->second.gpu->is_render_target = true;
                sit->second.gpu->dirty = (data_off != 0);
            }
        }
    }
    hle_ret(ctx, 24);
}

} // namespace

void hle_register_d3d8() {
    recomp_register_fn(0x000A0D10u, draw_frontend_screen);
    struct Entry { uint32_t va; RecompFn fn; };
    static const Entry table[] = {
        {0x00235100u, hle_D3D_SetLighting},
        {0x00235130u, hle_D3D_FastMethod},
        {0x00235940u, hle_RS_97_Specular},
        {0x00235980u, hle_RS_9C_FogColor},
        {0x002359C0u, hle_RS_8A_TexFactor},
        {0x00235A10u, hle_RS_93_FillCullEn},
        {0x00235A80u, hle_RS_92_FrontFace},
        {0x00235AC0u, hle_RS_8E_CullMode},
        {0x00235B00u, hle_RS_94_PSFlags},
        {0x00235B60u, hle_RS_9D_FogDensity},
        {0x00235BD0u, hle_RS_9F_YUV},
        {0x00235C50u, hle_RS_95_Wrap},
        {0x00235CD0u, hle_RS_96_TexGen},
        {0x00235D30u, hle_RS_8B_PolyBias},
        {0x00235D80u, hle_RS_8C_ZBias},
        {0x00235DE0u, hle_RS_8D_PolyOffset},
        {0x00235E50u, hle_RS_89_StencilEnable},
        {0x00235EA0u, hle_TSS_Stage28},
        {0x00235FB0u, hle_TSS_Generic},
        {0x00236020u, hle_TSS_Stage29},
        {0x00236060u, hle_TSS_Stage30},
        {0x00236BA0u, hle_RS_8F_ZEnable},
        {0x00236C30u, hle_RS_90_FogEnable},
        {0x00236CC0u, hle_RS_91_FogTableMode},
        {0x00236D30u, hle_RS_A0_FogStart},
        {0x00236D60u, hle_RS_A1_FogEnd},
        {0x00236DD0u, hle_RS_A2_RangeFog},
        {0x00236E40u, hle_RS_A3_Shadow},
        {0x00236E60u, hle_RS_A4_ShadowA},
        {0x00236E80u, hle_RS_A5_ShadowB},
        {0x00236EA0u, hle_RS_9A_EdgeAA},
        {0x00236EE0u, hle_RS_9B_SampleAlpha},
        {0x00236F20u, hle_RS_98_LineWidth},
        {0x00236FA0u, hle_RS_99_Antialias},
        {0x00236FF0u, hle_RS_9E_Multisample},
        {0x002370B0u, hle_D3D_SetRenderTarget},
        {0x002370D0u, hle_D3D_StubRet8},
        {0x002371F0u, hle_D3D_SetTransform},
        {0x002373B0u, hle_D3D_SetMaterial},
        {0x00237570u, hle_D3D_StubRet0},
        {0x002375E0u, hle_D3D_GetVisibilityTestResult},
        {0x00237640u, hle_D3D_StubRet4},
        {0x00237680u, hle_D3D_BlockUntilVerticalBlank},
        {0x002377F0u, hle_D3D_StubRet0},
        {0x00237E80u, hle_D3D_StubRet4_0},
        {0x00237F30u, hle_D3D_GetBackBuffer},
        {0x00238340u, hle_D3D_CopyRects},
        {0x00238610u, hle_D3D_GetRenderTarget2},
        {0x00238630u, hle_D3D_GetDepthStencilSurface2},
        {0x00238650u, hle_D3D_SetViewport},
        {0x002387B0u, hle_D3D_SetLight},
        {0x00238990u, hle_D3D_LightEnable},
        {0x00238AB0u, hle_D3D_SetTexture},
        {0x00238D20u, hle_D3D_SetIndices},
        {0x00238E30u, hle_D3D_StubRet0},
        {0x00238ED0u, hle_D3D_EndVisibilityTest},
        {0x00238F30u, hle_D3D_GetDisplayFieldStatus},
        {0x00239840u, hle_D3D_StubRet8},
        {0x00239A60u, hle_D3D_CreateTexture2},
        {0x00239A00u, hle_D3D_StaticInit},
        {0x00239B40u, hle_D3D_GetSurfaceLevel2},
        {0x00239BD0u, hle_D3D_GetCubeMapSurface2},
        {0x00239C90u, hle_D3D_TextureLockRect_Vol},
        {0x00239D10u, hle_D3D_Clear},
        {0x0023A3F0u, hle_D3D_DrawVerticesUP},
        {0x0023A550u, hle_D3D_DrawVertices},
        {0x0023A730u, hle_D3D_DrawIndexedVertices},
        {0x0023B200u, hle_D3D_CreateVertexShader},
        {0x0023B360u, hle_D3D_SetVSConstant1},
        {0x0023B410u, hle_D3D_SetVSConstant4},
        {0x0023B5A0u, hle_D3D_SetVSConstantN},
        {0x0023B7F0u, hle_D3D_SetStreamSource},
        {0x0023BC00u, hle_D3D_ReleaseVS},
        {0x0023BD20u, hle_D3D_SetVertexShader},
        {0x0023C760u, hle_D3D_ReleaseResource},
        {0x0023CBE0u, hle_D3D_CreateImageSurface},
        {0x0023CD40u, hle_D3D_SurfaceLockRect},
        {0x00239C40u, hle_D3D_CreatePalette2},
        {0x0023CDB0u, hle_D3D_SurfaceGetDesc},
        {0x0023CE10u, hle_D3D_StubRet4_1},
        {0x0023CE20u, hle_D3D_GetDeviceCaps},
        {0x0023CE50u, hle_D3D_GetAdapterModeCount},
        {0x0023CEF0u, hle_D3D_EnumAdapterModes},
        {0x0023D050u, hle_D3D_CheckDeviceMultiSampleType},
        {0x0023D320u, hle_D3D_StubRet8},
        {0x0023D340u, hle_D3D_CreateDevice},
        {0x0023D710u, hle_D3D_GetSuperSampleScale},
        {0x0023E090u, hle_D3D_Swap},
        {0x0023E350u, hle_D3D_CreateIndexBuffer2},
        {0x0023E400u, hle_D3D_CreateVertexBuffer2},
        {0x0023E450u, hle_D3D_VertexBufferLock},
        {0x0023E570u, hle_D3D_CreatePixelShader},
        {0x0023E5C0u, hle_D3D_ReleasePS},
        {0x0023EA20u, hle_D3D_SetPixelShader},
        {0x0023EC20u, hle_D3D_SetPixelShaderConstant},
        // XGRPH
        {0x00256F77u, hle_XGSwizzleRect},
        {0x002583E3u, hle_XGCompressRect},
        {0x00258F41u, hle_XGSetTextureHeader},
    };
    for (const auto& e : table) recomp_register_fn(e.va, e.fn);
}
