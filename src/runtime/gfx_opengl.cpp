#include "gfx_backend.h"
#include "nv2a_shaders.h"
#include "tex_decode.h"
#include "gl_loader.h"
#include "launcher_ui.h"
#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

SDL_Window* g_sdl_win = nullptr;
SDL_GLContext g_gl_ctx = nullptr;
RendererSettings g_cfg{};
std::atomic<int> g_pending_resize_w{0}, g_pending_resize_h{0};
std::thread::id g_gl_owner_tid{};
std::mutex g_gl_owner_mtx;

void ensure_gl_current() {
    if (!g_gl_ctx || !g_sdl_win) return;
    auto tid = std::this_thread::get_id();
    if (g_gl_owner_tid == tid) return;
    std::lock_guard<std::mutex> lk(g_gl_owner_mtx);
    if (g_gl_owner_tid != tid) {
        SDL_GL_MakeCurrent(g_sdl_win, g_gl_ctx);
        g_gl_owner_tid = tid;
    }
}

GLuint g_vao = 0;
GLuint g_dyn_vb = 0;
GLuint g_dyn_ib = 0;
GLuint g_ubo_vs_nv2a = 0;
GLuint g_ubo_vs_scr  = 0;
GLuint g_ubo_ff_vs   = 0;
GLuint g_ubo_ps      = 0;

struct CachedGLBuf {
    GLuint buf = 0;
    uint32_t bytes = 0;
    uint32_t seq = 0;
};
std::unordered_map<uint32_t, CachedGLBuf> g_static_vbs;

GLuint g_msaa_fbo = 0, g_msaa_color_rbo = 0, g_msaa_ds_rbo = 0;
GLuint g_resolve_fbo = 0, g_resolve_tex = 0;
int g_fb_samples = 1;

GLuint g_white_tex = 0;
GLuint g_bink_tex = 0;
int g_bink_w = 0, g_bink_h = 0;

GLuint g_quad_vao = 0, g_quad_vbo = 0;
GLuint g_quad_prog_blit = 0, g_quad_prog_post = 0;

std::unordered_map<uint64_t, GLuint> g_prog_cache;
std::unordered_map<uint64_t, GLuint> g_samp_cache;

std::vector<uint16_t> g_scratch_tris;
std::vector<uint32_t> g_scratch_rgba;

std::string g_pending_screenshot;
int g_frame_count = 0;
int g_draw_calls = 0;
int g_last_draw_calls = 0;
int g_clear_calls = 0;
int g_draw_skipped_shader = 0;
int g_draw_skipped_vert = 0;
double g_fps = 60.0;
auto g_fps_t0 = std::chrono::steady_clock::now();
auto g_frame_t0 = std::chrono::steady_clock::now();
int g_fps_frames = 0;

uint64_t hash_pair(uint64_t a, uint64_t b) {
    uint64_t h = 0xcbf29ce484222325ull ^ a;
    h *= 0x100000001b3ull;
    h ^= b;
    return h * 0x100000001b3ull;
}

GLenum gl_cmp(uint32_t c) {
    switch (c) {
        case 1: return GL_NEVER;
        case 2: return GL_LESS;
        case 3: return GL_EQUAL;
        case 4: return GL_LEQUAL;
        case 5: return GL_GREATER;
        case 6: return GL_NOTEQUAL;
        case 7: return GL_GEQUAL;
        default: return GL_ALWAYS;
    }
}

GLenum gl_blend(uint32_t b, bool alpha_ch) {
    switch (b) {
        case 1: return GL_ZERO;
        case 2: return GL_ONE;
        case 3: return alpha_ch ? GL_SRC_ALPHA : GL_SRC_COLOR;
        case 4: return alpha_ch ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE_MINUS_SRC_COLOR;
        case 5: return GL_SRC_ALPHA;
        case 6: return GL_ONE_MINUS_SRC_ALPHA;
        case 7: return GL_DST_ALPHA;
        case 8: return GL_ONE_MINUS_DST_ALPHA;
        case 9: return alpha_ch ? GL_DST_ALPHA : GL_DST_COLOR;
        case 10: return alpha_ch ? GL_ONE_MINUS_DST_ALPHA : GL_ONE_MINUS_DST_COLOR;
        case 11: return GL_SRC_ALPHA_SATURATE;
        default: return GL_ONE;
    }
}

GLenum gl_stencil_op(uint32_t op) {
    switch (op) {
        case 1: case 0x1E00u: return GL_KEEP;
        case 2: case 0u:      return GL_ZERO;
        case 3: case 0x1E01u: return GL_REPLACE;
        case 4: case 0x1E02u: return GL_INCR;
        case 5: case 0x1E03u: return GL_DECR;
        case 6: case 0x150Au: return GL_INVERT;
        case 7: case 0x8507u: return GL_INCR_WRAP;
        case 8: case 0x8508u: return GL_DECR_WRAP;
        default: return GL_KEEP;
    }
}

GLint gl_wrap(uint32_t a) {
    switch (a) {
        case 2: return GL_MIRRORED_REPEAT;
        case 3: case 4: return GL_CLAMP_TO_EDGE;
        default: return GL_REPEAT;
    }
}

GLuint get_sampler(uint32_t au, uint32_t av, uint32_t minf, uint32_t magf) {
    auto& gl = whacked_gl();
    uint64_t k = uint64_t(au) | (uint64_t(av) << 4) | (uint64_t(minf) << 8) | (uint64_t(magf) << 12) | (g_cfg.aniso_16x ? 0x10000ull : 0ull);
    auto it = g_samp_cache.find(k);
    if (it != g_samp_cache.end()) return it->second;

    GLuint s = 0;
    gl.glGenSamplers(1, &s);
    gl.glSamplerParameteri(s, GL_TEXTURE_WRAP_S, gl_wrap(au));
    gl.glSamplerParameteri(s, GL_TEXTURE_WRAP_T, gl_wrap(av));
    gl.glSamplerParameteri(s, GL_TEXTURE_WRAP_R, GL_REPEAT);
    bool point = (minf <= 1 && magf <= 1);
    gl.glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, point ? GL_NEAREST : GL_LINEAR_MIPMAP_LINEAR);
    gl.glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, point ? GL_NEAREST : GL_LINEAR);
    if (g_cfg.aniso_16x && !point && gl.has_aniso) {
        GLfloat maxa = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxa);
        gl.glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY_EXT, std::min(16.0f, maxa));
    }
    g_samp_cache[k] = s;
    return s;
}

void release_targets() {
    auto& gl = whacked_gl();
    if (g_msaa_fbo) { gl.glDeleteFramebuffers(1, &g_msaa_fbo); g_msaa_fbo = 0; }
    if (g_msaa_color_rbo) { gl.glDeleteRenderbuffers(1, &g_msaa_color_rbo); g_msaa_color_rbo = 0; }
    if (g_msaa_ds_rbo) { gl.glDeleteRenderbuffers(1, &g_msaa_ds_rbo); g_msaa_ds_rbo = 0; }
    if (g_resolve_fbo) { gl.glDeleteFramebuffers(1, &g_resolve_fbo); g_resolve_fbo = 0; }
    if (g_resolve_tex) { glDeleteTextures(1, &g_resolve_tex); g_resolve_tex = 0; }
}

bool create_targets(int w, int h) {
    auto& gl = whacked_gl();
    release_targets();
    g_cfg.width = std::max(64, w);
    g_cfg.height = std::max(64, h);

    GLint max_s = 1;
    glGetIntegerv(GL_MAX_SAMPLES, &max_s);
    g_fb_samples = g_cfg.msaa_4x ? std::min<int>(4, std::max<int>(1, max_s)) : 1;

    gl.glGenFramebuffers(1, &g_msaa_fbo);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, g_msaa_fbo);

    gl.glGenRenderbuffers(1, &g_msaa_color_rbo);
    gl.glBindRenderbuffer(GL_RENDERBUFFER, g_msaa_color_rbo);
    if (g_fb_samples > 1) gl.glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_fb_samples, GL_RGBA8, g_cfg.width, g_cfg.height);
    else gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, g_cfg.width, g_cfg.height);
    gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_msaa_color_rbo);

    gl.glGenRenderbuffers(1, &g_msaa_ds_rbo);
    gl.glBindRenderbuffer(GL_RENDERBUFFER, g_msaa_ds_rbo);
    if (g_fb_samples > 1) gl.glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_fb_samples, GL_DEPTH24_STENCIL8, g_cfg.width, g_cfg.height);
    else gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, g_cfg.width, g_cfg.height);
    gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_msaa_ds_rbo);

    gl.glGenFramebuffers(1, &g_resolve_fbo);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, g_resolve_fbo);
    glGenTextures(1, &g_resolve_tex);
    glBindTexture(GL_TEXTURE_2D, g_resolve_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_cfg.width, g_cfg.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_resolve_tex, 0);

    gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

GLuint compile_raw_prog(const char* vsrc, const char* fsrc) {
    auto& gl = whacked_gl();
    auto comp = [&](GLenum t, const char* s) -> GLuint {
        GLuint sh = gl.glCreateShader(t);
        gl.glShaderSource(sh, 1, &s, nullptr);
        gl.glCompileShader(sh);
        GLint ok = 0; gl.glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024]{}; gl.glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
            std::fprintf(stderr, "[GL QUAD SHADER ERR]: %s\n", log);
        }
        return sh;
    };
    GLuint vs = comp(GL_VERTEX_SHADER, vsrc);
    GLuint fs = comp(GL_FRAGMENT_SHADER, fsrc);
    GLuint p = gl.glCreateProgram();
    gl.glAttachShader(p, vs);
    gl.glAttachShader(p, fs);
    gl.glLinkProgram(p);
    gl.glDeleteShader(vs);
    gl.glDeleteShader(fs);
    return p;
}

void init_quad_and_white() {
    auto& gl = whacked_gl();
    const char* qvs = R"(#version 330 core
        layout(location=0) in vec2 pos;
        layout(location=1) in vec2 uv;
        layout(location=2) in vec4 col;
        out vec2 v_uv; out vec4 v_col;
        void main() { gl_Position = vec4(pos, 0.0, 1.0); v_uv = uv; v_col = col; }
    )";
    const char* qps = R"(#version 330 core
        uniform sampler2D tx;
        in vec2 v_uv; in vec4 v_col;
        layout(location=0) out vec4 FragColor;
        void main() { FragColor = texture(tx, v_uv) * v_col; }
    )";
    const char* pps = R"(#version 330 core
        uniform sampler2D tx;
        in vec2 v_uv; in vec4 v_col;
        layout(location=0) out vec4 FragColor;
        void main() {
            vec4 c = texture(tx, v_uv);
            vec3 bloom = max(c.rgb - 0.85, 0.0) * 0.25;
            FragColor = vec4(clamp(c.rgb + bloom, 0.0, 1.0), 1.0);
        }
    )";
    g_quad_prog_blit = compile_raw_prog(qvs, qps);
    g_quad_prog_post = compile_raw_prog(qvs, pps);
    gl.glUseProgram(g_quad_prog_blit);
    gl.glUniform1i(gl.glGetUniformLocation(g_quad_prog_blit, "tx"), 0);
    gl.glUseProgram(g_quad_prog_post);
    gl.glUniform1i(gl.glGetUniformLocation(g_quad_prog_post, "tx"), 0);
    gl.glUseProgram(0);

    gl.glGenVertexArrays(1, &g_quad_vao);
    gl.glGenBuffers(1, &g_quad_vbo);
    gl.glBindVertexArray(g_quad_vao);
    gl.glBindBuffer(GL_ARRAY_BUFFER, g_quad_vbo);
    gl.glBufferData(GL_ARRAY_BUFFER, 4096, nullptr, GL_STREAM_DRAW);
    gl.glEnableVertexAttribArray(0);
    gl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 20, reinterpret_cast<void*>(0));
    gl.glEnableVertexAttribArray(1);
    gl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, reinterpret_cast<void*>(8));
    gl.glEnableVertexAttribArray(2);
    gl.glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 20, reinterpret_cast<void*>(16));
    gl.glBindVertexArray(0);

    uint32_t white = 0xFFFFFFFFu;
    glGenTextures(1, &g_white_tex);
    glBindTexture(GL_TEXTURE_2D, g_white_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
}

GLuint get_linked_program(const Nv2aCompiledVS* vs, const Nv2aCompiledPS* ps) {
    auto& gl = whacked_gl();
    uint64_t k = hash_pair(vs->key, ps->key);
    auto it = g_prog_cache.find(k);
    if (it != g_prog_cache.end()) return it->second;

    GLuint prog = gl.glCreateProgram();
    gl.glAttachShader(prog, vs->gl_vs);
    gl.glAttachShader(prog, ps->gl_ps);
    gl.glLinkProgram(prog);
    GLint ok = 0;
    gl.glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]{};
        gl.glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[GL LINK ERR]: %s\n", log);
        gl.glDeleteProgram(prog);
        g_prog_cache[k] = 0;
        return 0;
    }

    auto bind_ubo = [&](const char* name, GLuint binding) {
        GLuint idx = gl.glGetUniformBlockIndex(prog, name);
        if (idx != GL_INVALID_INDEX) gl.glUniformBlockBinding(prog, idx, binding);
    };
    bind_ubo("NV2AVSConsts", 0);
    bind_ubo("FFFrameCB",    0);
    bind_ubo("NV2AScreenCB", 1);
    bind_ubo("NV2APSConsts", 2);
    bind_ubo("FFPSCB",       2);

    gl.glUseProgram(prog);
    for (int s = 0; s < 4; ++s) {
        char nm[8];
        std::snprintf(nm, sizeof(nm), "tex%d", s);
        GLint loc = gl.glGetUniformLocation(prog, nm);
        if (loc >= 0) gl.glUniform1i(loc, s);
    }
    gl.glUseProgram(0);
    g_prog_cache[k] = prog;
    return prog;
}

void ensure_gl_rt(GpuTextureObj* t) {
    auto& gl = whacked_gl();
    if (!t || (t->gl_tex && (!t->is_render_target || t->gl_fbo))) return;
    if (t->width == 0 || t->height == 0) return;
    if (!t->gl_tex) {
        glGenTextures(1, &t->gl_tex);
        t->gl_is_cube = t->is_cube;
        GLenum target = t->is_cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
        glBindTexture(target, t->gl_tex);
        if (t->is_cube) {
            for (int f = 0; f < 6; ++f) {
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, t->width, t->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            }
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->width, t->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        }
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(target, 0);
    }
    if (t->is_render_target && !t->gl_fbo && !t->is_cube) {
        gl.glGenFramebuffers(1, &t->gl_fbo);
        gl.glBindFramebuffer(GL_FRAMEBUFFER, t->gl_fbo);
        gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->gl_tex, 0);
        gl.glGenRenderbuffers(1, &t->gl_rbo);
        gl.glBindRenderbuffer(GL_RENDERBUFFER, t->gl_rbo);
        gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, t->width, t->height);
        gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->gl_rbo);
        gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    t->dirty = false;
}

void save_gl_backbuffer_png(const std::string& path) {
    int w = g_cfg.width, h = g_cfg.height;
    std::vector<uint8_t> raw(size_t(w) * h * 4u);
    std::vector<uint8_t> flipped(size_t(w) * h * 4u);
    whacked_gl().glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = raw.data() + size_t(h - 1 - y) * w * 4u;
        uint8_t* dst = flipped.data() + size_t(y) * w * 4u;
        std::memcpy(dst, src, size_t(w) * 4u);
        for (int x = 0; x < w; ++x) dst[x * 4 + 3] = 255;
    }
    if (gfx_save_png_rgba(path, w, h, flipped.data(), w * 4)) {
        std::printf("[GFX-GL] Saved PNG screenshot: %s (%dx%d)\n", path.c_str(), w, h);
    }
}

void set_vertex_attrib_from_decl(const Nv2aVertexDecl::Element& e, uint32_t stride, size_t base_off) {
    auto& gl = whacked_gl();
    const void* ptr = reinterpret_cast<const void*>(base_off + e.offset);
    switch (e.type) {
        case 0x12: gl.glVertexAttribPointer(e.reg, 1, GL_FLOAT, GL_FALSE, stride, ptr); break;
        case 0x22: gl.glVertexAttribPointer(e.reg, 2, GL_FLOAT, GL_FALSE, stride, ptr); break;
        case 0x32: gl.glVertexAttribPointer(e.reg, 3, GL_FLOAT, GL_FALSE, stride, ptr); break;
        case 0x42: gl.glVertexAttribPointer(e.reg, 4, GL_FLOAT, GL_FALSE, stride, ptr); break;
        case 0x40: gl.glVertexAttribPointer(e.reg, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, stride, ptr); break;
        case 0x41: gl.glVertexAttribIPointer(e.reg, 4, GL_UNSIGNED_BYTE, stride, ptr); break;
        case 0x11: gl.glVertexAttribPointer(e.reg, 1, GL_SHORT, GL_FALSE, stride, ptr); break;
        case 0x21: gl.glVertexAttribPointer(e.reg, 2, GL_SHORT, GL_FALSE, stride, ptr); break;
        case 0x45: gl.glVertexAttribPointer(e.reg, 4, GL_SHORT, GL_TRUE, stride, ptr); break;
        case 0x25: gl.glVertexAttribPointer(e.reg, 2, GL_SHORT, GL_TRUE, stride, ptr); break;
        case 0x44: gl.glVertexAttribPointer(e.reg, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, ptr); break;
        case 0x34: gl.glVertexAttribPointer(e.reg, 3, GL_UNSIGNED_BYTE, GL_TRUE, stride, ptr); break;
        case 0x16: gl.glVertexAttribIPointer(e.reg, 1, GL_UNSIGNED_INT, stride, ptr); break;
        default:   gl.glVertexAttribPointer(e.reg, 4, GL_FLOAT, GL_FALSE, stride, ptr); break;
    }
}

} // namespace

bool gl_init(void* sdl_win, const RendererSettings& settings) {
    g_sdl_win = static_cast<SDL_Window*>(sdl_win);
    g_cfg = settings;

    g_gl_ctx = SDL_GL_CreateContext(g_sdl_win);
    if (!g_gl_ctx) {
        std::fprintf(stderr, "[GFX-GL] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(g_sdl_win, g_gl_ctx);
    SDL_GL_SetSwapInterval(g_cfg.vsync ? 1 : 0);

    if (!whacked_gl_load()) {
        std::fprintf(stderr, "[GFX-GL] Failed to load required OpenGL 3.3 Core functions.\n");
        return false;
    }

    auto& gl = whacked_gl();
    std::printf("[GFX-GL] OpenGL Renderer: %s | Version: %s\n",
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    create_targets(g_cfg.width, g_cfg.height);
    nv2a_shaders_init(nullptr, true, g_cfg.cache_dir);
    init_quad_and_white();

    gl.glGenVertexArrays(1, &g_vao);
    gl.glGenBuffers(1, &g_dyn_vb);
    gl.glGenBuffers(1, &g_dyn_ib);
    auto mk_ubo = [&](GLuint& ubo, GLsizeiptr sz) {
        gl.glGenBuffers(1, &ubo);
        gl.glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        gl.glBufferData(GL_UNIFORM_BUFFER, sz, nullptr, GL_DYNAMIC_DRAW);
    };
    mk_ubo(g_ubo_vs_nv2a, 192 * 16);
    mk_ubo(g_ubo_vs_scr,  32);
    mk_ubo(g_ubo_ff_vs,   640);
    mk_ubo(g_ubo_ps,      352);
    gl.glBindBuffer(GL_UNIFORM_BUFFER, 0);

    ui_set_active_backend_name("OpenGL 3.3 Core (Cross-Platform)");
    ui_init_gl3(sdl_win, g_gl_ctx);
    g_gl_owner_tid = std::this_thread::get_id();
    return true;
}

void gl_release_context() {
    if (g_sdl_win && g_gl_ctx) {
        std::lock_guard<std::mutex> lk(g_gl_owner_mtx);
        SDL_GL_MakeCurrent(g_sdl_win, nullptr);
        g_gl_owner_tid = {};
    }
}

void gl_shutdown() {
    ensure_gl_current();
    ui_shutdown();
    nv2a_shaders_shutdown();
    auto& gl = whacked_gl();
    if (gl.glDeleteProgram) {
        for (auto& kv : g_prog_cache) if (kv.second) gl.glDeleteProgram(kv.second);
        if (g_quad_prog_blit) gl.glDeleteProgram(g_quad_prog_blit);
        if (g_quad_prog_post) gl.glDeleteProgram(g_quad_prog_post);
        for (auto& kv : g_samp_cache) if (kv.second) gl.glDeleteSamplers(1, &kv.second);
        for (auto& kv : g_static_vbs) if (kv.second.buf) gl.glDeleteBuffers(1, &kv.second.buf);
        if (g_vao) gl.glDeleteVertexArrays(1, &g_vao);
        if (g_quad_vao) gl.glDeleteVertexArrays(1, &g_quad_vao);
        if (g_quad_vbo) gl.glDeleteBuffers(1, &g_quad_vbo);
        if (g_dyn_vb) gl.glDeleteBuffers(1, &g_dyn_vb);
        if (g_dyn_ib) gl.glDeleteBuffers(1, &g_dyn_ib);
        if (g_ubo_vs_nv2a) gl.glDeleteBuffers(1, &g_ubo_vs_nv2a);
        if (g_ubo_vs_scr) gl.glDeleteBuffers(1, &g_ubo_vs_scr);
        if (g_ubo_ff_vs) gl.glDeleteBuffers(1, &g_ubo_ff_vs);
        if (g_ubo_ps) gl.glDeleteBuffers(1, &g_ubo_ps);
        release_targets();
    }
    if (g_white_tex) { glDeleteTextures(1, &g_white_tex); g_white_tex = 0; }
    if (g_bink_tex) { glDeleteTextures(1, &g_bink_tex); g_bink_tex = 0; }
    g_prog_cache.clear(); g_samp_cache.clear(); g_static_vbs.clear();
    if (g_gl_ctx) { SDL_GL_DeleteContext(g_gl_ctx); g_gl_ctx = nullptr; }
    g_sdl_win = nullptr;
}

void gl_resize(int width, int height) {
    if (!g_gl_ctx || width <= 0 || height <= 0) return;
    if (g_gl_owner_tid != std::thread::id{} && g_gl_owner_tid != std::this_thread::get_id()) {
        g_pending_resize_w.store(width);
        g_pending_resize_h.store(height);
        return;
    }
    ensure_gl_current();
    create_targets(width, height);
}

void gl_set_fullscreen(bool fs) {
    if (g_sdl_win) SDL_SetWindowFullscreen(g_sdl_win, fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

RendererSettings& gl_settings() { return g_cfg; }
double gl_get_fps() { return g_fps; }
int gl_get_last_draw_calls() { return g_last_draw_calls; }

void gl_clear(uint32_t flags, uint32_t color_argb, float z, uint32_t stencil, GpuTextureObj* custom_rt) {
    if (!g_gl_ctx) return;
    ensure_gl_current();
    auto& gl = whacked_gl();
    if (!custom_rt) ++g_clear_calls;
    else ensure_gl_rt(custom_rt);

    GLuint fbo = (custom_rt && custom_rt->gl_fbo) ? custom_rt->gl_fbo : g_msaa_fbo;
    gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);

    GLbitfield mask = 0;
    if (flags & 1u) {
        glClearColor(((color_argb >> 16) & 0xFF) / 255.0f,
                     ((color_argb >>  8) & 0xFF) / 255.0f,
                     ((color_argb >>  0) & 0xFF) / 255.0f,
                     ((color_argb >> 24) & 0xFF) / 255.0f);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if ((flags & 2u) && !custom_rt) { glClearDepth(std::clamp(z, 0.0f, 1.0f)); mask |= GL_DEPTH_BUFFER_BIT; }
    if ((flags & 4u) && !custom_rt) { glClearStencil(static_cast<GLint>(stencil & 0xFFu)); mask |= GL_STENCIL_BUFFER_BIT; }
    if (mask) glClear(mask);
}

void gl_upload_texture(GpuTextureObj* t, uint32_t palette_va) {
    if (!g_gl_ctx || !t || !t->width || !t->height) return;
    ensure_gl_current();
    if (!t->data_va || !mem_ptr(t->data_va)) return;
    const uint8_t* src_base = static_cast<const uint8_t*>(mem_ptr(t->data_va));

    uint32_t f = t->xb_format;
    bool is_dxt = xb_is_dxt_format(f);
    uint32_t mips = std::max<uint32_t>(1, t->levels);
    uint32_t faces = t->is_cube ? 6u : 1u;

    if (!t->gl_tex) glGenTextures(1, &t->gl_tex);
    t->gl_is_cube = t->is_cube;
    GLenum bind_tgt = t->is_cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    glBindTexture(bind_tgt, t->gl_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    auto& gl = whacked_gl();
    const uint32_t* pal = (palette_va && mem_ptr(palette_va)) ? reinterpret_cast<const uint32_t*>(mem_ptr(palette_va)) : nullptr;
    size_t src_off = 0;

    for (uint32_t face = 0; face < faces; ++face) {
        GLenum img_tgt = t->is_cube ? (GL_TEXTURE_CUBE_MAP_POSITIVE_X + face) : GL_TEXTURE_2D;
        uint32_t mw = t->width, mh = t->height;
        for (uint32_t lvl = 0; lvl < mips; ++lvl) {
            if (is_dxt) {
                uint32_t bw = std::max<uint32_t>(1, (mw + 3u) / 4u);
                uint32_t bh = std::max<uint32_t>(1, (mh + 3u) / 4u);
                uint32_t bpb = (f == 0x0Cu) ? 8u : 16u;
                size_t bytes = size_t(bw) * bh * bpb;
                if (gl.has_s3tc && gl.glCompressedTexImage2D) {
                    GLenum ifmt = (f == 0x0Cu) ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT :
                                  (f == 0x0Eu) ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
                    gl.glCompressedTexImage2D(img_tgt, lvl, ifmt, mw, mh, 0, static_cast<GLsizei>(bytes), src_base + src_off);
                } else {
                    xb_decode_dxt_to_bgra(src_base + src_off, mw, mh, f, g_scratch_rgba);
                    glTexImage2D(img_tgt, lvl, GL_RGBA8, mw, mh, 0, GL_BGRA, GL_UNSIGNED_BYTE, g_scratch_rgba.data());
                }
                src_off += bytes;
            } else {
                size_t consumed = 0;
                xb_decode_mip_to_bgra(src_base + src_off, mw, mh, f, (lvl == 0 ? t->pitch : 0u), pal, g_scratch_rgba, consumed);
                glTexImage2D(img_tgt, lvl, GL_RGBA8, mw, mh, 0, GL_BGRA, GL_UNSIGNED_BYTE, g_scratch_rgba.data());
                src_off += consumed;
            }
            mw = std::max<uint32_t>(1, mw >> 1);
            mh = std::max<uint32_t>(1, mh >> 1);
        }
        src_off = (src_off + 63u) & ~63ull;
    }
    glTexParameteri(bind_tgt, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(mips - 1));
    if (mips == 1 && !is_dxt) gl.glGenerateMipmap(bind_tgt);
    glBindTexture(bind_tgt, 0);
    t->dirty = false;
}

void gl_draw(const DrawStateSnapshot& st) {
    if (!g_gl_ctx) return;
    ensure_gl_current();
    auto& gl = whacked_gl();
    if (!st.vs || !st.vs->gl_vs || !st.ps || !st.ps->gl_ps) { ++g_draw_skipped_shader; return; }
    GLuint prog = get_linked_program(st.vs, st.ps);
    if (!prog) { ++g_draw_skipped_shader; return; }
    ++g_draw_calls;

    bool to_rt = (st.custom_rt != nullptr);
    if (to_rt) ensure_gl_rt(st.custom_rt);
    GLuint fbo = (to_rt && st.custom_rt->gl_fbo) ? st.custom_rt->gl_fbo : g_msaa_fbo;
    gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    int rt_w = to_rt ? static_cast<int>(st.custom_rt->width)  : g_cfg.width;
    int rt_h = to_rt ? static_cast<int>(st.custom_rt->height) : g_cfg.height;
    float sx = static_cast<float>(rt_w) / std::max(1.0f, st.xb_fb_w);
    float sy = static_cast<float>(rt_h) / std::max(1.0f, st.xb_fb_h);
    int vpx = static_cast<int>(st.vp_x * sx);
    int vpw = std::max(1, static_cast<int>(st.vp_w * sx));
    int vph = std::max(1, static_cast<int>(st.vp_h * sy));
    int vpy = to_rt ? static_cast<int>(st.vp_y * sy) : (rt_h - static_cast<int>(st.vp_y * sy) - vph);
    glViewport(vpx, vpy, vpw, vph);
    glDepthRange(st.vp_minz, st.vp_maxz);

    if (st.alpha_blend) {
        glEnable(GL_BLEND);
        GLenum bop = (st.blend_op == 2) ? GL_FUNC_SUBTRACT : (st.blend_op == 3) ? GL_FUNC_REVERSE_SUBTRACT :
                     (st.blend_op == 4) ? GL_MIN : (st.blend_op == 5) ? GL_MAX : GL_FUNC_ADD;
        gl.glBlendEquation(bop);
        gl.glBlendFuncSeparate(gl_blend(st.src_blend, false), gl_blend(st.dst_blend, false),
                               gl_blend(st.src_blend, true), gl_blend(st.dst_blend, true));
    } else {
        glDisable(GL_BLEND);
    }

    glColorMask((st.color_write_mask & 1) != 0, (st.color_write_mask & 2) != 0,
                (st.color_write_mask & 4) != 0, (st.color_write_mask & 8) != 0);

    if (st.z_enable && !to_rt) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(gl_cmp(st.z_func));
        glDepthMask(st.z_write ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }

    if (st.stencil_enable && !to_rt) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(gl_cmp(st.stencil_func), static_cast<GLint>(st.stencil_ref & 0xFFu), st.stencil_mask & 0xFFu);
        glStencilOp(gl_stencil_op(st.stencil_fail), gl_stencil_op(st.stencil_zfail), gl_stencil_op(st.stencil_pass));
        glStencilMask(st.stencil_writemask & 0xFFu);
    } else {
        glDisable(GL_STENCIL_TEST);
    }

    glPolygonMode(GL_FRONT_AND_BACK, (st.fill_mode == 2) ? GL_LINE : GL_FILL);
    if (st.cull_mode == 1) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glFrontFace(to_rt ? GL_CCW : GL_CW);
        glCullFace((st.cull_mode == 2) ? GL_FRONT : GL_BACK);
    }

    for (int s = 0; s < 4; ++s) {
        gl.glActiveTexture(GL_TEXTURE0 + s);
        bool expect_cube = (st.ps->tex_is_cube_mask & (1 << s)) != 0;
        if (st.tex[s]) {
            if (st.tex[s]->dirty && !st.tex[s]->is_render_target) {
                gl_upload_texture(st.tex[s], st.current_palette_va);
            } else if (!st.tex[s]->gl_tex) {
                if (st.tex[s]->is_render_target) ensure_gl_rt(st.tex[s]);
                else gl_upload_texture(st.tex[s], st.current_palette_va);
            }
            GLenum tgt = st.tex[s]->gl_is_cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
            glBindTexture(tgt, st.tex[s]->gl_tex ? st.tex[s]->gl_tex : g_white_tex);
        } else {
            glBindTexture(expect_cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, expect_cube ? 0u : g_white_tex);
        }
        gl.glBindSampler(s, get_sampler(st.addr_u[s], st.addr_v[s], st.min_filter[s], st.mag_filter[s]));
    }

    bool guest_is_16x9 = (st.xb_fb_w / std::max(1.0f, st.xb_fb_h)) > 1.5f;
    float target_asp = static_cast<float>(rt_w) / std::max(1.0f, static_cast<float>(rt_h));
    float ws = (g_cfg.widescreen && !guest_is_16x9) ? ((4.0f / 3.0f) / std::max(0.5f, target_asp)) : 1.0f;
    float ui_ws = (!guest_is_16x9 && g_cfg.unstretched_hud) ? ws : 1.0f;
    float y_flip = to_rt ? -1.0f : 1.0f;

    gl.glUseProgram(prog);
    if (st.is_programmable_vs && st.vs_consts) {
        gl.glBindBuffer(GL_UNIFORM_BUFFER, g_ubo_vs_nv2a);
        gl.glBufferSubData(GL_UNIFORM_BUFFER, 0, 192 * 16, st.vs_consts);
        gl.glBindBufferBase(GL_UNIFORM_BUFFER, 0, g_ubo_vs_nv2a);

        float sc[8] = {st.xb_fb_w, st.xb_fb_h, st.vs->is_screenspace ? ui_ws : ws, st.vs->is_screenspace ? 1.0f : 0.0f, y_flip, 0, 0, 0};
        gl.glBindBuffer(GL_UNIFORM_BUFFER, g_ubo_vs_scr);
        gl.glBufferSubData(GL_UNIFORM_BUFFER, 0, 32, sc);
        gl.glBindBufferBase(GL_UNIFORM_BUFFER, 1, g_ubo_vs_scr);
    } else {
        float d[160]{};
        static const float ID[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        std::memcpy(d +  0, st.world_m ? st.world_m : ID, 64);
        std::memcpy(d + 16, st.view_m  ? st.view_m  : ID, 64);
        std::memcpy(d + 32, st.proj_m  ? st.proj_m  : ID, 64);
        std::memcpy(d + 48, st.mat_diffuse, 16);
        std::memcpy(d + 52, st.mat_ambient, 16);
        std::memcpy(d + 56, st.mat_specular, 16);
        std::memcpy(d + 60, st.mat_emissive, 16);
        std::memcpy(d + 64, st.global_ambient, 16);
        std::memcpy(d + 68,  st.light_pos,   64);
        std::memcpy(d + 84,  st.light_dir,   64);
        std::memcpy(d + 100, st.light_diff,  64);
        std::memcpy(d + 116, st.light_spec,  64);
        std::memcpy(d + 132, st.light_atten, 64);
        d[148] = st.fog_enable ? 1.0f : 0.0f;
        d[149] = st.fog_start; d[150] = st.fog_end; d[151] = st.fog_density;
        d[152] = st.xb_fb_w; d[153] = st.xb_fb_h;
        d[154] = st.vs->is_screenspace ? ui_ws : ws;
        d[155] = st.lighting_en ? 1.0f : 0.0f;
        d[156] = st.specular_en ? 1.0f : 0.0f;
        d[157] = y_flip; d[158] = 1.0f; d[159] = static_cast<float>(st.fog_mode);
        gl.glBindBuffer(GL_UNIFORM_BUFFER, g_ubo_ff_vs);
        gl.glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(d), d);
        gl.glBindBufferBase(GL_UNIFORM_BUFFER, 0, g_ubo_ff_vs);
    }

    float tex_scale[16]{};
    for (int s = 0; s < 4; ++s) {
        uint32_t f = st.tex[s] ? st.tex[s]->xb_format : 0u;
        bool is_lin = xb_is_linear_format(f);
        if (st.tex[s] && is_lin && st.tex[s]->width > 0 && st.tex[s]->height > 0) {
            tex_scale[s * 4 + 0] = 1.0f / static_cast<float>(st.tex[s]->width);
            tex_scale[s * 4 + 1] = 1.0f / static_cast<float>(st.tex[s]->height);
        } else {
            tex_scale[s * 4 + 0] = 1.0f; tex_scale[s * 4 + 1] = 1.0f;
        }
        tex_scale[s * 4 + 2] = 1.0f; tex_scale[s * 4 + 3] = 1.0f;
    }

    gl.glBindBuffer(GL_UNIFORM_BUFFER, g_ubo_ps);
    if (st.is_programmable_ps) {
        float p[88]{};
        if (st.ps_consts) std::memcpy(p, st.ps_consts, 16 * 16);
        p[64] = st.alpha_test ? 1.0f : 0.0f; p[65] = st.alpha_ref;
        p[66] = static_cast<float>(st.alpha_func); p[67] = st.fog_enable ? 1.0f : 0.0f;
        std::memcpy(p + 68, st.fog_color, 16);
        std::memcpy(p + 72, tex_scale, sizeof(tex_scale));
        gl.glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(p), p);
    } else {
        float p[28]{};
        std::memcpy(p + 0, st.tex_factor, 16);
        p[4] = st.alpha_test ? 1.0f : 0.0f; p[5] = st.alpha_ref;
        p[6] = static_cast<float>(st.alpha_func); p[7] = st.fog_enable ? 1.0f : 0.0f;
        std::memcpy(p + 8, st.fog_color, 16);
        std::memcpy(p + 12, tex_scale, sizeof(tex_scale));
        gl.glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(p), p);
    }
    gl.glBindBufferBase(GL_UNIFORM_BUFFER, 2, g_ubo_ps);

    uint32_t vcnt = st.vertex_count;
    if (st.ib_ptr && st.index_count > 0) {
        uint16_t max_idx = 0;
        for (uint32_t i = 0; i < st.index_count; ++i) max_idx = std::max(max_idx, st.ib_ptr[i]);
        vcnt = static_cast<uint32_t>(max_idx) + 1u + static_cast<uint32_t>(std::max(0, st.base_vertex));
    }
    if (vcnt == 0) { ++g_draw_skipped_vert; return; }

    gl.glBindVertexArray(g_vao);
    GLuint stream_vbos[4]{};
    GLuint stream_strides[4]{};
    GLuint stream_offs[4]{};

    // Pack all needed streams or bind static VBOs cleanly
    uint32_t total_dyn_bytes = 0;
    for (int s = 0; s < 4; ++s) {
        if (!st.vb_ptrs[s] || !st.vb_strides[s]) continue;
        if (st.vb_guest_va[s] && st.vb_total_bytes[s] > 0) {
            auto& entry = g_static_vbs[st.vb_guest_va[s]];
            if (!entry.buf || entry.bytes < st.vb_total_bytes[s]) {
                if (entry.buf) gl.glDeleteBuffers(1, &entry.buf);
                gl.glGenBuffers(1, &entry.buf);
                gl.glBindBuffer(GL_ARRAY_BUFFER, entry.buf);
                gl.glBufferData(GL_ARRAY_BUFFER, st.vb_total_bytes[s], st.vb_ptrs[s], GL_STATIC_DRAW);
                entry.bytes = st.vb_total_bytes[s];
                entry.seq = st.vb_dirty_seq[s];
            } else if (entry.seq != st.vb_dirty_seq[s]) {
                gl.glBindBuffer(GL_ARRAY_BUFFER, entry.buf);
                gl.glBufferSubData(GL_ARRAY_BUFFER, 0, st.vb_total_bytes[s], st.vb_ptrs[s]);
                entry.seq = st.vb_dirty_seq[s];
            }
            stream_vbos[s] = entry.buf;
            stream_strides[s] = st.vb_strides[s];
            stream_offs[s] = st.vb_offsets[s];
        } else {
            uint32_t sz = vcnt * st.vb_strides[s];
            stream_offs[s] = total_dyn_bytes;
            total_dyn_bytes += (sz + 15u) & ~15u;
        }
    }
    if (total_dyn_bytes > 0) {
        gl.glBindBuffer(GL_ARRAY_BUFFER, g_dyn_vb);
        gl.glBufferData(GL_ARRAY_BUFFER, total_dyn_bytes, nullptr, GL_STREAM_DRAW);
        for (int s = 0; s < 4; ++s) {
            if (stream_vbos[s] || !st.vb_ptrs[s] || !st.vb_strides[s]) continue;
            uint32_t sz = vcnt * st.vb_strides[s];
            gl.glBufferSubData(GL_ARRAY_BUFFER, stream_offs[s], sz, st.vb_ptrs[s]);
            stream_vbos[s] = g_dyn_vb;
            stream_strides[s] = st.vb_strides[s];
        }
    }

    bool attr_on[16]{};
    for (const auto& e : st.vs->decl.elements) {
        if (e.reg >= 16 || e.stream >= 4 || !stream_vbos[e.stream]) continue;
        gl.glBindBuffer(GL_ARRAY_BUFFER, stream_vbos[e.stream]);
        gl.glEnableVertexAttribArray(e.reg);
        set_vertex_attrib_from_decl(e, stream_strides[e.stream], stream_offs[e.stream]);
        attr_on[e.reg] = true;
    }
    for (int i = 0; i < 16; ++i) {
        if (!attr_on[i]) gl.glDisableVertexAttribArray(i);
    }

    bool indexed = (st.ib_ptr != nullptr && st.index_count > 0);
    auto idx_at = [&](uint32_t i) -> uint16_t { return indexed ? st.ib_ptr[i] : static_cast<uint16_t>(i); };
    uint32_t n = indexed ? st.index_count : st.vertex_count;

    if (st.prim_type == 7 || st.prim_type == 8 || st.prim_type == 9) {
        g_scratch_tris.clear();
        if (st.prim_type == 7 && n >= 3) {
            uint16_t v0 = idx_at(0);
            for (uint32_t i = 1; i + 1 < n; ++i) {
                g_scratch_tris.push_back(v0); g_scratch_tris.push_back(idx_at(i)); g_scratch_tris.push_back(idx_at(i + 1));
            }
        } else if (st.prim_type == 8 && n >= 4) {
            for (uint32_t i = 0; i + 3 < n; i += 4) {
                uint16_t i0 = idx_at(i), i1 = idx_at(i+1), i2 = idx_at(i+2), i3 = idx_at(i+3);
                g_scratch_tris.push_back(i0); g_scratch_tris.push_back(i1); g_scratch_tris.push_back(i2);
                g_scratch_tris.push_back(i0); g_scratch_tris.push_back(i2); g_scratch_tris.push_back(i3);
            }
        } else if (st.prim_type == 9 && n >= 4) {
            for (uint32_t i = 0; i + 3 < n; i += 2) {
                g_scratch_tris.push_back(idx_at(i)); g_scratch_tris.push_back(idx_at(i+1)); g_scratch_tris.push_back(idx_at(i+3));
                g_scratch_tris.push_back(idx_at(i)); g_scratch_tris.push_back(idx_at(i+3)); g_scratch_tris.push_back(idx_at(i+2));
            }
        }
        if (g_scratch_tris.empty()) return;
        gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_dyn_ib);
        gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER, g_scratch_tris.size() * 2, g_scratch_tris.data(), GL_STREAM_DRAW);
        if (st.base_vertex != 0) {
            gl.glDrawElementsBaseVertex(GL_TRIANGLES, static_cast<GLsizei>(g_scratch_tris.size()), GL_UNSIGNED_SHORT, nullptr, st.base_vertex);
        } else {
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(g_scratch_tris.size()), GL_UNSIGNED_SHORT, nullptr);
        }
        return;
    }

    GLenum mode = GL_TRIANGLES;
    if (st.prim_type == 1) mode = GL_POINTS;
    else if (st.prim_type == 2) mode = GL_LINES;
    else if (st.prim_type == 3 || st.prim_type == 4) mode = GL_LINE_STRIP;
    else if (st.prim_type == 6) mode = GL_TRIANGLE_STRIP;

    if (indexed) {
        gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_dyn_ib);
        gl.glBufferData(GL_ELEMENT_ARRAY_BUFFER, n * 2, st.ib_ptr, GL_STREAM_DRAW);
        if (st.base_vertex != 0) {
            gl.glDrawElementsBaseVertex(mode, static_cast<GLsizei>(n), GL_UNSIGNED_SHORT, nullptr, st.base_vertex);
        } else {
            glDrawElements(mode, static_cast<GLsizei>(n), GL_UNSIGNED_SHORT, nullptr);
        }
    } else {
        glDrawArrays(mode, 0, static_cast<GLsizei>(n));
    }
}

void gl_blit_rgba_rect(const void* bgra, int src_w, int src_h, int dst_x, int dst_y, int dst_w, int dst_h) {
    if (!g_gl_ctx || !bgra || src_w <= 0 || src_h <= 0) return;
    ensure_gl_current();
    auto& gl = whacked_gl();
    if (!g_bink_tex || g_bink_w != src_w || g_bink_h != src_h) {
        if (!g_bink_tex) glGenTextures(1, &g_bink_tex);
        glBindTexture(GL_TEXTURE_2D, g_bink_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, src_w, src_h, 0, GL_BGRA, GL_UNSIGNED_BYTE, bgra);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g_bink_w = src_w; g_bink_h = src_h;
    } else {
        glBindTexture(GL_TEXTURE_2D, g_bink_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, src_w, src_h, GL_BGRA, GL_UNSIGNED_BYTE, bgra);
    }

    gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_cfg.width, g_cfg.height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    float dw = dst_w > 0 ? static_cast<float>(dst_w) : static_cast<float>(g_cfg.width);
    float dh = dst_h > 0 ? static_cast<float>(dst_h) : static_cast<float>(g_cfg.height);
    if (g_cfg.unstretched_hud && dw == g_cfg.width && dh == g_cfg.height) {
        float asp_w = dh * (4.0f / 3.0f);
        if (asp_w < dw) { dst_x = static_cast<int>((dw - asp_w) * 0.5f); dw = asp_w; }
    }
    float x0 = (dst_x / static_cast<float>(g_cfg.width)) * 2.0f - 1.0f;
    float x1 = ((dst_x + dw) / static_cast<float>(g_cfg.width)) * 2.0f - 1.0f;
    float y0 = 1.0f - (dst_y / static_cast<float>(g_cfg.height)) * 2.0f;
    float y1 = 1.0f - ((dst_y + dh) / static_cast<float>(g_cfg.height)) * 2.0f;
    struct QV { float x, y, u, v; uint32_t col; } verts[6] = {
        {x0, y0, 0, 0, 0xFFFFFFFFu}, {x1, y0, 1, 0, 0xFFFFFFFFu}, {x0, y1, 0, 1, 0xFFFFFFFFu},
        {x1, y0, 1, 0, 0xFFFFFFFFu}, {x1, y1, 1, 1, 0xFFFFFFFFu}, {x0, y1, 0, 1, 0xFFFFFFFFu}
    };
    gl.glBindVertexArray(g_quad_vao);
    gl.glBindBuffer(GL_ARRAY_BUFFER, g_quad_vbo);
    gl.glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    gl.glUseProgram(g_quad_prog_blit);
    gl.glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_bink_tex);
    gl.glBindSampler(0, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void gl_present() {
    if (!g_sdl_win || !g_gl_ctx) return;
    ensure_gl_current();
    int pw = g_pending_resize_w.exchange(0);
    int ph = g_pending_resize_h.exchange(0);
    if (pw > 0 && ph > 0) create_targets(pw, ph);
    auto& gl = whacked_gl();

    ++g_frame_count;
    ++g_fps_frames;
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - g_fps_t0).count();
    if (dt >= 0.5) {
        g_fps = g_fps_frames / dt;
        g_fps_frames = 0;
        g_fps_t0 = now;
    }

    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    if (ui_is_launcher_active()) {
        gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, g_cfg.width, g_cfg.height);
        glClearColor(0.05f, 0.06f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    } else if (g_draw_calls > 0 || g_clear_calls > 0) {
        gl.glBindFramebuffer(GL_READ_FRAMEBUFFER, g_msaa_fbo);
        gl.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_resolve_fbo);
        gl.glBlitFramebuffer(0, 0, g_cfg.width, g_cfg.height, 0, 0, g_cfg.width, g_cfg.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, g_cfg.width, g_cfg.height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);

        struct QV { float x, y, u, v; uint32_t col; } fsq[6] = {
            {-1,  1, 0, 1, 0xFFFFFFFFu}, { 1,  1, 1, 1, 0xFFFFFFFFu}, {-1, -1, 0, 0, 0xFFFFFFFFu},
            { 1,  1, 1, 1, 0xFFFFFFFFu}, { 1, -1, 1, 0, 0xFFFFFFFFu}, {-1, -1, 0, 0, 0xFFFFFFFFu}
        };
        gl.glBindVertexArray(g_quad_vao);
        gl.glBindBuffer(GL_ARRAY_BUFFER, g_quad_vbo);
        gl.glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(fsq), fsq);
        gl.glUseProgram((g_cfg.bloom_sharpen && g_quad_prog_post) ? g_quad_prog_post : g_quad_prog_blit);
        gl.glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_resolve_tex);
        gl.glBindSampler(0, 0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_cfg.width, g_cfg.height);
    ui_render_frame(true);

    if (!g_pending_screenshot.empty()) {
        save_gl_backbuffer_png(g_pending_screenshot);
        g_pending_screenshot.clear();
    }

    g_last_draw_calls = g_draw_calls;
    if (!ui_is_launcher_active() &&
        (g_frame_count == 1 || g_frame_count == 15 || g_frame_count == 60 || g_frame_count == 180 || g_frame_count == 360)) {
        std::printf("[GFX-GL] Frame %d: clears=%d draws=%d skip_sh=%d skip_v=%d\n",
                    g_frame_count, g_clear_calls, g_draw_calls, g_draw_skipped_shader, g_draw_skipped_vert);
    }

    SDL_GL_SwapWindow(g_sdl_win);
    g_draw_calls = 0;
    g_clear_calls = 0;
    g_draw_skipped_shader = 0;
    g_draw_skipped_vert = 0;

    if (g_cfg.fps_cap > 0 && !g_cfg.vsync) {
        double target_ms = 1000.0 / g_cfg.fps_cap;
        double elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_frame_t0).count();
        if (elapsed_ms + 1.5 < target_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(target_ms - elapsed_ms)));
        }
    }
    g_frame_t0 = std::chrono::steady_clock::now();
}

void gl_request_screenshot(const std::string& filename) {
    g_pending_screenshot = filename;
}
