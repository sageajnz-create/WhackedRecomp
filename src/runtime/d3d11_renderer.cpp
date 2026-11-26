#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include "d3d11_renderer.h"
#include "tex_decode.h"
#include "launcher_ui.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

HWND g_hwnd = nullptr;
RendererSettings g_cfg{};
std::atomic<int> g_pending_resize_w{0}, g_pending_resize_h{0};
std::thread::id g_dx_owner_tid{};

ID3D11Device*           g_dev = nullptr;
ID3D11DeviceContext*    g_ctx = nullptr;
IDXGISwapChain*         g_sc = nullptr;
ID3D11RenderTargetView* g_back_rtv = nullptr;

ID3D11Texture2D*        g_msaa_color = nullptr;
ID3D11RenderTargetView* g_msaa_rtv = nullptr;
ID3D11Texture2D*        g_resolve_tex = nullptr;
ID3D11ShaderResourceView* g_resolve_srv = nullptr;
ID3D11Texture2D*        g_depth_tex = nullptr;
ID3D11DepthStencilView* g_depth_dsv = nullptr;

ID3D11Buffer* g_dyn_vb = nullptr;
ID3D11Buffer* g_dyn_ib = nullptr;
UINT          g_dyn_vb_off = 0;
UINT          g_dyn_ib_off = 0;
constexpr UINT DYN_VB_BYTES = 12 * 1024 * 1024;
constexpr UINT DYN_IB_BYTES = 4 * 1024 * 1024;

struct CachedGpuBuf {
    ID3D11Buffer* buf = nullptr;
    uint32_t bytes = 0;
    uint32_t seq = 0;
};
std::unordered_map<uint32_t, CachedGpuBuf> g_static_vbs;
std::unordered_map<uint32_t, CachedGpuBuf> g_static_ibs;
std::mutex g_retire_mtx;
std::vector<GpuTextureObj*> g_retired_textures;
std::vector<uint32_t> g_retired_buffers;

void drain_retired_dx_resources() {
    std::vector<GpuTextureObj*> texs;
    std::vector<uint32_t> bufs;
    {
        std::lock_guard<std::mutex> lk(g_retire_mtx);
        texs.swap(g_retired_textures);
        bufs.swap(g_retired_buffers);
    }
    for (uint32_t va : bufs) {
        auto vit = g_static_vbs.find(va);
        if (vit != g_static_vbs.end()) {
            if (vit->second.buf) vit->second.buf->Release();
            g_static_vbs.erase(vit);
        }
        auto iit = g_static_ibs.find(va);
        if (iit != g_static_ibs.end()) {
            if (iit->second.buf) iit->second.buf->Release();
            g_static_ibs.erase(iit);
        }
    }
    for (GpuTextureObj* t : texs) {
        if (!t) continue;
        if (t->dx_dsv) { static_cast<ID3D11DepthStencilView*>(t->dx_dsv)->Release(); t->dx_dsv = nullptr; }
        if (t->dx_rtv) { static_cast<ID3D11RenderTargetView*>(t->dx_rtv)->Release(); t->dx_rtv = nullptr; }
        if (t->dx_srv) { static_cast<ID3D11ShaderResourceView*>(t->dx_srv)->Release(); t->dx_srv = nullptr; }
        if (t->dx_tex) { static_cast<ID3D11Texture2D*>(t->dx_tex)->Release(); t->dx_tex = nullptr; }
        delete t;
    }
}

ID3D11Buffer* g_cb_vs_nv2a = nullptr; // 192 * 16 bytes
ID3D11Buffer* g_cb_vs_scr  = nullptr; // 32 bytes
ID3D11Buffer* g_cb_ff_vs   = nullptr; // 640 bytes
ID3D11Buffer* g_cb_ps_nv2a = nullptr; // 352 bytes
ID3D11Buffer* g_cb_ff_ps   = nullptr; // 112 bytes

ID3D11VertexShader*      g_quad_vs = nullptr;
ID3D11PixelShader*       g_blit_ps = nullptr;
ID3D11PixelShader*       g_post_ps = nullptr;
ID3D11Texture2D*         g_bink_tex = nullptr;
ID3D11ShaderResourceView* g_bink_srv = nullptr;
int g_bink_w = 0, g_bink_h = 0;

ID3D11ShaderResourceView* g_white_srv = nullptr;
ID3D11Buffer*             g_quad_vb   = nullptr;
ID3D11InputLayout*        g_quad_il   = nullptr;

std::unordered_map<uint64_t, ID3D11BlendState*>        g_blend_cache;
std::unordered_map<uint64_t, ID3D11DepthStencilState*> g_ds_cache;
std::unordered_map<uint64_t, ID3D11RasterizerState*>   g_rs_cache;
std::unordered_map<uint64_t, ID3D11SamplerState*>      g_samp_cache;

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
int g_fps_frames = 0;

struct QuadVert { float x, y, u, v; uint32_t col; };

uint64_t fnv_step(uint64_t h, uint64_t v) {
    h ^= v;
    return h * 0x100000001b3ull;
}

void release_targets() {
    if (g_back_rtv)   { g_back_rtv->Release();   g_back_rtv = nullptr; }
    if (g_msaa_rtv)   { g_msaa_rtv->Release();   g_msaa_rtv = nullptr; }
    if (g_msaa_color) { g_msaa_color->Release(); g_msaa_color = nullptr; }
    if (g_resolve_srv){ g_resolve_srv->Release();g_resolve_srv = nullptr; }
    if (g_resolve_tex){ g_resolve_tex->Release();g_resolve_tex = nullptr; }
    if (g_depth_dsv)  { g_depth_dsv->Release();  g_depth_dsv = nullptr; }
    if (g_depth_tex)  { g_depth_tex->Release();  g_depth_tex = nullptr; }
}

bool create_targets(int w, int h) {
    release_targets();
    g_cfg.width = std::max(64, w);
    g_cfg.height = std::max(64, h);

    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return false;
    g_dev->CreateRenderTargetView(back, nullptr, &g_back_rtv);
    back->Release();

    UINT sample_cnt = 1;
    if (g_cfg.msaa_4x) {
        UINT q = 0;
        if (SUCCEEDED(g_dev->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, 4, &q)) && q > 0) {
            sample_cnt = 4;
        }
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(g_cfg.width);
    td.Height = static_cast<UINT>(g_cfg.height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = sample_cnt;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_msaa_color))) return false;
    g_dev->CreateRenderTargetView(g_msaa_color, nullptr, &g_msaa_rtv);

    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_resolve_tex))) return false;
    g_dev->CreateShaderResourceView(g_resolve_tex, nullptr, &g_resolve_srv);

    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = static_cast<UINT>(g_cfg.width);
    dd.Height = static_cast<UINT>(g_cfg.height);
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = sample_cnt;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(g_dev->CreateTexture2D(&dd, nullptr, &g_depth_tex))) return false;
    g_dev->CreateDepthStencilView(g_depth_tex, nullptr, &g_depth_dsv);
    return true;
}

D3D11_BLEND d3d_blend_to_dx11(uint32_t b, bool alpha_ch) {
    switch (b) {
        case 1: return D3D11_BLEND_ZERO;
        case 2: return D3D11_BLEND_ONE;
        case 3: return alpha_ch ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
        case 4: return alpha_ch ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
        case 5: return D3D11_BLEND_SRC_ALPHA;
        case 6: return D3D11_BLEND_INV_SRC_ALPHA;
        case 7: return D3D11_BLEND_DEST_ALPHA;
        case 8: return D3D11_BLEND_INV_DEST_ALPHA;
        case 9: return alpha_ch ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
        case 10:return alpha_ch ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
        case 11:return D3D11_BLEND_SRC_ALPHA_SAT;
        default:return D3D11_BLEND_ONE;
    }
}

D3D11_COMPARISON_FUNC d3d_cmp_to_dx11(uint32_t c) {
    switch (c) {
        case 1: return D3D11_COMPARISON_NEVER;
        case 2: return D3D11_COMPARISON_LESS;
        case 3: return D3D11_COMPARISON_EQUAL;
        case 4: return D3D11_COMPARISON_LESS_EQUAL;
        case 5: return D3D11_COMPARISON_GREATER;
        case 6: return D3D11_COMPARISON_NOT_EQUAL;
        case 7: return D3D11_COMPARISON_GREATER_EQUAL;
        default:return D3D11_COMPARISON_ALWAYS;
    }
}

D3D11_STENCIL_OP d3d_stencil_op_to_dx11(uint32_t op) {
    switch (op) {
        case 1: case 0x1E00u: return D3D11_STENCIL_OP_KEEP;
        case 2: case 0u:      return D3D11_STENCIL_OP_ZERO;
        case 3: case 0x1E01u: return D3D11_STENCIL_OP_REPLACE;
        case 4: case 0x1E02u: return D3D11_STENCIL_OP_INCR_SAT;
        case 5: case 0x1E03u: return D3D11_STENCIL_OP_DECR_SAT;
        case 6: case 0x150Au: return D3D11_STENCIL_OP_INVERT;
        case 7: case 0x8507u: return D3D11_STENCIL_OP_INCR;
        case 8: case 0x8508u: return D3D11_STENCIL_OP_DECR;
        default: return D3D11_STENCIL_OP_KEEP;
    }
}

D3D11_TEXTURE_ADDRESS_MODE d3d_addr_to_dx11(uint32_t a) {
    switch (a) {
        case 1: return D3D11_TEXTURE_ADDRESS_WRAP;
        case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case 3: return D3D11_TEXTURE_ADDRESS_CLAMP;
        case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
        default:return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

ID3D11BlendState* get_blend_state(bool en, uint32_t src, uint32_t dst, uint32_t op, uint32_t mask) {
    uint64_t k = (en ? 1ull : 0ull) | (uint64_t(src) << 4) | (uint64_t(dst) << 12) | (uint64_t(op) << 20) | (uint64_t(mask) << 28);
    auto it = g_blend_cache.find(k);
    if (it != g_blend_cache.end()) return it->second;

    D3D11_BLEND_DESC d{};
    d.RenderTarget[0].BlendEnable = en ? TRUE : FALSE;
    d.RenderTarget[0].SrcBlend = d3d_blend_to_dx11(src, false);
    d.RenderTarget[0].DestBlend = d3d_blend_to_dx11(dst, false);
    d.RenderTarget[0].BlendOp = (op == 2) ? D3D11_BLEND_OP_SUBTRACT : (op == 3) ? D3D11_BLEND_OP_REV_SUBTRACT :
                                (op == 4) ? D3D11_BLEND_OP_MIN : (op == 5) ? D3D11_BLEND_OP_MAX : D3D11_BLEND_OP_ADD;
    d.RenderTarget[0].SrcBlendAlpha = d3d_blend_to_dx11(src, true);
    d.RenderTarget[0].DestBlendAlpha = d3d_blend_to_dx11(dst, true);
    d.RenderTarget[0].BlendOpAlpha = d.RenderTarget[0].BlendOp;
    d.RenderTarget[0].RenderTargetWriteMask = static_cast<UINT8>(mask & 0xF);
    ID3D11BlendState* s = nullptr;
    g_dev->CreateBlendState(&d, &s);
    g_blend_cache[k] = s;
    return s;
}

ID3D11DepthStencilState* get_ds_state(bool zen, bool zwrite, uint32_t zfunc,
                                      bool sten_en, uint32_t sfunc, uint32_t sfail, uint32_t szfail, uint32_t spass,
                                      uint8_t smask, uint8_t swmask) {
    uint64_t k = 0xcbf29ce484222325ull;
    k = fnv_step(k, zen ? 1 : 0);
    k = fnv_step(k, zwrite ? 1 : 0);
    k = fnv_step(k, zfunc);
    k = fnv_step(k, sten_en ? 1 : 0);
    if (sten_en) {
        k = fnv_step(k, sfunc);
        k = fnv_step(k, sfail);
        k = fnv_step(k, szfail);
        k = fnv_step(k, spass);
        k = fnv_step(k, (uint64_t(smask) << 8) | swmask);
    }
    auto it = g_ds_cache.find(k);
    if (it != g_ds_cache.end()) return it->second;

    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = zen ? TRUE : FALSE;
    d.DepthWriteMask = zwrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = d3d_cmp_to_dx11(zfunc);
    d.StencilEnable = sten_en ? TRUE : FALSE;
    d.StencilReadMask = smask;
    d.StencilWriteMask = swmask;
    d.FrontFace.StencilFailOp = d3d_stencil_op_to_dx11(sfail);
    d.FrontFace.StencilDepthFailOp = d3d_stencil_op_to_dx11(szfail);
    d.FrontFace.StencilPassOp = d3d_stencil_op_to_dx11(spass);
    d.FrontFace.StencilFunc = d3d_cmp_to_dx11(sfunc);
    d.BackFace = d.FrontFace;
    ID3D11DepthStencilState* s = nullptr;
    g_dev->CreateDepthStencilState(&d, &s);
    g_ds_cache[k] = s;
    return s;
}

ID3D11RasterizerState* get_rs_state(uint32_t cull, uint32_t fill) {
    uint64_t k = uint64_t(cull) | (uint64_t(fill) << 8) | (g_cfg.msaa_4x ? 0x10000ull : 0ull);
    auto it = g_rs_cache.find(k);
    if (it != g_rs_cache.end()) return it->second;

    D3D11_RASTERIZER_DESC d{};
    d.FillMode = (fill == 2) ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    d.CullMode = (cull == 1) ? D3D11_CULL_NONE : (cull == 2) ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
    d.FrontCounterClockwise = FALSE;
    d.DepthClipEnable = TRUE;
    d.MultisampleEnable = g_cfg.msaa_4x ? TRUE : FALSE;
    ID3D11RasterizerState* s = nullptr;
    g_dev->CreateRasterizerState(&d, &s);
    g_rs_cache[k] = s;
    return s;
}

ID3D11SamplerState* get_sampler(uint32_t au, uint32_t av, uint32_t minf, uint32_t magf) {
    uint64_t k = uint64_t(au) | (uint64_t(av) << 4) | (uint64_t(minf) << 8) | (uint64_t(magf) << 12) | (g_cfg.aniso_16x ? 0x10000ull : 0ull);
    auto it = g_samp_cache.find(k);
    if (it != g_samp_cache.end()) return it->second;

    D3D11_SAMPLER_DESC d{};
    d.AddressU = d3d_addr_to_dx11(au);
    d.AddressV = d3d_addr_to_dx11(av);
    d.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    if (g_cfg.aniso_16x && minf >= 2 && magf >= 2) {
        d.Filter = D3D11_FILTER_ANISOTROPIC;
        d.MaxAnisotropy = 16;
    } else if (minf <= 1 && magf <= 1) {
        d.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        d.MaxAnisotropy = 1;
    } else {
        d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        d.MaxAnisotropy = 1;
    }
    ID3D11SamplerState* s = nullptr;
    g_dev->CreateSamplerState(&d, &s);
    g_samp_cache[k] = s;
    return s;
}

void init_quad_and_white() {
    const char* qvs = R"(
        struct VI { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
        struct VO { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
        VO main(VI i) { VO o; o.pos = float4(i.pos, 0, 1); o.uv = i.uv; o.col = i.col; return o; }
    )";
    const char* qps = R"(
        Texture2D tx : register(t0); SamplerState sp : register(s0);
        struct VO { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
        float4 main(VO i) : SV_TARGET { return tx.Sample(sp, i.uv) * i.col; }
    )";
    const char* post_ps = R"(
        Texture2D tx : register(t0); SamplerState sp : register(s0);
        struct VO { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
        float4 main(VO i) : SV_TARGET {
            uint tw = 1, th = 1;
            tx.GetDimensions(tw, th);
            float2 ts = 1.0 / float2(max(tw, 1u), max(th, 1u));

            float3 cM = tx.Sample(sp, i.uv).rgb;
            float3 cN = tx.Sample(sp, i.uv + float2(0.0, -ts.y)).rgb;
            float3 cS = tx.Sample(sp, i.uv + float2(0.0,  ts.y)).rgb;
            float3 cW = tx.Sample(sp, i.uv + float2(-ts.x, 0.0)).rgb;
            float3 cE = tx.Sample(sp, i.uv + float2( ts.x, 0.0)).rgb;

            // FidelityFX-style Contrast Adaptive Sharpening (no halo ringing on 2D UI)
            float3 mn = min(cM, min(min(cN, cS), min(cW, cE)));
            float3 mx = max(cM, max(max(cN, cS), max(cW, cE)));
            float3 amp = saturate(min(mn, 1.0 - mx) / max(mx, 1e-5));
            float3 w = -sqrt(amp) * 0.14;
            float3 sharp = (cM + (cN + cS + cW + cE) * w) / max(1.0 + 4.0 * w, 0.05);
            sharp = clamp(sharp, mn, mx);

            // Dual-ring 8-tap soft-knee highlight bloom
            float3 b0 = tx.Sample(sp, i.uv + ts * float2(-1.5, -1.5)).rgb
                      + tx.Sample(sp, i.uv + ts * float2( 1.5, -1.5)).rgb
                      + tx.Sample(sp, i.uv + ts * float2(-1.5,  1.5)).rgb
                      + tx.Sample(sp, i.uv + ts * float2( 1.5,  1.5)).rgb;
            float3 b1 = tx.Sample(sp, i.uv + ts * float2( 0.0, -3.25)).rgb
                      + tx.Sample(sp, i.uv + ts * float2( 0.0,  3.25)).rgb
                      + tx.Sample(sp, i.uv + ts * float2(-3.25, 0.0)).rgb
                      + tx.Sample(sp, i.uv + ts * float2( 3.25, 0.0)).rgb;
            float3 bAvg = b0 * 0.15 + b1 * 0.10;
            float3 h = max(bAvg - 0.78, 0.0);
            float3 glow = (h * h) / (h + 0.22);

            return float4(saturate(sharp + glow * 0.36), 1.0);
        }
    )";

    auto comp = [](const char* s, const char* tgt, ID3DBlob** b) {
        ID3DBlob* err = nullptr;
        bool ok = SUCCEEDED(D3DCompile(s, std::strlen(s), nullptr, nullptr, nullptr, "main", tgt, 0, 0, b, &err));
        if (err) err->Release();
        return ok;
    };
    ID3DBlob *vb = nullptr, *pb = nullptr, *ppb = nullptr;
    if (comp(qvs, "vs_5_0", &vb)) {
        g_dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_quad_vs);
        D3D11_INPUT_ELEMENT_DESC il[3] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        g_dev->CreateInputLayout(il, 3, vb->GetBufferPointer(), vb->GetBufferSize(), &g_quad_il);
        vb->Release();
    }
    if (comp(qps, "ps_5_0", &pb)) {
        g_dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_blit_ps);
        pb->Release();
    }
    if (comp(post_ps, "ps_5_0", &ppb)) {
        g_dev->CreatePixelShader(ppb->GetBufferPointer(), ppb->GetBufferSize(), nullptr, &g_post_ps);
        ppb->Release();
    }

    D3D11_BUFFER_DESC qb{};
    qb.ByteWidth = 65536;
    qb.Usage = D3D11_USAGE_DYNAMIC;
    qb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    qb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&qb, nullptr, &g_quad_vb);

    uint32_t white = 0xFFFFFFFFu;
    D3D11_TEXTURE2D_DESC wt{};
    wt.Width = wt.Height = 1; wt.MipLevels = wt.ArraySize = 1;
    wt.Format = DXGI_FORMAT_R8G8B8A8_UNORM; wt.SampleDesc.Count = 1;
    wt.Usage = D3D11_USAGE_IMMUTABLE; wt.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA ws{&white, 4, 0};
    ID3D11Texture2D* wtex = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&wt, &ws, &wtex))) {
        g_dev->CreateShaderResourceView(wtex, nullptr, &g_white_srv);
        wtex->Release();
    }
}

void draw_2d_quads(const std::vector<QuadVert>& verts, ID3D11ShaderResourceView* srv) {
    if (verts.empty() || !g_quad_vb || !g_quad_vs || !g_blit_ps) return;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_ctx->Map(g_quad_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    UINT copy_n = static_cast<UINT>(std::min<size_t>(verts.size(), 65536 / sizeof(QuadVert)));
    std::memcpy(m.pData, verts.data(), copy_n * sizeof(QuadVert));
    g_ctx->Unmap(g_quad_vb, 0);

    D3D11_VIEWPORT vp{0, 0, static_cast<float>(g_cfg.width), static_cast<float>(g_cfg.height), 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->OMSetRenderTargets(1, &g_back_rtv, nullptr);
    g_ctx->OMSetDepthStencilState(get_ds_state(false, false, 8, false, 8, 1, 1, 1, 0xFF, 0xFF), 0);
    g_ctx->OMSetBlendState(get_blend_state(true, 5, 6, 1, 0xF), nullptr, 0xFFFFFFFF);
    g_ctx->RSSetState(get_rs_state(1, 3));
    UINT stride = sizeof(QuadVert), off = 0;
    g_ctx->IASetInputLayout(g_quad_il);
    g_ctx->IASetVertexBuffers(0, 1, &g_quad_vb, &stride, &off);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_quad_vs, nullptr, 0);
    g_ctx->PSSetShader(g_blit_ps, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* ss = get_sampler(3, 3, 1, 1);
    g_ctx->PSSetSamplers(0, 1, &ss);
    g_ctx->Draw(copy_n, 0);
}

void ensure_dx_rt(GpuTextureObj* t) {
    if (!t || (t->dx_tex && (!t->is_render_target || t->dx_rtv))) return;
    if (t->width == 0 || t->height == 0) return;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = t->width;
    td.Height = t->height;
    td.MipLevels = std::max<uint32_t>(1, t->levels);
    td.ArraySize = t->is_cube ? 6u : 1u;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (t->is_render_target ? D3D11_BIND_RENDER_TARGET : 0u);
    td.MiscFlags = t->is_cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &tex))) {
        t->dx_tex = tex;
        ID3D11ShaderResourceView* srv = nullptr;
        g_dev->CreateShaderResourceView(tex, nullptr, &srv);
        t->dx_srv = srv;
        if (t->is_render_target) {
            ID3D11RenderTargetView* rtv = nullptr;
            g_dev->CreateRenderTargetView(tex, nullptr, &rtv);
            t->dx_rtv = rtv;
            if (!t->is_cube && !t->dx_dsv) {
                D3D11_TEXTURE2D_DESC dd{};
                dd.Width = t->width;
                dd.Height = t->height;
                dd.MipLevels = 1;
                dd.ArraySize = 1;
                dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
                dd.SampleDesc.Count = 1;
                dd.Usage = D3D11_USAGE_DEFAULT;
                dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
                ID3D11Texture2D* dtex = nullptr;
                if (SUCCEEDED(g_dev->CreateTexture2D(&dd, nullptr, &dtex))) {
                    ID3D11DepthStencilView* dsv = nullptr;
                    if (SUCCEEDED(g_dev->CreateDepthStencilView(dtex, nullptr, &dsv))) {
                        t->dx_dsv = dsv;
                    }
                    dtex->Release();
                }
            }
        }
        t->dirty = false;
    }
}

void save_backbuffer_png(const std::string& path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return;
    D3D11_TEXTURE2D_DESC td{};
    back->GetDesc(&td);
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.Usage = D3D11_USAGE_STAGING;
    td.MiscFlags = 0;
    ID3D11Texture2D* stg = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &stg))) {
        g_ctx->CopyResource(stg, back);
        D3D11_MAPPED_SUBRESOURCE mr{};
        if (SUCCEEDED(g_ctx->Map(stg, 0, D3D11_MAP_READ, 0, &mr))) {
            std::vector<uint8_t> rgba(size_t(td.Width) * td.Height * 4u);
            for (UINT y = 0; y < td.Height; ++y) {
                const uint8_t* src = static_cast<const uint8_t*>(mr.pData) + size_t(y) * mr.RowPitch;
                uint8_t* dst = rgba.data() + size_t(y) * td.Width * 4u;
                for (UINT x = 0; x < td.Width; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + 2]; // B -> R
                    dst[x * 4 + 1] = src[x * 4 + 1]; // G -> G
                    dst[x * 4 + 2] = src[x * 4 + 0]; // R -> B
                    dst[x * 4 + 3] = 255;
                }
            }
            if (gfx_save_png_rgba(path, static_cast<int>(td.Width), static_cast<int>(td.Height), rgba.data(), static_cast<int>(td.Width * 4u))) {
                std::printf("[GFX] Saved PNG screenshot: %s (%ux%u)\n", path.c_str(), td.Width, td.Height);
            }
            g_ctx->Unmap(stg, 0);
        }
        stg->Release();
    }
    back->Release();
}

} // namespace

bool d3d11_init(void* sdl_win, const RendererSettings& settings) {
    SDL_SysWMinfo wmi{};
    SDL_VERSION(&wmi.version);
    if (!SDL_GetWindowWMInfo(static_cast<SDL_Window*>(sdl_win), &wmi)) {
        std::fprintf(stderr, "[GFX] SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        return false;
    }
    g_hwnd = wmi.info.win.window;
    g_cfg = settings;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = static_cast<UINT>(g_cfg.width);
    sd.BufferDesc.Height = static_cast<UINT>(g_cfg.height);
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    D3D_FEATURE_LEVEL fl_out = D3D_FEATURE_LEVEL_11_0;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                               fls, 2, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &fl_out, &g_ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                           fls, 2, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &fl_out, &g_ctx);
        if (FAILED(hr)) return false;
        std::printf("[GFX] D3D11 WARP fallback active.\n");
    }

    if (!create_targets(g_cfg.width, g_cfg.height)) return false;
    nv2a_shaders_init(g_dev, false, g_cfg.cache_dir);
    init_quad_and_white();

    auto mk_buf = [](UINT bytes, UINT bind) -> ID3D11Buffer* {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (bytes + 15u) & ~15u;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = bind;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Buffer* b = nullptr;
        g_dev->CreateBuffer(&bd, nullptr, &b);
        return b;
    };
    g_dyn_vb     = mk_buf(DYN_VB_BYTES, D3D11_BIND_VERTEX_BUFFER);
    g_dyn_ib     = mk_buf(DYN_IB_BYTES, D3D11_BIND_INDEX_BUFFER);
    g_cb_vs_nv2a = mk_buf(192 * 16, D3D11_BIND_CONSTANT_BUFFER);
    g_cb_vs_scr  = mk_buf(32, D3D11_BIND_CONSTANT_BUFFER);
    g_cb_ff_vs   = mk_buf(640, D3D11_BIND_CONSTANT_BUFFER);
    g_cb_ps_nv2a = mk_buf(352, D3D11_BIND_CONSTANT_BUFFER);
    g_cb_ff_ps   = mk_buf(112, D3D11_BIND_CONSTANT_BUFFER);

    ui_set_active_backend_name("Direct3D 11 (Windows Hardware)");
    ui_init_dx11(sdl_win, g_dev, g_ctx);
    g_dx_owner_tid = std::this_thread::get_id();

    std::printf("[GFX] D3D11 backend ready (%dx%d, MSAA=%dx, AF=%dx, Widescreen=%s).\n",
                g_cfg.width, g_cfg.height, g_cfg.msaa_4x ? 4 : 1, g_cfg.aniso_16x ? 16 : 1,
                g_cfg.widescreen ? "16:9" : "4:3");
    return true;
}

void d3d11_shutdown() {
    drain_retired_dx_resources();
    ui_shutdown();
    nv2a_shaders_shutdown();
    for (auto& kv : g_blend_cache) if (kv.second) kv.second->Release();
    for (auto& kv : g_ds_cache) if (kv.second) kv.second->Release();
    for (auto& kv : g_rs_cache) if (kv.second) kv.second->Release();
    for (auto& kv : g_samp_cache) if (kv.second) kv.second->Release();
    for (auto& kv : g_static_vbs) if (kv.second.buf) kv.second.buf->Release();
    for (auto& kv : g_static_ibs) if (kv.second.buf) kv.second.buf->Release();
    g_blend_cache.clear(); g_ds_cache.clear(); g_rs_cache.clear(); g_samp_cache.clear();
    g_static_vbs.clear(); g_static_ibs.clear();
    if (g_bink_srv) { g_bink_srv->Release(); g_bink_srv = nullptr; }
    if (g_bink_tex) { g_bink_tex->Release(); g_bink_tex = nullptr; }
    if (g_white_srv) { g_white_srv->Release(); g_white_srv = nullptr; }
    if (g_quad_vb) { g_quad_vb->Release(); g_quad_vb = nullptr; }
    if (g_quad_il) { g_quad_il->Release(); g_quad_il = nullptr; }
    if (g_quad_vs) { g_quad_vs->Release(); g_quad_vs = nullptr; }
    if (g_blit_ps) { g_blit_ps->Release(); g_blit_ps = nullptr; }
    if (g_post_ps) { g_post_ps->Release(); g_post_ps = nullptr; }
    if (g_dyn_vb) { g_dyn_vb->Release(); g_dyn_vb = nullptr; }
    if (g_dyn_ib) { g_dyn_ib->Release(); g_dyn_ib = nullptr; }
    if (g_cb_vs_nv2a) { g_cb_vs_nv2a->Release(); g_cb_vs_nv2a = nullptr; }
    if (g_cb_vs_scr) { g_cb_vs_scr->Release(); g_cb_vs_scr = nullptr; }
    if (g_cb_ff_vs) { g_cb_ff_vs->Release(); g_cb_ff_vs = nullptr; }
    if (g_cb_ps_nv2a) { g_cb_ps_nv2a->Release(); g_cb_ps_nv2a = nullptr; }
    if (g_cb_ff_ps) { g_cb_ff_ps->Release(); g_cb_ff_ps = nullptr; }
    release_targets();
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_sc) { g_sc->Release(); g_sc = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

static void apply_pending_dx_resize() {
    int pw = g_pending_resize_w.exchange(0);
    int ph = g_pending_resize_h.exchange(0);
    if (pw > 0 && ph > 0 && g_sc) {
        g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        release_targets();
        g_sc->ResizeBuffers(2, static_cast<UINT>(pw), static_cast<UINT>(ph), DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        create_targets(pw, ph);
    }
}

void d3d11_resize(int width, int height) {
    if (!g_sc || width <= 0 || height <= 0) return;
    if (g_dx_owner_tid != std::thread::id{} && g_dx_owner_tid != std::this_thread::get_id()) {
        g_pending_resize_w.store(width);
        g_pending_resize_h.store(height);
        return;
    }
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    release_targets();
    g_sc->ResizeBuffers(2, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    create_targets(width, height);
}

void d3d11_set_fullscreen(bool fs) {
    if (g_sc) g_sc->SetFullscreenState(fs ? TRUE : FALSE, nullptr);
}

ID3D11Device* d3d11_device() { return g_dev; }
ID3D11DeviceContext* d3d11_context() { return g_ctx; }
RendererSettings& d3d11_settings() { return g_cfg; }
double d3d11_get_fps() { return g_fps; }
int d3d11_get_last_draw_calls() { return g_last_draw_calls; }

void d3d11_clear(uint32_t flags, uint32_t color_argb, float z, uint32_t stencil, GpuTextureObj* custom_rt) {
    if (!g_ctx) return;
    if (!custom_rt) ++g_clear_calls;
    else ensure_dx_rt(custom_rt);

    float c[4] = {
        ((color_argb >> 16) & 0xFF) / 255.0f,
        ((color_argb >>  8) & 0xFF) / 255.0f,
        ((color_argb >>  0) & 0xFF) / 255.0f,
        ((color_argb >> 24) & 0xFF) / 255.0f,
    };
    ID3D11RenderTargetView* rtv = (custom_rt && custom_rt->dx_rtv)
        ? static_cast<ID3D11RenderTargetView*>(custom_rt->dx_rtv)
        : g_msaa_rtv;
    ID3D11DepthStencilView* dsv = custom_rt
        ? static_cast<ID3D11DepthStencilView*>(custom_rt->dx_dsv)
        : g_depth_dsv;
    if ((flags & 1u) && rtv) g_ctx->ClearRenderTargetView(rtv, c);
    if ((flags & 6u) && dsv) {
        UINT df = 0;
        if (flags & 2u) df |= D3D11_CLEAR_DEPTH;
        if (flags & 4u) df |= D3D11_CLEAR_STENCIL;
        g_ctx->ClearDepthStencilView(dsv, df, std::clamp(z, 0.0f, 1.0f), static_cast<UINT8>(stencil));
    }
}

void d3d11_upload_texture(GpuTextureObj* t, uint32_t palette_va) {
    if (!g_dev || !g_ctx || !t || !t->width || !t->height) return;
    if (!t->data_va || !mem_ptr(t->data_va)) return;
    const uint8_t* src_base = static_cast<const uint8_t*>(mem_ptr(t->data_va));

    uint32_t f = t->xb_format;
    bool is_dxt = xb_is_dxt_format(f);
    bool is_lin = xb_is_linear_format(f);
    bool is_swz = !is_dxt && !is_lin;

    DXGI_FORMAT dx_fmt = is_dxt
        ? (f == 0x0Cu ? DXGI_FORMAT_BC1_UNORM : f == 0x0Eu ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM)
        : DXGI_FORMAT_B8G8R8A8_UNORM;

    uint32_t mips = std::max<uint32_t>(1, t->levels);
    uint32_t faces = t->is_cube ? 6u : 1u;
    if (!t->dx_tex) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = t->width;
        td.Height = t->height;
        td.MipLevels = mips;
        td.ArraySize = faces;
        td.Format = dx_fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (t->is_render_target && !is_dxt ? D3D11_BIND_RENDER_TARGET : 0u);
        td.MiscFlags = t->is_cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &tex))) return;
        t->dx_tex = tex;

        ID3D11ShaderResourceView* srv = nullptr;
        g_dev->CreateShaderResourceView(tex, nullptr, &srv);
        t->dx_srv = srv;

        if ((td.BindFlags & D3D11_BIND_RENDER_TARGET) && !t->is_cube) {
            ID3D11RenderTargetView* rtv = nullptr;
            g_dev->CreateRenderTargetView(tex, nullptr, &rtv);
            t->dx_rtv = rtv;
        }
    }

    auto* dx_tex = static_cast<ID3D11Texture2D*>(t->dx_tex);
    const uint32_t* pal = (palette_va && mem_ptr(palette_va)) ? reinterpret_cast<const uint32_t*>(mem_ptr(palette_va)) : nullptr;
    size_t src_off = 0;

    for (uint32_t face = 0; face < faces; ++face) {
        uint32_t mw = t->width, mh = t->height;
        for (uint32_t lvl = 0; lvl < mips; ++lvl) {
            UINT subres = D3D11CalcSubresource(lvl, face, mips);
            if (is_dxt) {
                uint32_t bw = std::max<uint32_t>(1, (mw + 3u) / 4u);
                uint32_t bh = std::max<uint32_t>(1, (mh + 3u) / 4u);
                uint32_t bpb = (f == 0x0Cu) ? 8u : 16u;
                g_ctx->UpdateSubresource(dx_tex, subres, nullptr, src_base + src_off, bw * bpb, 0);
                src_off += size_t(bw) * bh * bpb;
            } else {
                size_t consumed = 0;
                xb_decode_mip_to_bgra(src_base + src_off, mw, mh, f, (lvl == 0 ? t->pitch : 0u), pal, g_scratch_rgba, consumed);
                g_ctx->UpdateSubresource(dx_tex, subres, nullptr, g_scratch_rgba.data(), mw * 4u, 0);
                src_off += consumed;
            }
            if (is_swz && t->is_cube) {
                // Align to 4-byte or 64-byte alignment matching CreateTexture2
            }
            mw = std::max<uint32_t>(1, mw >> 1);
            mh = std::max<uint32_t>(1, mh >> 1);
        }
        src_off = (src_off + 63u) & ~63ull;
    }
    t->dirty = false;
}

void d3d11_draw(const DrawStateSnapshot& st) {
    if (!g_ctx) return;
    auto* dx_vs = st.vs ? static_cast<ID3D11VertexShader*>(st.vs->dx_vs) : nullptr;
    auto* dx_il = st.vs ? static_cast<ID3D11InputLayout*>(st.vs->dx_il) : nullptr;
    auto* dx_ps = st.ps ? static_cast<ID3D11PixelShader*>(st.ps->dx_ps) : nullptr;
    if (!dx_vs || !dx_il || !dx_ps) {
        ++g_draw_skipped_shader;
        return;
    }
    ++g_draw_calls;

    if (st.custom_rt) ensure_dx_rt(st.custom_rt);
    ID3D11RenderTargetView* rtv = (st.custom_rt && st.custom_rt->dx_rtv)
        ? static_cast<ID3D11RenderTargetView*>(st.custom_rt->dx_rtv) : g_msaa_rtv;
    ID3D11DepthStencilView* dsv = (st.custom_rt)
        ? static_cast<ID3D11DepthStencilView*>(st.custom_rt->dx_dsv) : g_depth_dsv;
    g_ctx->OMSetRenderTargets(1, &rtv, dsv);

    float rt_w = (st.custom_rt) ? static_cast<float>(st.custom_rt->width)  : static_cast<float>(g_cfg.width);
    float rt_h = (st.custom_rt) ? static_cast<float>(st.custom_rt->height) : static_cast<float>(g_cfg.height);
    float sx = rt_w / std::max(1.0f, st.xb_fb_w);
    float sy = rt_h / std::max(1.0f, st.xb_fb_h);
    D3D11_VIEWPORT vp{st.vp_x * sx, st.vp_y * sy, st.vp_w * sx, st.vp_h * sy, st.vp_minz, st.vp_maxz};
    if (vp.Width < 1) vp.Width = rt_w;
    if (vp.Height < 1) vp.Height = rt_h;
    g_ctx->RSSetViewports(1, &vp);

    g_ctx->OMSetBlendState(get_blend_state(st.alpha_blend, st.src_blend, st.dst_blend, st.blend_op, st.color_write_mask), nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(get_ds_state(
        st.z_enable, st.z_write, st.z_func,
        st.stencil_enable, st.stencil_func, st.stencil_fail, st.stencil_zfail, st.stencil_pass,
        static_cast<uint8_t>(st.stencil_mask & 0xFFu), static_cast<uint8_t>(st.stencil_writemask & 0xFFu)),
        st.stencil_ref & 0xFFu);
    g_ctx->RSSetState(get_rs_state(st.cull_mode, st.fill_mode));

    ID3D11ShaderResourceView* srvs[4]{};
    ID3D11SamplerState* samps[4]{};
    for (int s = 0; s < 4; ++s) {
        if (st.tex[s]) {
            if (st.tex[s]->dirty && !st.tex[s]->is_render_target) {
                d3d11_upload_texture(st.tex[s], st.current_palette_va);
            } else if (!st.tex[s]->dx_srv) {
                if (st.tex[s]->is_render_target) ensure_dx_rt(st.tex[s]);
                else d3d11_upload_texture(st.tex[s], st.current_palette_va);
            }
            srvs[s] = st.tex[s]->dx_srv ? static_cast<ID3D11ShaderResourceView*>(st.tex[s]->dx_srv) : g_white_srv;
        } else {
            srvs[s] = g_white_srv;
        }
        samps[s] = get_sampler(st.addr_u[s], st.addr_v[s], st.min_filter[s], st.mag_filter[s]);
    }
    g_ctx->PSSetShaderResources(0, 4, srvs);
    g_ctx->PSSetSamplers(0, 4, samps);

    GfxTransformParams tp = gfx_compute_transform_params(st, rt_w, rt_h, g_cfg.widescreen, g_cfg.unstretched_hud);

    if (st.is_programmable_vs && st.vs_consts) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_ctx->Map(g_cb_vs_nv2a, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, st.vs_consts, 192 * 16);
            g_ctx->Unmap(g_cb_vs_nv2a, 0);
        }
        if (SUCCEEDED(g_ctx->Map(g_cb_vs_scr, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float sc[8] = {tp.ss_w, tp.ss_h, tp.scale_x, st.vs->is_screenspace ? 1.0f : 0.0f, 1, 0, 0, 0};
            std::memcpy(m.pData, sc, 32);
            g_ctx->Unmap(g_cb_vs_scr, 0);
        }
        ID3D11Buffer* cbs[2] = {g_cb_vs_nv2a, g_cb_vs_scr};
        g_ctx->VSSetConstantBuffers(0, 2, cbs);
    } else {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_ctx->Map(g_cb_ff_vs, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float* d = static_cast<float*>(m.pData);
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
            d[149] = st.fog_start;
            d[150] = st.fog_end;
            d[151] = st.fog_density;
            d[152] = tp.ss_w;
            d[153] = tp.ss_h;
            d[154] = tp.scale_x;
            d[155] = st.lighting_en ? 1.0f : 0.0f;
            d[156] = st.specular_en ? 1.0f : 0.0f;
            d[157] = 1.0f;
            d[158] = 1.0f;
            d[159] = static_cast<float>(st.fog_mode);
            g_ctx->Unmap(g_cb_ff_vs, 0);
        }
        g_ctx->VSSetConstantBuffers(0, 1, &g_cb_ff_vs);
    }

    float tex_scale[16]{};
    for (int s = 0; s < 4; ++s) {
        uint32_t f = st.tex[s] ? st.tex[s]->xb_format : 0u;
        bool is_lin = xb_is_linear_format(f);
        if (st.tex[s] && is_lin && st.tex[s]->width > 0 && st.tex[s]->height > 0) {
            tex_scale[s * 4 + 0] = 1.0f / static_cast<float>(st.tex[s]->width);
            tex_scale[s * 4 + 1] = 1.0f / static_cast<float>(st.tex[s]->height);
        } else {
            tex_scale[s * 4 + 0] = 1.0f;
            tex_scale[s * 4 + 1] = 1.0f;
        }
        tex_scale[s * 4 + 2] = 1.0f;
        tex_scale[s * 4 + 3] = 1.0f;
    }

    if (st.is_programmable_ps) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_ctx->Map(g_cb_ps_nv2a, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float* p = static_cast<float*>(m.pData);
            if (st.ps_consts) std::memcpy(p, st.ps_consts, 16 * 16);
            else std::memset(p, 0, 16 * 16);
            p[64] = st.alpha_test ? 1.0f : 0.0f;
            p[65] = st.alpha_ref;
            p[66] = static_cast<float>(st.alpha_func);
            p[67] = st.fog_enable ? 1.0f : 0.0f;
            std::memcpy(p + 68, st.fog_color, 16);
            std::memcpy(p + 72, tex_scale, sizeof(tex_scale));
            g_ctx->Unmap(g_cb_ps_nv2a, 0);
        }
        g_ctx->PSSetConstantBuffers(0, 1, &g_cb_ps_nv2a);
    } else {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_ctx->Map(g_cb_ff_ps, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float* p = static_cast<float*>(m.pData);
            std::memcpy(p + 0, st.tex_factor, 16);
            p[4] = st.alpha_test ? 1.0f : 0.0f;
            p[5] = st.alpha_ref;
            p[6] = static_cast<float>(st.alpha_func);
            p[7] = st.fog_enable ? 1.0f : 0.0f;
            std::memcpy(p + 8, st.fog_color, 16);
            std::memcpy(p + 12, tex_scale, sizeof(tex_scale));
            g_ctx->Unmap(g_cb_ff_ps, 0);
        }
        g_ctx->PSSetConstantBuffers(0, 1, &g_cb_ff_ps);
    }

    g_ctx->IASetInputLayout(dx_il);
    g_ctx->VSSetShader(dx_vs, nullptr, 0);
    g_ctx->PSSetShader(dx_ps, nullptr, 0);

    uint32_t vcnt = st.vertex_count;
    if (st.ib_ptr && st.index_count > 0) {
        uint16_t max_idx = 0;
        for (uint32_t i = 0; i < st.index_count; ++i) max_idx = std::max(max_idx, st.ib_ptr[i]);
        vcnt = static_cast<uint32_t>(max_idx) + 1u + static_cast<uint32_t>(std::max(0, st.base_vertex));
    }
    if (vcnt == 0) { ++g_draw_skipped_vert; return; }

    ID3D11Buffer* vbs[4]{};
    UINT strides[4]{}, offsets[4]{};
    for (int s = 0; s < 4; ++s) {
        if (!st.vb_ptrs[s] || !st.vb_strides[s]) continue;
        // Fast static GPU buffer cache for guest VBs!
        if (st.vb_guest_va[s] && st.vb_total_bytes[s] > 0 && st.vb_total_bytes[s] <= DYN_VB_BYTES) {
            auto& entry = g_static_vbs[st.vb_guest_va[s]];
            if (!entry.buf || entry.bytes < st.vb_total_bytes[s]) {
                if (entry.buf) entry.buf->Release();
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = (st.vb_total_bytes[s] + 15u) & ~15u;
                bd.Usage = D3D11_USAGE_DEFAULT;
                bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                g_dev->CreateBuffer(&bd, nullptr, &entry.buf);
                entry.bytes = st.vb_total_bytes[s];
                entry.seq = 0;
            }
            if (entry.buf) {
                if (entry.seq != st.vb_dirty_seq[s]) {
                    g_ctx->UpdateSubresource(entry.buf, 0, nullptr, st.vb_ptrs[s], 0, 0);
                    entry.seq = st.vb_dirty_seq[s];
                }
                vbs[s] = entry.buf;
                strides[s] = st.vb_strides[s];
                offsets[s] = st.vb_offsets[s];
                continue;
            }
        }
        UINT sz = vcnt * st.vb_strides[s];
        if (sz > DYN_VB_BYTES) sz = DYN_VB_BYTES;
        bool disc = (g_dyn_vb_off + sz > DYN_VB_BYTES);
        if (disc) g_dyn_vb_off = 0;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_ctx->Map(g_dyn_vb, 0, disc ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m))) {
            std::memcpy(static_cast<uint8_t*>(m.pData) + g_dyn_vb_off, st.vb_ptrs[s], sz);
            g_ctx->Unmap(g_dyn_vb, 0);
        }
        vbs[s] = g_dyn_vb;
        strides[s] = st.vb_strides[s];
        offsets[s] = g_dyn_vb_off;
        g_dyn_vb_off = (g_dyn_vb_off + sz + 15u) & ~15u;
    }
    g_ctx->IASetVertexBuffers(0, 4, vbs, strides, offsets);

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
        UINT ib_sz = static_cast<UINT>(g_scratch_tris.size() * 2);
        bool disc = (g_dyn_ib_off + ib_sz > DYN_IB_BYTES);
        if (disc) g_dyn_ib_off = 0;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_ctx->Map(g_dyn_ib, 0, disc ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m))) {
            std::memcpy(static_cast<uint8_t*>(m.pData) + g_dyn_ib_off, g_scratch_tris.data(), ib_sz);
            g_ctx->Unmap(g_dyn_ib, 0);
        }
        g_ctx->IASetIndexBuffer(g_dyn_ib, DXGI_FORMAT_R16_UINT, g_dyn_ib_off);
        g_dyn_ib_off = (g_dyn_ib_off + ib_sz + 15u) & ~15u;
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->DrawIndexed(static_cast<UINT>(g_scratch_tris.size()), 0, st.base_vertex);
        return;
    }

    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    if (st.prim_type == 1) topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    else if (st.prim_type == 2) topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    else if (st.prim_type == 4 || st.prim_type == 3) topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    else if (st.prim_type == 6) topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    g_ctx->IASetPrimitiveTopology(topo);

    if (indexed) {
        UINT ib_sz = n * 2;
        bool disc = (g_dyn_ib_off + ib_sz > DYN_IB_BYTES);
        if (disc) g_dyn_ib_off = 0;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_ctx->Map(g_dyn_ib, 0, disc ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m))) {
            std::memcpy(static_cast<uint8_t*>(m.pData) + g_dyn_ib_off, st.ib_ptr, ib_sz);
            g_ctx->Unmap(g_dyn_ib, 0);
        }
        g_ctx->IASetIndexBuffer(g_dyn_ib, DXGI_FORMAT_R16_UINT, g_dyn_ib_off);
        g_dyn_ib_off = (g_dyn_ib_off + ib_sz + 15u) & ~15u;
        g_ctx->DrawIndexed(n, 0, st.base_vertex);
    } else {
        g_ctx->Draw(n, 0);
    }
}

void d3d11_blit_rgba_rect(const void* bgra, int src_w, int src_h, int dst_x, int dst_y, int dst_w, int dst_h) {
    if (!g_dev || !g_ctx || !bgra || src_w <= 0 || src_h <= 0) return;
    if (!g_bink_tex || g_bink_w != src_w || g_bink_h != src_h) {
        if (g_bink_srv) g_bink_srv->Release();
        if (g_bink_tex) g_bink_tex->Release();
        g_bink_srv = nullptr; g_bink_tex = nullptr;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(src_w); td.Height = static_cast<UINT>(src_h);
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_bink_tex))) {
            g_dev->CreateShaderResourceView(g_bink_tex, nullptr, &g_bink_srv);
            g_bink_w = src_w; g_bink_h = src_h;
        }
    }
    if (!g_bink_tex || !g_bink_srv) return;
    g_ctx->UpdateSubresource(g_bink_tex, 0, nullptr, bgra, static_cast<UINT>(src_w * 4), 0);

    float blk[4] = {0, 0, 0, 1};
    g_ctx->ClearRenderTargetView(g_back_rtv, blk);
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
    std::vector<QuadVert> v{
        {x0, y0, 0, 0, 0xFFFFFFFFu}, {x1, y0, 1, 0, 0xFFFFFFFFu}, {x0, y1, 0, 1, 0xFFFFFFFFu},
        {x1, y0, 1, 0, 0xFFFFFFFFu}, {x1, y1, 1, 1, 0xFFFFFFFFu}, {x0, y1, 0, 1, 0xFFFFFFFFu}
    };
    draw_2d_quads(v, g_bink_srv);
}

void d3d11_present() {
    if (!g_sc || !g_ctx) return;
    g_dx_owner_tid = std::this_thread::get_id();
    drain_retired_dx_resources();
    apply_pending_dx_resize();

    ++g_frame_count;
    ++g_fps_frames;
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - g_fps_t0).count();
    if (dt >= 0.5) {
        g_fps = g_fps_frames / dt;
        g_fps_frames = 0;
        g_fps_t0 = now;
    }

    if (ui_is_launcher_active()) {
        float bg[4] = {0.05f, 0.06f, 0.09f, 1.0f};
        g_ctx->OMSetRenderTargets(1, &g_back_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_back_rtv, bg);
    } else if (g_msaa_color && g_resolve_tex && (g_draw_calls > 0 || g_clear_calls > 0)) {
        if (g_cfg.msaa_4x) {
            g_ctx->ResolveSubresource(g_resolve_tex, 0, g_msaa_color, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
        } else {
            g_ctx->CopyResource(g_resolve_tex, g_msaa_color);
        }
        QuadVert fsq[6] = {
            {-1,  1, 0, 0, 0xFFFFFFFFu}, { 1,  1, 1, 0, 0xFFFFFFFFu}, {-1, -1, 0, 1, 0xFFFFFFFFu},
            { 1,  1, 1, 0, 0xFFFFFFFFu}, { 1, -1, 1, 1, 0xFFFFFFFFu}, {-1, -1, 0, 1, 0xFFFFFFFFu}
        };
        D3D11_VIEWPORT vp{0, 0, static_cast<float>(g_cfg.width), static_cast<float>(g_cfg.height), 0, 1};
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->OMSetRenderTargets(1, &g_back_rtv, nullptr);
        g_ctx->OMSetDepthStencilState(get_ds_state(false, false, 8, false, 8, 1, 1, 1, 0xFF, 0xFF), 0);
        g_ctx->OMSetBlendState(get_blend_state(false, 2, 1, 1, 0xF), nullptr, 0xFFFFFFFF);
        g_ctx->RSSetState(get_rs_state(1, 3));
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g_ctx->Map(g_quad_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, fsq, sizeof(fsq));
            g_ctx->Unmap(g_quad_vb, 0);
        }
        UINT st_sz = sizeof(QuadVert), off = 0;
        g_ctx->IASetInputLayout(g_quad_il);
        g_ctx->IASetVertexBuffers(0, 1, &g_quad_vb, &st_sz, &off);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(g_quad_vs, nullptr, 0);
        g_ctx->PSSetShader((g_cfg.bloom_sharpen && g_post_ps) ? g_post_ps : g_blit_ps, nullptr, 0);
        g_ctx->PSSetShaderResources(0, 1, &g_resolve_srv);
        ID3D11SamplerState* ss = get_sampler(3, 3, 2, 2);
        g_ctx->PSSetSamplers(0, 1, &ss);
        g_ctx->Draw(6, 0);
    }

    // Bind backbuffer and render Dear ImGui Launcher or F1 Overlay
    g_ctx->OMSetRenderTargets(1, &g_back_rtv, nullptr);
    ui_render_frame(false);

    if (!g_pending_screenshot.empty()) {
        save_backbuffer_png(g_pending_screenshot);
        g_pending_screenshot.clear();
    }

    g_last_draw_calls = g_draw_calls;
    if (!ui_is_launcher_active() &&
        (g_frame_count == 1 || g_frame_count == 15 || g_frame_count == 60 || g_frame_count == 180 || g_frame_count == 360)) {
        std::printf("[GFX] Frame %d: clears=%d draws=%d skip_sh=%d skip_v=%d\n",
                    g_frame_count, g_clear_calls, g_draw_calls, g_draw_skipped_shader, g_draw_skipped_vert);
    }

    g_sc->Present(g_cfg.vsync ? 1 : 0, 0);
    g_draw_calls = 0;
    g_clear_calls = 0;
    g_draw_skipped_shader = 0;
    g_draw_skipped_vert = 0;

    gfx_pace_frame(g_cfg.fps_cap);
}

void d3d11_request_screenshot(const std::string& filename) {
    g_pending_screenshot = filename;
}

void d3d11_retire_texture(GpuTextureObj* tex) {
    if (!tex) return;
    std::lock_guard<std::mutex> lk(g_retire_mtx);
    g_retired_textures.push_back(tex);
}

void d3d11_retire_buffer(uint32_t obj_va) {
    if (!obj_va) return;
    std::lock_guard<std::mutex> lk(g_retire_mtx);
    g_retired_buffers.push_back(obj_va);
}

void* d3d11_create_ui_texture_rgba(const uint8_t* rgba, int w, int h) {
    if (!g_dev || !rgba || w <= 0 || h <= 0) return nullptr;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = rgba;
    init.SysMemPitch = static_cast<UINT>(w * 4);
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&td, &init, &tex)) || !tex) return nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = g_dev->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    return SUCCEEDED(hr) ? static_cast<void*>(srv) : nullptr;
}

void d3d11_destroy_ui_texture(void* tex_handle) {
    if (!tex_handle) return;
    static_cast<ID3D11ShaderResourceView*>(tex_handle)->Release();
}

#endif // _WIN32
