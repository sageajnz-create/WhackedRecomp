#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#endif
#include "nv2a_shaders.h"
#include "gl_loader.h"
#include "xbox_mem.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
ID3D11Device* g_dx_dev = nullptr;
#endif
bool g_gl_mode = false;
std::string g_cache_dir;
std::mutex g_sh_mtx;
std::unordered_map<uint64_t, Nv2aCompiledVS> g_vs_cache;
std::unordered_map<uint64_t, Nv2aCompiledPS> g_ps_cache;

uint64_t fnv1a_bytes(const void* data, size_t len, uint64_t seed = 0xcbf29ce484222325ull) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

uint32_t vsdt_byte_size(uint8_t type) {
    static const uint8_t counts[8]  = {0, 1, 2, 3, 4, 0, 0, 3};
    static const uint8_t elem_sz[16] = {1, 2, 4, 0, 1, 2, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    return static_cast<uint32_t>(counts[(type >> 4) & 7]) * static_cast<uint32_t>(elem_sz[type & 0xF]);
}

std::string mask_to_str(uint32_t m) {
    std::string r;
    if (m & 8) r += 'x';
    if (m & 4) r += 'y';
    if (m & 2) r += 'z';
    if (m & 1) r += 'w';
    return r;
}

std::string swz_to_str(uint32_t s) {
    static const char ch[4] = {'x', 'y', 'z', 'w'};
    std::string r;
    r += ch[(s >> 6) & 3];
    r += ch[(s >> 4) & 3];
    r += ch[(s >> 2) & 3];
    r += ch[s & 3];
    return r;
}

#ifdef _WIN32
DXGI_FORMAT vsdt_to_dxgi(uint8_t type) {
    switch (type) {
        case 0x12: return DXGI_FORMAT_R32_FLOAT;
        case 0x22: return DXGI_FORMAT_R32G32_FLOAT;
        case 0x32: return DXGI_FORMAT_R32G32B32_FLOAT;
        case 0x42: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case 0x40: return DXGI_FORMAT_B8G8R8A8_UNORM;     // D3DVSDT_D3DCOLOR
        case 0x41: return DXGI_FORMAT_R8G8B8A8_UINT;      // UBYTE4
        case 0x11: return DXGI_FORMAT_R16_SINT;
        case 0x21: return DXGI_FORMAT_R16G16_SINT;
        case 0x45: return DXGI_FORMAT_R16G16B16A16_SNORM; // NORMSHORT4
        case 0x25: return DXGI_FORMAT_R16G16_SNORM;       // NORMSHORT2
        case 0x44: return DXGI_FORMAT_R8G8B8A8_UNORM;     // PBYTE4
        case 0x34: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case 0x16: return DXGI_FORMAT_R32_UINT;           // NORMPACKED3
        default:   return DXGI_FORMAT_R32G32B32A32_FLOAT;
    }
}

std::string cache_file_path(const char* prefix, uint64_t key) {
    if (g_cache_dir.empty()) return {};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s_%016llX.dxbc", prefix, static_cast<unsigned long long>(key));
    return (fs::path(g_cache_dir) / buf).string();
}

bool load_cached_dxbc(const char* prefix, uint64_t key, std::vector<uint8_t>& out_bytes) {
    std::string p = cache_file_path(prefix, key);
    if (p.empty()) return false;
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    std::streamsize sz = f.tellg();
    if (sz <= 16) return false;
    f.seekg(0);
    out_bytes.resize(static_cast<size_t>(sz));
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out_bytes.data()), sz));
}

void save_cached_dxbc(const char* prefix, uint64_t key, const void* data, size_t sz) {
    std::string p = cache_file_path(prefix, key);
    if (p.empty() || !data || sz == 0) return;
    std::error_code ec;
    fs::create_directories(g_cache_dir, ec);
    std::ofstream f(p, std::ios::binary);
    if (f.is_open()) f.write(static_cast<const char*>(data), static_cast<std::streamsize>(sz));
}

bool compile_hlsl(const std::string& src, const char* entry, const char* target,
                  const char* cache_prefix, uint64_t key, std::vector<uint8_t>& out_bytecode) {
    if (load_cached_dxbc(cache_prefix, key, out_bytecode)) return true;
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &blob, &err);
    if (FAILED(hr) || !blob) {
        std::fprintf(stderr, "[SHADER ERR %s/%s]: %s\n--- SOURCE ---\n%s\n",
                     entry, target, err ? static_cast<const char*>(err->GetBufferPointer()) : "unknown",
                     src.c_str());
        if (err) err->Release();
        if (blob) blob->Release();
        return false;
    }
    if (err) err->Release();
    out_bytecode.resize(blob->GetBufferSize());
    std::memcpy(out_bytecode.data(), blob->GetBufferPointer(), out_bytecode.size());
    blob->Release();
    save_cached_dxbc(cache_prefix, key, out_bytecode.data(), out_bytecode.size());
    return true;
}

ID3D11InputLayout* build_dx_input_layout(const Nv2aVertexDecl& decl, const void* vs_bytecode, size_t sz) {
    if (!g_dx_dev || !vs_bytecode || !sz) return nullptr;
    std::vector<D3D11_INPUT_ELEMENT_DESC> descs;
    for (const auto& e : decl.elements) {
        if (e.reg >= 16) continue;
        D3D11_INPUT_ELEMENT_DESC d{};
        d.SemanticName         = "TEXCOORD";
        d.SemanticIndex        = e.reg;
        d.Format               = vsdt_to_dxgi(e.type);
        d.InputSlot            = e.stream;
        d.AlignedByteOffset    = e.offset;
        d.InputSlotClass       = D3D11_INPUT_PER_VERTEX_DATA;
        d.InstanceDataStepRate = 0;
        descs.push_back(d);
    }
    if (descs.empty()) return nullptr;
    ID3D11InputLayout* il = nullptr;
    HRESULT hr = g_dx_dev->CreateInputLayout(descs.data(), static_cast<UINT>(descs.size()),
                                             vs_bytecode, sz, &il);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[SHADER] CreateInputLayout failed (0x%08X, elems=%zu)\n",
                     static_cast<uint32_t>(hr), descs.size());
    }
    return il;
}
#endif

uint32_t compile_gl_shader(GLenum stage, const std::string& src) {
    auto& gl = whacked_gl();
    if (!gl.glCreateShader) return 0;
    GLuint sh = gl.glCreateShader(stage);
    const char* p = src.c_str();
    GLint len = static_cast<GLint>(src.size());
    gl.glShaderSource(sh, 1, &p, &len);
    gl.glCompileShader(sh);
    GLint ok = 0;
    gl.glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]{};
        gl.glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[GL SHADER ERR stage=0x%X]: %s\n--- SOURCE ---\n%s\n", stage, log, src.c_str());
        gl.glDeleteShader(sh);
        return 0;
    }
    return sh;
}

std::string decl_vs_input_struct(const Nv2aVertexDecl& decl, bool glsl, bool* out_has_reg = nullptr) {
    bool has[16]{};
    uint8_t types[16]{};
    for (const auto& e : decl.elements) {
        if (e.reg < 16) { has[e.reg] = true; types[e.reg] = e.type; }
    }
    if (out_has_reg) std::memcpy(out_has_reg, has, 16);

    std::ostringstream ss;
    if (glsl) {
        for (int i = 0; i < 16; ++i) {
            if (!has[i]) continue;
            if (types[i] == 0x16) {
                ss << "layout(location = " << i << ") in uint vin_v" << i << "_raw;\n";
            } else if (types[i] == 0x41) {
                ss << "layout(location = " << i << ") in uvec4 vin_v" << i << ";\n";
            } else {
                ss << "layout(location = " << i << ") in vec4 vin_v" << i << ";\n";
            }
        }
    } else {
        ss << "struct VS_IN {\n";
        for (int i = 0; i < 16; ++i) {
            if (!has[i]) continue;
            if (types[i] == 0x16) {
                ss << "    uint v" << i << "_raw : TEXCOORD" << i << ";\n";
            } else if (types[i] == 0x41) {
                ss << "    uint4 v" << i << " : TEXCOORD" << i << ";\n";
            } else {
                ss << "    float4 v" << i << " : TEXCOORD" << i << ";\n";
            }
        }
        ss << "};\n";
    }
    return ss.str();
}

void emit_unpack_inputs(std::ostringstream& ss, const Nv2aVertexDecl& decl, const bool has_reg[16], bool glsl) {
    const char* f4 = glsl ? "vec4" : "float4";
    const char* f3 = glsl ? "vec3" : "float3";
    const char* f2 = glsl ? "vec2" : "float2";
    const char* in_pre = glsl ? "vin_v" : "vin.v";

    for (int i = 0; i < 16; ++i) {
        if (!has_reg[i]) {
            ss << "    " << f4 << " v" << i << " = " << f4 << "(0.0,0.0,0.0,1.0);\n";
            continue;
        }
        uint8_t etype = 0;
        for (const auto& e : decl.elements) if (e.reg == i) etype = e.type;
        if (etype == 0x16) {
            ss << "    " << f4 << " v" << i << "; {\n"
               << "        uint p = " << in_pre << i << "_raw;\n"
               << "        int x = int(p << 21u) >> 21;\n"
               << "        int y = int((p >> 11u) << 21u) >> 21;\n"
               << "        int z = int((p >> 22u) << 22u) >> 22;\n"
               << "        v" << i << " = " << f4 << "(float(x) / 1023.0, float(y) / 1023.0, float(z) / 511.0, 1.0);\n"
               << "    }\n";
        } else if (etype == 0x41) {
            ss << "    " << f4 << " v" << i << " = " << f4 << "(" << in_pre << i << ");\n";
        } else {
            ss << "    " << f4 << " v" << i << " = " << in_pre << i << ";\n";
            if (etype == 0x32) ss << "    v" << i << ".w = 1.0;\n";
            else if (etype == 0x22) ss << "    v" << i << ".zw = " << f2 << "(0.0, 1.0);\n";
            else if (etype == 0x12) ss << "    v" << i << ".yzw = " << f3 << "(0.0, 0.0, 1.0);\n";
        }
    }
}

std::string generate_xvu_vs(const Nv2aVertexDecl& decl, const uint8_t* xvu_bytes, uint16_t nins,
                            bool has_viewport_epilogue, bool is_screenspace, bool glsl) {
    const char* f4 = glsl ? "vec4" : "float4";
    auto sat = [&](const std::string& x) {
        return glsl ? ("clamp(" + x + ", 0.0, 1.0)") : ("saturate(" + x + ")");
    };

    bool has_reg[16]{};
    std::ostringstream ss;
    if (glsl) {
        ss << "#version 330 core\n"
           << "layout(std140) uniform NV2AVSConsts { vec4 c[192]; };\n"
           << "layout(std140) uniform NV2AScreenCB { float vpW; float vpH; float widescreenScale; float isSS; float yFlip; float pad0; float pad1; float pad2; };\n";
        ss << decl_vs_input_struct(decl, true, has_reg);
        ss << "out vec4 vs_oD0;\n"
           << "out vec4 vs_oD1;\n"
           << "out vec4 vs_oT0;\n"
           << "out vec4 vs_oT1;\n"
           << "out vec4 vs_oT2;\n"
           << "out vec4 vs_oT3;\n"
           << "out float vs_oFog;\n"
           << "void main() {\n";
    } else {
        ss << "cbuffer NV2AVSConsts : register(b0) { float4 c[192]; };\n"
           << "cbuffer NV2AScreenCB : register(b1) { float vpW; float vpH; float widescreenScale; float isSS; };\n";
        ss << decl_vs_input_struct(decl, false, has_reg);
        ss << "struct VS_OUT {\n"
           << "    float4 oPos  : SV_POSITION;\n"
           << "    float4 oD0   : COLOR0;\n"
           << "    float4 oD1   : COLOR1;\n"
           << "    float4 oT0   : TEXCOORD0;\n"
           << "    float4 oT1   : TEXCOORD1;\n"
           << "    float4 oT2   : TEXCOORD2;\n"
           << "    float4 oT3   : TEXCOORD3;\n"
           << "    float  oFog  : FOG;\n"
           << "};\n"
           << "VS_OUT main(VS_IN vin) {\n";
    }

    emit_unpack_inputs(ss, decl, has_reg, glsl);

    ss << "    " << f4 << " r[13];\n";
    if (!glsl) ss << "    [unroll] ";
    ss << "    for (int ri = 0; ri < 12; ++ri) r[ri] = " << f4 << "(0.0,0.0,0.0,0.0);\n";
    ss << "    r[12] = " << f4 << "(0.0,0.0,0.0,1.0);\n"
       << "    int a0 = 0;\n"
       << "    " << f4 << " oD0 = " << f4 << "(1.0,1.0,1.0,1.0), oD1 = " << f4 << "(0.0,0.0,0.0,1.0);\n"
       << "    " << f4 << " oT0 = " << f4 << "(0.0,0.0,0.0,1.0), oT1 = " << f4 << "(0.0,0.0,0.0,1.0), oT2 = " << f4 << "(0.0,0.0,0.0,1.0), oT3 = " << f4 << "(0.0,0.0,0.0,1.0);\n"
       << "    float oFog = 1.0, oPts = 1.0;\n"
       << "    " << f4 << " tmpMAC = " << f4 << "(0.0,0.0,0.0,0.0), tmpILU = " << f4 << "(0.0,0.0,0.0,0.0);\n";

    for (uint16_t k = 0; k < nins; ++k) {
        uint32_t w1, w2, w3;
        std::memcpy(&w1, xvu_bytes + 4 + k * 16 + 4, 4);
        std::memcpy(&w2, xvu_bytes + 4 + k * 16 + 8, 4);
        std::memcpy(&w3, xvu_bytes + 4 + k * 16 + 12, 4);

        uint32_t ilu  = (w1 >> 25) & 7u;
        uint32_t mac  = (w1 >> 21) & 15u;
        uint32_t cidx = (w1 >> 13) & 0xFFu;
        uint32_t vidx = (w1 >> 9) & 15u;
        uint32_t rel  = (w3 >> 1) & 1u;

        if (has_viewport_epilogue && !rel && (cidx == 58u || cidx == 59u)) continue;

        auto fmt_src = [&](uint32_t neg, uint32_t swz, uint32_t reg, uint32_t mux) -> std::string {
            std::string base;
            if (mux == 1u) base = "r[" + std::to_string(std::min<uint32_t>(reg, 12u)) + "]";
            else if (mux == 2u) base = "v" + std::to_string(vidx);
            else if (mux == 3u) {
                if (rel) base = "c[clamp(a0 + " + std::to_string(cidx) + ", 0, 191)]";
                else base = "c[" + std::to_string(std::min<uint32_t>(cidx, 191u)) + "]";
            } else {
                base = std::string(f4) + "(0.0,0.0,0.0,0.0)";
            }
            std::string expr = "(" + base + "." + swz_to_str(swz) + ")";
            return neg ? ("(-" + expr + ")") : expr;
        };

        std::string a = fmt_src((w1 >> 8) & 1u, w1 & 0xFFu, (w2 >> 28) & 15u, (w2 >> 26) & 3u);
        std::string b = fmt_src((w2 >> 25) & 1u, (w2 >> 17) & 0xFFu, (w2 >> 13) & 15u, (w2 >> 11) & 3u);
        uint32_t s3_reg = ((w2 & 3u) << 2) | ((w3 >> 30) & 3u);
        std::string cc = fmt_src((w2 >> 10) & 1u, (w2 >> 2) & 0xFFu, s3_reg, (w3 >> 28) & 3u);

        uint32_t mac_mask = (w3 >> 24) & 15u;
        uint32_t dst_temp = std::min<uint32_t>((w3 >> 20) & 15u, 12u);
        uint32_t ilu_mask = (w3 >> 16) & 15u;
        uint32_t out_mask = (w3 >> 12) & 15u;
        uint32_t dst_sel  = (w3 >> 11) & 1u;
        uint32_t out_reg  = (w3 >> 3) & 0xFFu;
        uint32_t dst_mux  = (w3 >> 2) & 1u;

        if (mac != 0u) {
            switch (mac) {
                case 1u: ss << "    tmpMAC = " << a << ";\n"; break;
                case 2u: ss << "    tmpMAC = (" << a << " * " << b << ");\n"; break;
                case 3u: ss << "    tmpMAC = (" << a << " + " << cc << ");\n"; break;
                case 4u: ss << "    tmpMAC = (" << a << " * " << b << " + " << cc << ");\n"; break;
                case 5u: ss << "    { float _d = dot(" << a << ".xyz, " << b << ".xyz); tmpMAC = " << f4 << "(_d,_d,_d,_d); }\n"; break;
                case 6u: ss << "    { float _d = dot(" << f4 << "(" << a << ".xyz, 1.0), " << b << "); tmpMAC = " << f4 << "(_d,_d,_d,_d); }\n"; break;
                case 7u: ss << "    { float _d = dot(" << a << ", " << b << "); tmpMAC = " << f4 << "(_d,_d,_d,_d); }\n"; break;
                case 8u: ss << "    { " << f4 << " _a = " << a << ", _b = " << b << "; tmpMAC = " << f4 << "(1.0, _a.y*_b.y, _a.z, _b.w); }\n"; break;
                case 9u: ss << "    tmpMAC = min(" << a << ", " << b << ");\n"; break;
                case 10u: ss << "    tmpMAC = max(" << a << ", " << b << ");\n"; break;
                case 11u:
                    ss << "    tmpMAC = " << f4 << "(" << a << ".x<" << b << ".x?1.0:0.0, " << a << ".y<" << b << ".y?1.0:0.0, "
                       << a << ".z<" << b << ".z?1.0:0.0, " << a << ".w<" << b << ".w?1.0:0.0);\n";
                    break;
                case 12u:
                    ss << "    tmpMAC = " << f4 << "(" << a << ".x>=" << b << ".x?1.0:0.0, " << a << ".y>=" << b << ".y?1.0:0.0, "
                       << a << ".z>=" << b << ".z?1.0:0.0, " << a << ".w>=" << b << ".w?1.0:0.0);\n";
                    break;
                case 13u: ss << "    a0 = int(floor(" << a << ".x));\n"; break;
                default: break;
            }
        }
        if (ilu != 0u) {
            const char* rsq = glsl ? "inversesqrt" : "rsqrt";
            switch (ilu) {
                case 1u: ss << "    tmpILU = " << cc << ";\n"; break;
                case 2u: ss << "    { float _v = (" << cc << ").w; float _r = (_v==0.0)?1e10:(1.0/_v); tmpILU = " << f4 << "(_r,_r,_r,_r); }\n"; break;
                case 3u: ss << "    { float _v = (" << cc << ").w; float _r = (abs(_v)<1e-6)?1.0:(1.0/_v); tmpILU = " << f4 << "(_r,_r,_r,_r); }\n"; break;
                case 4u: ss << "    { float _v = max(abs((" << cc << ").w), 1e-10); float _r = " << rsq << "(_v); tmpILU = " << f4 << "(_r,_r,_r,_r); }\n"; break;
                case 5u: ss << "    tmpILU = " << f4 << "(exp2((" << cc << ").w),0.0,0.0,0.0);\n"; break;
                case 6u: ss << "    tmpILU = " << f4 << "(log2(max(abs((" << cc << ").w), 1e-10)),0.0,0.0,0.0);\n"; break;
                case 7u: ss << "    { " << f4 << " _l = " << cc << "; float _m = 128.0; tmpILU = " << f4 << "(1.0, max(_l.x,0.0), (_l.x>0.0)?pow(max(_l.y,0.0), clamp(_l.w,-_m,_m)):0.0, 1.0); }\n"; break;
                default: break;
            }
        }
        if (mac != 0u && mac != 13u && mac_mask != 0u) {
            std::string m = mask_to_str(mac_mask);
            ss << "    r[" << dst_temp << "]." << m << " = tmpMAC." << m << ";\n";
        }
        if (ilu != 0u && ilu_mask != 0u) {
            std::string m = mask_to_str(ilu_mask);
            uint32_t ilu_dst = (mac != 0u) ? 1u : dst_temp;
            ss << "    r[" << ilu_dst << "]." << m << " = tmpILU." << m << ";\n";
        }
        if (out_mask != 0u) {
            std::string m = mask_to_str(out_mask);
            const char* src_v = dst_mux ? "tmpILU" : "tmpMAC";
            if (dst_sel == 0u) {
                ss << "    r[" << std::min<uint32_t>(dst_temp, 12u) << "]." << m << " = " << src_v << "." << m << ";\n";
            } else {
                std::string target;
                switch (out_reg) {
                    case 0u:  target = "r[12]"; break;
                    case 3u:  target = "oD0"; break;
                    case 4u:  target = "oD1"; break;
                    case 5u:  target = "oFog"; break;
                    case 6u:  target = "oPts"; break;
                    case 9u:  target = "oT0"; break;
                    case 10u: target = "oT1"; break;
                    case 11u: target = "oT2"; break;
                    case 12u: target = "oT3"; break;
                    default:  target = ""; break;
                }
                if (!target.empty()) {
                    if (out_reg == 5u || out_reg == 6u) ss << "    " << target << " = " << src_v << ".x;\n";
                    else ss << "    " << target << "." << m << " = " << src_v << "." << m << ";\n";
                }
            }
        }
    }

    if (glsl) {
        if (is_screenspace) {
            ss << "    vec4 _p = r[12];\n"
               << "    gl_Position = vec4((_p.x / max(vpW, 1.0)) * 2.0 - 1.0, 1.0 - (_p.y / max(vpH, 1.0)) * 2.0, clamp(_p.z, 0.0, 1.0), 1.0);\n"
               << "    gl_Position.x *= (isSS > 0.5 ? widescreenScale : 1.0);\n";
        } else {
            ss << "    gl_Position = r[12];\n"
               << "    gl_Position.x *= widescreenScale;\n";
        }
        ss << "    gl_Position.y *= yFlip;\n"
           << "    gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;\n"
           << "    vs_oD0 = " << sat("oD0") << ";\n"
           << "    vs_oD1 = " << sat("oD1") << ";\n"
           << "    vs_oT0 = oT0; vs_oT1 = oT1; vs_oT2 = oT2; vs_oT3 = oT3;\n"
           << "    vs_oFog = " << sat("oFog") << ";\n"
           << "}\n";
    } else {
        ss << "    VS_OUT vout;\n";
        if (is_screenspace) {
            ss << "    float4 _p = r[12];\n"
               << "    vout.oPos = float4((_p.x / max(vpW, 1.0)) * 2.0 - 1.0, 1.0 - (_p.y / max(vpH, 1.0)) * 2.0, clamp(_p.z, 0.0, 1.0), 1.0);\n"
               << "    vout.oPos.x *= (isSS > 0.5 ? widescreenScale : 1.0);\n";
        } else {
            ss << "    vout.oPos = r[12];\n"
               << "    vout.oPos.x *= widescreenScale;\n";
        }
        ss << "    vout.oD0 = saturate(oD0);\n"
           << "    vout.oD1 = saturate(oD1);\n"
           << "    vout.oT0 = oT0; vout.oT1 = oT1; vout.oT2 = oT2; vout.oT3 = oT3;\n"
           << "    vout.oFog = saturate(oFog);\n"
           << "    return vout;\n}\n";
    }
    return ss.str();
}

std::string generate_ff_vs(const Nv2aVertexDecl& decl, bool is_rhw, bool glsl) {
    bool has_reg[16]{};
    const char* f4 = glsl ? "vec4" : "float4";
    const char* f3 = glsl ? "vec3" : "float3";
    auto sat = [&](const std::string& x) {
        return glsl ? ("clamp(" + x + ", 0.0, 1.0)") : ("saturate(" + x + ")");
    };

    std::ostringstream ss;
    if (glsl) {
        ss << "#version 330 core\n"
           << "layout(std140) uniform FFFrameCB {\n";
    } else {
        ss << "cbuffer FFFrameCB : register(b0) {\n";
    }
    ss << (glsl ? "    mat4 mWorld;\n    mat4 mView;\n    mat4 mProj;\n"
                : "    row_major float4x4 mWorld;\n    row_major float4x4 mView;\n    row_major float4x4 mProj;\n")
       << "    " << f4 << " matDiffuse;\n"
       << "    " << f4 << " matAmbient;\n"
       << "    " << f4 << " matSpecular;\n"
       << "    " << f4 << " matEmissive;\n"
       << "    " << f4 << " globalAmbient;\n"
       << "    " << f4 << " lightPos[4];\n"
       << "    " << f4 << " lightDir[4];\n"
       << "    " << f4 << " lightDiff[4];\n"
       << "    " << f4 << " lightSpec[4];\n"
       << "    " << f4 << " lightAtten[4];\n"
       << "    " << f4 << " fogParams;\n" // x=enable, y=start, z=end, w=density
       << "    " << f4 << " vpInfo;\n"    // x=vpW, y=vpH, z=ws, w=lightingEn
       << "    " << f4 << " eyeFlags;\n"  // x=specEn, y=yFlip, z=hasVertColor, w=fogMode
       << "};\n";

    ss << decl_vs_input_struct(decl, glsl, has_reg);
    if (glsl) {
        ss << "out vec4 vs_oD0;\n"
           << "out vec4 vs_oD1;\n"
           << "out vec4 vs_oT0;\n"
           << "out vec4 vs_oT1;\n"
           << "out vec4 vs_oT2;\n"
           << "out vec4 vs_oT3;\n"
           << "out float vs_oFog;\n"
           << "void main() {\n";
    } else {
        ss << "struct VS_OUT {\n"
           << "    float4 oPos : SV_POSITION;\n"
           << "    float4 oD0  : COLOR0;\n"
           << "    float4 oD1  : COLOR1;\n"
           << "    float4 oT0  : TEXCOORD0;\n"
           << "    float4 oT1  : TEXCOORD1;\n"
           << "    float4 oT2  : TEXCOORD2;\n"
           << "    float4 oT3  : TEXCOORD3;\n"
           << "    float  oFog : FOG;\n"
           << "};\n"
           << "VS_OUT main(VS_IN vin) {\n";
    }

    emit_unpack_inputs(ss, decl, has_reg, glsl);

    if (is_rhw) {
        ss << "    " << f4 << " posClip = " << f4 << "((v0.x / max(vpInfo.x, 1.0)) * 2.0 - 1.0, "
           << "1.0 - (v0.y / max(vpInfo.y, 1.0)) * 2.0, clamp(v0.z, 0.0, 1.0), 1.0);\n"
           << "    posClip.x *= vpInfo.z;\n"
           << "    float fogF = 1.0;\n";
    } else {
        if (glsl) {
            ss << "    vec4 wp = mWorld * vec4(v0.xyz, 1.0);\n"
               << "    vec4 vp = mView * wp;\n"
               << "    vec4 posClip = mProj * vp;\n";
        } else {
            ss << "    float4 wp = mul(float4(v0.xyz, 1.0), mWorld);\n"
               << "    float4 vp = mul(wp, mView);\n"
               << "    float4 posClip = mul(vp, mProj);\n";
        }
        ss << "    posClip.x *= vpInfo.z;\n"
           << "    float fogF = 1.0;\n"
           << "    if (fogParams.x > 0.5) {\n"
           << "        float fd = abs(vp.z);\n"
           << "        if (eyeFlags.w > 2.5) fogF = clamp((fogParams.z - fd) / max(fogParams.z - fogParams.y, 1e-4), 0.0, 1.0);\n"
           << "        else if (eyeFlags.w > 1.5) { float e = fogParams.w * fd; fogF = clamp(exp(-(e*e)), 0.0, 1.0); }\n"
           << "        else fogF = clamp(exp(-fogParams.w * fd), 0.0, 1.0);\n"
           << "    }\n";
    }

    ss << "    " << f4 << " diff = " << (has_reg[3] ? "v3" : "matDiffuse") << ";\n"
       << "    " << f4 << " spec = " << (has_reg[4] ? sat("v4") : std::string(f4) + "(0.0,0.0,0.0,0.0)") << ";\n";

    if (!is_rhw) {
        std::string mul_n = glsl ? "(mat3(mWorld) * v2.xyz)" : "mul(v2.xyz, (float3x3)mWorld)";
        std::string norm_expr = has_reg[2] ? ("normalize(" + mul_n + ")") : (std::string(f3) + "(0.0, 1.0, 0.0)");
        ss << "    if (vpInfo.w > 0.5) {\n"
           << "        " << f3 << " wn = " << norm_expr << ";\n"
           << "        float nlen = length(wn); if (nlen > 1e-4) wn /= nlen; else wn = " << f3 << "(0.0, 1.0, 0.0);\n"
           << "        " << f3 << " ambL = globalAmbient.rgb * matAmbient.rgb + matEmissive.rgb;\n"
           << "        " << f3 << " diffL = " << f3 << "(0.0, 0.0, 0.0);\n"
           << "        " << f3 << " specL = " << f3 << "(0.0, 0.0, 0.0);\n";
        if (!glsl) ss << "        [unroll]\n";
        ss << "        for (int li = 0; li < 4; ++li) {\n"
           << "            if (lightDir[li].w > 0.5) {\n"
           << "                " << f3 << " L;\n"
           << "                float att = 1.0;\n"
           << "                if (lightPos[li].w > 2.5) {\n" // DIRECTIONAL=3
           << "                    L = normalize(-lightDir[li].xyz);\n"
           << "                } else {\n"
           << "                    " << f3 << " toL = lightPos[li].xyz - wp.xyz;\n"
           << "                    float d = length(toL);\n"
           << "                    L = toL / max(d, 1e-4);\n"
           << "                    float rng = lightAtten[li].x;\n"
           << "                    if (rng > 0.0 && d > rng) att = 0.0;\n"
           << "                    else att = 1.0 / max(lightAtten[li].y + lightAtten[li].z * d + lightAtten[li].w * d * d, 0.001);\n"
           << "                }\n"
           << "                float ndl = max(dot(wn, L), 0.0);\n"
           << "                diffL += att * lightDiff[li].rgb * ndl + att * globalAmbient.rgb * 0.0;\n"
           << "                if (eyeFlags.x > 0.5 && ndl > 0.0) {\n"
           << "                    " << f3 << " V = normalize(-vp.xyz);\n"
           << "                    " << f3 << " H = normalize(L + V);\n"
           << "                    specL += att * lightSpec[li].rgb * pow(max(dot(wn, H), 0.0), max(matSpecular.w, 1.0));\n"
           << "                }\n"
           << "            }\n"
           << "        }\n"
           << "        " << f3 << " matC = " << (has_reg[3] ? "v3.rgb" : "matDiffuse.rgb") << ";\n"
           << "        diff.rgb = " << sat("matC * diffL + ambL") << ";\n"
           << "        spec.rgb = " << sat("spec.rgb + specL * matSpecular.rgb") << ";\n"
           << "    }\n";
    }

    if (glsl) {
        ss << "    gl_Position = posClip;\n"
           << "    gl_Position.y *= eyeFlags.y;\n"
           << "    gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;\n"
           << "    vs_oD0 = " << sat("diff") << ";\n"
           << "    vs_oD1 = " << sat("spec") << ";\n"
           << "    vs_oT0 = v9; vs_oT1 = v10; vs_oT2 = v11; vs_oT3 = v12;\n"
           << "    vs_oFog = fogF;\n"
           << "}\n";
    } else {
        ss << "    VS_OUT vout;\n"
           << "    vout.oPos = posClip;\n"
           << "    vout.oD0 = saturate(diff);\n"
           << "    vout.oD1 = saturate(spec);\n"
           << "    vout.oT0 = v9; vout.oT1 = v10; vout.oT2 = v11; vout.oT3 = v12;\n"
           << "    vout.oFog = fogF;\n"
           << "    return vout;\n}\n";
    }
    return ss.str();
}

void emit_alpha_test_block(std::ostringstream& ss, const char* col, bool dynamic_func, uint8_t fixed_func) {
    if (dynamic_func) {
        ss << "    if (alphaTest.x > 0.5) {\n"
           << "        int af = int(alphaTest.z + 0.5);\n"
           << "        if (af == 1) discard;\n"
           << "        else if (af == 2 && !(" << col << ".a < alphaTest.y)) discard;\n"
           << "        else if (af == 3 && !(abs(" << col << ".a - alphaTest.y) < 0.004)) discard;\n"
           << "        else if (af == 4 && !(" << col << ".a <= alphaTest.y)) discard;\n"
           << "        else if (af == 5 && !(" << col << ".a > alphaTest.y)) discard;\n"
           << "        else if (af == 6 && !(" << col << ".a != alphaTest.y)) discard;\n"
           << "        else if (af == 7 && !(" << col << ".a >= alphaTest.y)) discard;\n"
           << "    }\n";
    } else {
        switch (fixed_func) {
            case 1: ss << "    discard;\n"; break;
            case 2: ss << "    if (!(" << col << ".a < alphaTest.y)) discard;\n"; break;
            case 3: ss << "    if (!(abs(" << col << ".a - alphaTest.y) < 0.004)) discard;\n"; break;
            case 4: ss << "    if (!(" << col << ".a <= alphaTest.y)) discard;\n"; break;
            case 5: ss << "    if (!(" << col << ".a > alphaTest.y)) discard;\n"; break;
            case 6: ss << "    if (!(" << col << ".a != alphaTest.y)) discard;\n"; break;
            case 7: ss << "    if (!(" << col << ".a >= alphaTest.y)) discard;\n"; break;
            default: break;
        }
    }
}

std::string generate_xpu_ps(const uint32_t psdef[60], bool glsl) {
    const uint32_t* alpha_in   = &psdef[0];
    uint32_t final_abcd        = psdef[8];
    uint32_t final_efg         = psdef[9];
    const uint32_t* alpha_out  = &psdef[26];
    const uint32_t* rgb_in     = &psdef[34];
    const uint32_t* rgb_out    = &psdef[45];
    uint32_t combo_cnt         = psdef[53];
    uint32_t tex_modes         = psdef[54];
    uint32_t c0_map            = psdef[57];
    uint32_t c1_map            = psdef[58];

    uint32_t stages = std::clamp<uint32_t>(combo_cnt & 0xFu, 1u, 8u);
    const char* f4 = glsl ? "vec4" : "float4";
    const char* f3 = glsl ? "vec3" : "float3";
    auto sat = [&](const std::string& x) {
        return glsl ? ("clamp(" + x + ", 0.0, 1.0)") : ("saturate(" + x + ")");
    };

    std::ostringstream ss;
    if (glsl) {
        ss << "#version 330 core\n"
           << "uniform sampler2D tex0; uniform sampler2D tex1;\n"
           << "uniform sampler2D tex2; uniform sampler2D tex3;\n"
           << "layout(std140) uniform NV2APSConsts { vec4 pc[16]; vec4 alphaTest; vec4 fogColor; vec4 texScale[4]; };\n"
           << "in vec4 vs_oD0; in vec4 vs_oD1; in vec4 vs_oT0; in vec4 vs_oT1; in vec4 vs_oT2; in vec4 vs_oT3; in float vs_oFog;\n"
           << "layout(location = 0) out vec4 FragColor;\n"
           << "void main() {\n";
    } else {
        ss << "Texture2D tex0 : register(t0); Texture2D tex1 : register(t1);\n"
           << "Texture2D tex2 : register(t2); Texture2D tex3 : register(t3);\n"
           << "SamplerState samp0 : register(s0); SamplerState samp1 : register(s1);\n"
           << "SamplerState samp2 : register(s2); SamplerState samp3 : register(s3);\n"
           << "cbuffer NV2APSConsts : register(b0) { float4 pc[16]; float4 alphaTest; float4 fogColor; float4 texScale[4]; };\n"
           << "struct PS_IN {\n"
           << "    float4 oPos : SV_POSITION;\n"
           << "    float4 v0   : COLOR0; float4 v1 : COLOR1;\n"
           << "    float4 tc0  : TEXCOORD0; float4 tc1 : TEXCOORD1; float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3;\n"
           << "    float  fog  : FOG;\n"
           << "};\n"
           << "float4 main(PS_IN pin) : SV_TARGET {\n";
    }
    const char* p0 = glsl ? "vs_oD0" : "pin.v0";
    const char* p1 = glsl ? "vs_oD1" : "pin.v1";
    const char* pf = glsl ? "vs_oFog" : "pin.fog";
    auto tc_name = [&](int s) { return std::string(glsl ? "vs_oT" : "pin.tc") + std::to_string(s); };
    auto sample2d = [&](int s, const std::string& uv) {
        return glsl ? ("texture(tex" + std::to_string(s) + ", " + uv + ")")
                    : ("tex" + std::to_string(s) + ".Sample(samp" + std::to_string(s) + ", " + uv + ")");
    };

    ss << "    " << f4 << " t0 = " << f4 << "(0.0,0.0,0.0,0.0), t1 = " << f4 << "(0.0,0.0,0.0,0.0), t2 = " << f4 << "(0.0,0.0,0.0,0.0), t3 = " << f4 << "(0.0,0.0,0.0,0.0);\n";
    for (int s = 0; s < 4; ++s) {
        uint32_t tm = (tex_modes >> (s * 5)) & 0x1Fu;
        std::string tc = tc_name(s);
        if (tm == 1u || tm == 2u || tm == 3u) {
            ss << "    t" << s << " = " << sample2d(s, "(" + tc + ".xy / max(" + tc + ".w, 1e-5)) * texScale[" + std::to_string(s) + "].xy") << ";\n";
            ss << "    if (abs(" << tc << ".w - 1.0) < 1e-4) t" << s << " = " << sample2d(s, tc + ".xy * texScale[" + std::to_string(s) + "].xy") << ";\n";
        } else if (tm == 4u) {
            ss << "    t" << s << " = " << tc << ";\n";
        } else if (tm != 0u) {
            ss << "    t" << s << " = " << sample2d(s, tc + ".xy * texScale[" + std::to_string(s) + "].xy") << ";\n";
        }
    }
    ss << "    " << f4 << " r0 = " << f4 << "(t0.rgb, t0.a);\n"
       << "    " << f4 << " r1 = " << f4 << "(0.0,0.0,0.0,0.0);\n"
       << "    " << f4 << " v0 = " << p0 << ", v1 = " << p1 << ";\n"
       << "    " << f4 << " sum = " << f4 << "(0.0,0.0,0.0,0.0);\n";

    auto reg_expr = [&](uint8_t byte_val, uint32_t stg, bool is_alpha_port) -> std::string {
        uint32_t mapping = (byte_val >> 5) & 7u;
        uint32_t alpha_sw = (byte_val >> 4) & 1u;
        uint32_t reg = byte_val & 0xFu;
        std::string r;
        switch (reg) {
            case 0u:  r = std::string(f4) + "(0.0,0.0,0.0,0.0)"; break;
            case 1u: {
                uint32_t ci = (c0_map >> (stg * 4u)) & 0xFu;
                if (ci == 0xFu) ci = stg * 2u;
                r = "pc[" + std::to_string(std::min<uint32_t>(ci, 15u)) + "]";
                break;
            }
            case 2u: {
                uint32_t ci = (c1_map >> (stg * 4u)) & 0xFu;
                if (ci == 0xFu) ci = stg * 2u + 1u;
                r = "pc[" + std::to_string(std::min<uint32_t>(ci, 15u)) + "]";
                break;
            }
            case 3u:  r = std::string(f4) + "(" + pf + "," + pf + "," + pf + ",1.0)"; break;
            case 4u:  r = "v0"; break;
            case 5u:  r = "v1"; break;
            case 8u:  r = "t0"; break;
            case 9u:  r = "t1"; break;
            case 10u: r = "t2"; break;
            case 11u: r = "t3"; break;
            case 12u: r = "r0"; break;
            case 13u: r = "r1"; break;
            case 14u: r = sat("v0 + r1"); break;
            case 15u: r = "sum"; break;
            default:  r = std::string(f4) + "(0.0,0.0,0.0,0.0)"; break;
        }
        std::string val = is_alpha_port ? ("(" + r + (alpha_sw ? ".a" : ".b") + ")")
                                        : ("(" + r + (alpha_sw ? ".aaa" : ".rgb") + ")");
        switch (mapping) {
            case 0u: return "max(" + val + ", 0.0)";
            case 1u: return "(1.0 - " + sat(val) + ")";
            case 2u: return "(2.0 * " + val + " - 1.0)";
            case 3u: return "(1.0 - 2.0 * " + val + ")";
            case 4u: return "(" + val + " - 0.5)";
            case 5u: return "(0.5 - " + val + ")";
            case 6u: return val;
            case 7u: return "(-" + val + ")";
            default: return val;
        }
    };

    auto dst_name = [](uint32_t reg) -> const char* {
        switch (reg) {
            case 4u: return "v0"; case 5u: return "v1";
            case 8u: return "t0"; case 9u: return "t1"; case 10u: return "t2"; case 11u: return "t3";
            case 12u: return "r0"; case 13u: return "r1";
            default: return nullptr;
        }
    };

    for (uint32_t s = 0; s < stages; ++s) {
        uint32_t ri = rgb_in[s], ro = rgb_out[s];
        uint32_t cd_dst = ro & 0xFu, ab_dst = (ro >> 4) & 0xFu, sum_dst = (ro >> 8) & 0xFu;
        bool ab_dot = (ro & 0x1000u) != 0, cd_dot = (ro & 0x2000u) != 0, mux = (ro & 0x4000u) != 0;
        uint32_t op_sh = (ro >> 15) & 7u;

        std::string rA = reg_expr(static_cast<uint8_t>((ri >> 24) & 0xFF), s, false);
        std::string rB = reg_expr(static_cast<uint8_t>((ri >> 16) & 0xFF), s, false);
        std::string rC = reg_expr(static_cast<uint8_t>((ri >> 8) & 0xFF), s, false);
        std::string rD = reg_expr(static_cast<uint8_t>(ri & 0xFF), s, false);

        if (ab_dst || cd_dst || sum_dst) {
            auto mkdot = [&](const std::string& x, const std::string& y) {
                return glsl ? (std::string(f3) + "(dot(" + x + "," + y + "))") : ("dot(" + x + "," + y + ").xxx");
            };
            ss << "    { " << f3 << " _ab = " << (ab_dot ? mkdot(rA, rB) : ("(" + rA + "*" + rB + ")")) << ";\n"
               << "      " << f3 << " _cd = " << (cd_dot ? mkdot(rC, rD) : ("(" + rC + "*" + rD + ")")) << ";\n";
            if (op_sh == 1) ss << "      _ab *= 2.0; _cd *= 2.0;\n";
            else if (op_sh == 2) ss << "      _ab *= 4.0; _cd *= 4.0;\n";
            else if (op_sh == 3) ss << "      _ab *= 0.5; _cd *= 0.5;\n";
            ss << "      " << f3 << " _ms = " << (mux ? "(r0.a >= 0.5 ? _cd : _ab)" : "(_ab + _cd)") << ";\n";
            if (const char* d = dst_name(ab_dst))  ss << "      " << d << ".rgb = clamp(_ab, -1.0, 1.0);\n";
            if (const char* d = dst_name(cd_dst))  ss << "      " << d << ".rgb = clamp(_cd, -1.0, 1.0);\n";
            if (const char* d = dst_name(sum_dst)) ss << "      " << d << ".rgb = clamp(_ms, -1.0, 1.0);\n";
            ss << "    }\n";
        }

        uint32_t ai = alpha_in[s], ao = alpha_out[s];
        uint32_t acd_dst = ao & 0xFu, aab_dst = (ao >> 4) & 0xFu, asum_dst = (ao >> 8) & 0xFu;
        bool amux = (ao & 0x4000u) != 0;
        uint32_t aop_sh = (ao >> 15) & 7u;

        std::string aA = reg_expr(static_cast<uint8_t>((ai >> 24) & 0xFF), s, true);
        std::string aB = reg_expr(static_cast<uint8_t>((ai >> 16) & 0xFF), s, true);
        std::string aC = reg_expr(static_cast<uint8_t>((ai >> 8) & 0xFF), s, true);
        std::string aD = reg_expr(static_cast<uint8_t>(ai & 0xFF), s, true);

        if (aab_dst || acd_dst || asum_dst) {
            ss << "    { float _aab = (" << aA << "*" << aB << ");\n"
               << "      float _acd = (" << aC << "*" << aD << ");\n";
            if (aop_sh == 1) ss << "      _aab *= 2.0; _acd *= 2.0;\n";
            else if (aop_sh == 2) ss << "      _aab *= 4.0; _acd *= 4.0;\n";
            else if (aop_sh == 3) ss << "      _aab *= 0.5; _acd *= 0.5;\n";
            ss << "      float _ams = " << (amux ? "(r0.a >= 0.5 ? _acd : _aab)" : "(_aab + _acd)") << ";\n";
            if (const char* d = dst_name(aab_dst))  ss << "      " << d << ".a = clamp(_aab, -1.0, 1.0);\n";
            if (const char* d = dst_name(acd_dst))  ss << "      " << d << ".a = clamp(_acd, -1.0, 1.0);\n";
            if (const char* d = dst_name(asum_dst)) ss << "      " << d << ".a = clamp(_ams, -1.0, 1.0);\n";
            ss << "    }\n";
        }
    }

    if (final_abcd || final_efg) {
        std::string fA = reg_expr(static_cast<uint8_t>((final_abcd >> 24) & 0xFF), 0, false);
        std::string fB = reg_expr(static_cast<uint8_t>((final_abcd >> 16) & 0xFF), 0, false);
        std::string fC = reg_expr(static_cast<uint8_t>((final_abcd >> 8)  & 0xFF), 0, false);
        std::string fD = reg_expr(static_cast<uint8_t>(final_abcd & 0xFF), 0, false);
        std::string fG = reg_expr(static_cast<uint8_t>((final_efg >> 8)   & 0xFF), 0, true);
        ss << "    " << f4 << " _outCol = " << f4 << "(" << sat(fA + " * " + fB + " + (1.0 - " + fA + ") * " + fC + " + " + fD)
           << ", " << sat(fG) << ");\n";
    } else {
        ss << "    " << f4 << " _outCol = " << sat("r0") << ";\n";
    }
    emit_alpha_test_block(ss, "_outCol", true, 8);
    const char* mixfn = glsl ? "mix" : "lerp";
    ss << "    if (alphaTest.w > 0.5) _outCol.rgb = " << mixfn << "(fogColor.rgb, _outCol.rgb, clamp(" << pf << ", 0.0, 1.0));\n";
    ss << (glsl ? "    FragColor = _outCol;\n}\n" : "    return _outCol;\n}\n");
    return ss.str();
}

std::string generate_ff_ps(const Nv2aFFPSKey& cfg, bool glsl) {
    const char* f4 = glsl ? "vec4" : "float4";
    auto sat = [&](const std::string& x) {
        return glsl ? ("clamp(" + x + ", 0.0, 1.0)") : ("saturate(" + x + ")");
    };

    std::ostringstream ss;
    if (glsl) {
        ss << "#version 330 core\n";
        for (int s = 0; s < 4; ++s) {
            bool cube = (cfg.tex_is_cube_mask & (1 << s)) != 0;
            ss << "uniform " << (cube ? "samplerCube" : "sampler2D") << " tex" << s << ";\n";
        }
        ss << "layout(std140) uniform FFPSCB { vec4 texFactor; vec4 alphaTest; vec4 fogColor; vec4 texScale[4]; };\n"
           << "in vec4 vs_oD0; in vec4 vs_oD1; in vec4 vs_oT0; in vec4 vs_oT1; in vec4 vs_oT2; in vec4 vs_oT3; in float vs_oFog;\n"
           << "layout(location = 0) out vec4 FragColor;\n";
    } else {
        for (int s = 0; s < 4; ++s) {
            bool cube = (cfg.tex_is_cube_mask & (1 << s)) != 0;
            ss << (cube ? "TextureCube" : "Texture2D") << " tex" << s << " : register(t" << s << ");\n"
               << "SamplerState samp" << s << " : register(s" << s << ");\n";
        }
        ss << "cbuffer FFPSCB : register(b0) { float4 texFactor; float4 alphaTest; float4 fogColor; float4 texScale[4]; };\n"
           << "struct PS_IN {\n"
           << "    float4 oPos : SV_POSITION;\n"
           << "    float4 v0 : COLOR0; float4 v1 : COLOR1;\n"
           << "    float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1; float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3;\n"
           << "    float  fog : FOG;\n"
           << "};\n";
    }

    const char* p0 = glsl ? "vs_oD0" : "pin.v0";
    const char* p1 = glsl ? "vs_oD1" : "pin.v1";
    const char* pf = glsl ? "vs_oFog" : "pin.fog";

    auto arg_str = [&](uint32_t arg, int stg, bool is_alpha) -> std::string {
        uint32_t base_id = arg & 0x0Fu;
        std::string b;
        switch (base_id) {
            case 0u: b = p0; break;
            case 1u: b = "cur"; break;
            case 2u: b = (cfg.tex_bound_mask & (1 << stg)) ? ("t" + std::to_string(stg)) : (std::string(f4) + "(1.0,1.0,1.0,1.0)"); break;
            case 3u: b = "texFactor"; break;
            case 4u: b = p1; break;
            default: b = "cur"; break;
        }
        bool comp = (arg & 0x10u) != 0;
        bool alp  = (arg & 0x20u) != 0;
        std::string v = is_alpha ? (b + ".a") : (b + (alp ? ".aaa" : ".rgb"));
        return comp ? ("(1.0 - " + v + ")") : v;
    };

    auto apply_op = [&](uint32_t op, const std::string& a1, const std::string& a2, int stg) -> std::string {
        switch (op) {
            case 2u: return a1;
            case 3u: return a2;
            case 4u: return "(" + a1 + " * " + a2 + ")";
            case 5u: return sat(a1 + " * " + a2 + " * 2.0");
            case 6u: return sat(a1 + " * " + a2 + " * 4.0");
            case 7u: return sat(a1 + " + " + a2);
            case 8u: return sat(a1 + " + " + a2 + " - 0.5");
            case 12u: return sat(a1 + " - " + a2);
            case 13u: return sat(a1 + " + " + a2 + " - " + a1 + "*" + a2);
            case 14u: return sat("(" + a1 + " * " + std::string(p0) + ".a) + (" + a2 + " * (1.0 - " + p0 + ".a))");
            case 15u: {
                std::string ta = (cfg.tex_bound_mask & (1 << stg)) ? ("t" + std::to_string(stg) + ".a") : "1.0";
                return sat("(" + a1 + " * " + ta + ") + (" + a2 + " * (1.0 - " + ta + "))");
            }
            case 16u: return sat("(" + a1 + " * texFactor.a) + (" + a2 + " * (1.0 - texFactor.a))");
            default: return "(" + a1 + " * " + a2 + ")";
        }
    };

    ss << (glsl ? "void main() {\n" : "float4 main(PS_IN pin) : SV_TARGET {\n");
    for (int s = 0; s < 4; ++s) {
        if (!(cfg.tex_bound_mask & (1 << s))) continue;
        std::string tc = std::string(glsl ? "vs_oT" : "pin.tc") + std::to_string(s);
        bool cube = (cfg.tex_is_cube_mask & (1 << s)) != 0;
        std::string coord = cube ? (tc + ".xyz") : (tc + ".xy * texScale[" + std::to_string(s) + "].xy");
        std::string samp = glsl ? ("texture(tex" + std::to_string(s) + ", " + coord + ")")
                                : ("tex" + std::to_string(s) + ".Sample(samp" + std::to_string(s) + ", " + coord + ")");
        ss << "    " << f4 << " t" << s << " = " << samp << ";\n";
    }
    ss << "    " << f4 << " cur = " << p0 << ";\n";
    for (int s = 0; s < 4; ++s) {
        uint32_t cop = cfg.stage_color_op[s];
        uint32_t aop = cfg.stage_alpha_op[s];
        if (cop <= 1u) break;
        ss << "    cur.rgb = " << apply_op(cop, arg_str(cfg.stage_color_arg1[s], s, false), arg_str(cfg.stage_color_arg2[s], s, false), s) << ";\n";
        if (aop > 1u) {
            ss << "    cur.a = " << apply_op(aop, arg_str(cfg.stage_alpha_arg1[s], s, true), arg_str(cfg.stage_alpha_arg2[s], s, true), s) << ";\n";
        }
    }
    if (cfg.has_specular) ss << "    cur.rgb = " << sat("cur.rgb + " + std::string(p1) + ".rgb") << ";\n";
    if (cfg.alpha_test_enable) emit_alpha_test_block(ss, "cur", false, cfg.alpha_func);
    if (cfg.fog_enable) {
        const char* mixfn = glsl ? "mix" : "lerp";
        ss << "    cur.rgb = " << mixfn << "(fogColor.rgb, cur.rgb, clamp(" << pf << ", 0.0, 1.0));\n";
    }
    ss << (glsl ? ("    FragColor = " + sat("cur") + ";\n}\n") : ("    return saturate(cur);\n}\n"));
    return ss.str();
}

} // namespace

void nv2a_shaders_init(void* dx_dev_or_null, bool opengl_mode, const std::string& cache_dir) {
    std::lock_guard<std::mutex> lk(g_sh_mtx);
#ifdef _WIN32
    g_dx_dev = static_cast<ID3D11Device*>(dx_dev_or_null);
#else
    (void)dx_dev_or_null;
#endif
    g_gl_mode = opengl_mode;
    g_cache_dir = cache_dir;
    if (!g_cache_dir.empty()) {
        std::error_code ec;
        fs::create_directories(g_cache_dir, ec);
    }
}

void nv2a_shaders_shutdown() {
    std::lock_guard<std::mutex> lk(g_sh_mtx);
    auto& gl = whacked_gl();
    for (auto& kv : g_vs_cache) {
#ifdef _WIN32
        if (kv.second.dx_il) static_cast<ID3D11InputLayout*>(kv.second.dx_il)->Release();
        if (kv.second.dx_vs) static_cast<ID3D11VertexShader*>(kv.second.dx_vs)->Release();
#endif
        if (kv.second.gl_vs && gl.glDeleteShader) gl.glDeleteShader(kv.second.gl_vs);
    }
    for (auto& kv : g_ps_cache) {
#ifdef _WIN32
        if (kv.second.dx_ps) static_cast<ID3D11PixelShader*>(kv.second.dx_ps)->Release();
#endif
        if (kv.second.gl_ps && gl.glDeleteShader) gl.glDeleteShader(kv.second.gl_ps);
    }
    g_vs_cache.clear();
    g_ps_cache.clear();
#ifdef _WIN32
    g_dx_dev = nullptr;
#endif
    g_gl_mode = false;
}

Nv2aVertexDecl nv2a_parse_declaration(uint32_t decl_va) {
    Nv2aVertexDecl d;
    if (!decl_va || !mem_ptr(decl_va)) return d;
    uint8_t cur_stream = 0;
    uint16_t cur_off = 0;
    uint32_t p = decl_va;
    for (int guard = 0; guard < 256; ++guard) {
        uint32_t tok = rd32(p);
        p += 4;
        if (tok == 0xFFFFFFFFu) break;
        uint32_t op = tok & 0xE0000000u;
        if (op == 0x00000000u) {
            continue;
        } else if (op == 0x20000000u) {
            cur_stream = static_cast<uint8_t>(tok & 0x3u);
            cur_off = 0;
        } else if (op == 0x40000000u) {
            if (tok & 0x10000000u) {
                uint32_t cnt = (tok >> 16) & 0xFu;
                cur_off += static_cast<uint16_t>((tok & 0x08000000u) ? cnt : (cnt * 4u));
            } else {
                uint8_t reg  = static_cast<uint8_t>(tok & 0x1Fu);
                uint8_t type = static_cast<uint8_t>((tok >> 16) & 0xFFu);
                if (reg < 16) {
                    Nv2aVertexDecl::Element e{cur_stream, reg, type, cur_off};
                    d.elements.push_back(e);
                }
                cur_off += static_cast<uint16_t>(vsdt_byte_size(type));
            }
            d.stream_strides[cur_stream] = std::max<uint32_t>(d.stream_strides[cur_stream], cur_off);
        } else if (op == 0x80000000u) {
            uint32_t cnt = (tok >> 25) & 0xFu;
            p += cnt * 16u;
        }
    }
    return d;
}

uint32_t nv2a_fvf_stride(uint32_t fvf) {
    uint32_t sz = 0;
    switch (fvf & 0x000E) {
        case 0x0002: sz += 12; break;
        case 0x0004: sz += 16; break;
        case 0x0006: sz += 16; break;
        case 0x0008: sz += 20; break;
        case 0x000A: sz += 24; break;
        case 0x000C: sz += 28; break;
        default: break;
    }
    if (fvf & 0x0010) sz += 12;
    if (fvf & 0x0020) sz += 4;
    if (fvf & 0x0040) sz += 4;
    if (fvf & 0x0080) sz += 4;
    uint32_t tex_cnt = (fvf >> 8) & 0xF;
    for (uint32_t t = 0; t < tex_cnt; ++t) {
        uint32_t fmt = (fvf >> (16 + t * 2)) & 3u;
        static const uint8_t tsz[4] = {8, 12, 16, 4};
        sz += tsz[fmt];
    }
    return sz;
}

Nv2aVertexDecl nv2a_decl_from_fvf(uint32_t fvf) {
    Nv2aVertexDecl d;
    d.is_fvf = true;
    d.fvf = fvf;
    uint16_t off = 0;
    uint32_t pos = fvf & 0x000E;
    if (pos == 0x0002) { d.elements.push_back({0, 0, 0x32, off}); off += 12; }
    else if (pos == 0x0004) { d.elements.push_back({0, 0, 0x42, off}); off += 16; }
    else if (pos >= 0x0006) {
        d.elements.push_back({0, 0, 0x32, off}); off += 12;
        uint32_t betas = (pos - 0x0004) / 2;
        uint8_t btype = (betas == 1) ? 0x12 : (betas == 2) ? 0x22 : (betas == 3) ? 0x32 : 0x42;
        d.elements.push_back({0, 1, btype, off}); off += static_cast<uint16_t>(betas * 4);
    }
    if (fvf & 0x0010) { d.elements.push_back({0, 2, 0x32, off}); off += 12; }
    if (fvf & 0x0020) { off += 4; }
    if (fvf & 0x0040) { d.elements.push_back({0, 3, 0x40, off}); off += 4; }
    if (fvf & 0x0080) { d.elements.push_back({0, 4, 0x40, off}); off += 4; }
    uint32_t tex_cnt = (fvf >> 8) & 0xF;
    for (uint32_t t = 0; t < tex_cnt && t < 4; ++t) {
        uint32_t fmt = (fvf >> (16 + t * 2)) & 3u;
        static const uint8_t dt[4]  = {0x22, 0x32, 0x42, 0x12};
        static const uint16_t sz[4] = {8, 12, 16, 4};
        d.elements.push_back({0, static_cast<uint8_t>(9 + t), dt[fmt], off});
        off += sz[fmt];
    }
    d.stream_strides[0] = off;
    return d;
}

const Nv2aCompiledVS* nv2a_get_xvu_vs(const Nv2aVertexDecl& decl, const uint8_t* xvu_bytes, size_t byte_len) {
    if (!xvu_bytes || byte_len < 4) return nullptr;
    uint16_t nins = static_cast<uint16_t>(xvu_bytes[2] | (xvu_bytes[3] << 8));
    if (byte_len < static_cast<size_t>(4 + nins * 16)) return nullptr;

    uint64_t key = fnv1a_bytes(xvu_bytes, 4 + nins * 16, 0x58565532ull);
    for (const auto& e : decl.elements) {
        uint32_t pack = (e.stream << 24) | (e.reg << 16) | (e.type << 8) | (e.offset & 0xFF);
        key = fnv1a_bytes(&pack, 4, key);
    }

    std::lock_guard<std::mutex> lk(g_sh_mtx);
    auto it = g_vs_cache.find(key);
    if (it != g_vs_cache.end()) return &it->second;

    bool has_viewport_epilogue = false;
    for (uint16_t k = 0; k < nins; ++k) {
        uint32_t w1;
        std::memcpy(&w1, xvu_bytes + 4 + k * 16 + 4, 4);
        uint32_t raw_c = (w1 >> 13) & 0xFFu;
        if (raw_c == 58u || raw_c == 59u) has_viewport_epilogue = true;
    }
    bool is_screenspace = !has_viewport_epilogue;

    Nv2aCompiledVS res{};
    res.key = key;
    res.is_screenspace = is_screenspace;
    res.decl = decl;

    if (g_gl_mode) {
        std::string src = generate_xvu_vs(decl, xvu_bytes, nins, has_viewport_epilogue, is_screenspace, true);
        res.gl_vs = compile_gl_shader(GL_VERTEX_SHADER, src);
    }
#ifdef _WIN32
    else if (g_dx_dev) {
        std::string src = generate_xvu_vs(decl, xvu_bytes, nins, has_viewport_epilogue, is_screenspace, false);
        std::vector<uint8_t> bytecode;
        if (compile_hlsl(src, "main", "vs_5_0", "xvu_vs", key, bytecode)) {
            ID3D11VertexShader* vs = nullptr;
            if (SUCCEEDED(g_dx_dev->CreateVertexShader(bytecode.data(), bytecode.size(), nullptr, &vs))) {
                res.dx_vs = vs;
                res.dx_il = build_dx_input_layout(decl, bytecode.data(), bytecode.size());
            }
        }
    }
#endif
    else {
        return nullptr;
    }

    g_vs_cache[key] = std::move(res);
    return &g_vs_cache[key];
}

const Nv2aCompiledVS* nv2a_get_ff_vs(const Nv2aVertexDecl& decl) {
    uint64_t key = fnv1a_bytes(&decl.fvf, 4, 0x46465654ull);
    for (const auto& e : decl.elements) {
        uint32_t pack = (e.stream << 24) | (e.reg << 16) | (e.type << 8) | (e.offset & 0xFF);
        key = fnv1a_bytes(&pack, 4, key);
    }

    std::lock_guard<std::mutex> lk(g_sh_mtx);
    auto it = g_vs_cache.find(key);
    if (it != g_vs_cache.end()) return &it->second;

    bool is_rhw = (decl.fvf & 0x000E) == 0x0004;
    Nv2aCompiledVS res{};
    res.key = key;
    res.is_screenspace = is_rhw;
    res.decl = decl;

    if (g_gl_mode) {
        std::string src = generate_ff_vs(decl, is_rhw, true);
        res.gl_vs = compile_gl_shader(GL_VERTEX_SHADER, src);
    }
#ifdef _WIN32
    else if (g_dx_dev) {
        std::string src = generate_ff_vs(decl, is_rhw, false);
        std::vector<uint8_t> bytecode;
        if (compile_hlsl(src, "main", "vs_5_0", "ff_vs", key, bytecode)) {
            ID3D11VertexShader* vs = nullptr;
            if (SUCCEEDED(g_dx_dev->CreateVertexShader(bytecode.data(), bytecode.size(), nullptr, &vs))) {
                res.dx_vs = vs;
                res.dx_il = build_dx_input_layout(decl, bytecode.data(), bytecode.size());
            }
        }
    }
#endif
    else {
        return nullptr;
    }

    g_vs_cache[key] = std::move(res);
    return &g_vs_cache[key];
}

const Nv2aCompiledPS* nv2a_get_xpu_ps(const uint32_t psdef[60]) {
    if (!psdef) return nullptr;
    uint64_t key = fnv1a_bytes(psdef, 240, 0x58505532ull);

    std::lock_guard<std::mutex> lk(g_sh_mtx);
    auto it = g_ps_cache.find(key);
    if (it != g_ps_cache.end()) return &it->second;

    Nv2aCompiledPS res{};
    res.key = key;
    res.tex_modes = psdef[54];

    if (g_gl_mode) {
        std::string src = generate_xpu_ps(psdef, true);
        res.gl_ps = compile_gl_shader(GL_FRAGMENT_SHADER, src);
    }
#ifdef _WIN32
    else if (g_dx_dev) {
        std::string src = generate_xpu_ps(psdef, false);
        std::vector<uint8_t> bytecode;
        if (compile_hlsl(src, "main", "ps_5_0", "xpu_ps", key, bytecode)) {
            ID3D11PixelShader* ps = nullptr;
            if (SUCCEEDED(g_dx_dev->CreatePixelShader(bytecode.data(), bytecode.size(), nullptr, &ps))) {
                res.dx_ps = ps;
            }
        }
    }
#endif
    else {
        return nullptr;
    }

    g_ps_cache[key] = res;
    return &g_ps_cache[key];
}

const Nv2aCompiledPS* nv2a_get_ff_ps(const Nv2aFFPSKey& cfg) {
    uint64_t key = fnv1a_bytes(&cfg, sizeof(cfg), 0x46465054ull);

    std::lock_guard<std::mutex> lk(g_sh_mtx);
    auto it = g_ps_cache.find(key);
    if (it != g_ps_cache.end()) return &it->second;

    Nv2aCompiledPS res{};
    res.key = key;
    res.tex_is_cube_mask = cfg.tex_is_cube_mask;

    if (g_gl_mode) {
        std::string src = generate_ff_ps(cfg, true);
        res.gl_ps = compile_gl_shader(GL_FRAGMENT_SHADER, src);
    }
#ifdef _WIN32
    else if (g_dx_dev) {
        std::string src = generate_ff_ps(cfg, false);
        std::vector<uint8_t> bytecode;
        if (compile_hlsl(src, "main", "ps_5_0", "ff_ps", key, bytecode)) {
            ID3D11PixelShader* ps = nullptr;
            if (SUCCEEDED(g_dx_dev->CreatePixelShader(bytecode.data(), bytecode.size(), nullptr, &ps))) {
                res.dx_ps = ps;
            }
        }
    }
#endif
    else {
        return nullptr;
    }

    g_ps_cache[key] = res;
    return &g_ps_cache[key];
}
