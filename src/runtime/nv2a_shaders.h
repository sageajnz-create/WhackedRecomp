#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Nv2aVertexDecl {
    struct Element {
        uint8_t stream = 0;
        uint8_t reg = 0;      // NV2A input reg v[0..15]
        uint8_t type = 0;     // D3DVSDT_*
        uint16_t offset = 0;  // byte offset inside stream
    };
    std::vector<Element> elements;
    uint32_t stream_strides[4]{};
    bool is_fvf = false;
    uint32_t fvf = 0;
};

struct Nv2aCompiledVS {
    void* dx_vs = nullptr;           // ID3D11VertexShader* (Windows D3D11)
    void* dx_il = nullptr;           // ID3D11InputLayout*  (Windows D3D11)
    uint32_t gl_vs = 0;              // GLuint shader (OpenGL 3.3)
    Nv2aVertexDecl decl;             // Needed by GL backend for vertex attrib binding
    bool is_screenspace = false;
    uint64_t key = 0;
};

struct Nv2aCompiledPS {
    void* dx_ps = nullptr;           // ID3D11PixelShader*  (Windows D3D11)
    uint32_t gl_ps = 0;              // GLuint shader (OpenGL 3.3)
    uint32_t tex_modes = 0;          // PSTextureModes (5 bits per stage 0..3)
    uint8_t  tex_is_cube_mask = 0;
    uint64_t key = 0;
};

struct Nv2aFFPSKey {
    uint32_t stage_color_op[4]{};
    uint32_t stage_color_arg1[4]{};
    uint32_t stage_color_arg2[4]{};
    uint32_t stage_alpha_op[4]{};
    uint32_t stage_alpha_arg1[4]{};
    uint32_t stage_alpha_arg2[4]{};
    uint8_t  tex_bound_mask = 0;
    uint8_t  tex_is_cube_mask = 0;
    uint8_t  alpha_test_enable = 0;
    uint8_t  alpha_func = 8; // D3DCMP_ALWAYS
    uint8_t  fog_enable = 0;
    uint8_t  lighting_enable = 0;
    uint8_t  has_diffuse = 1;
    uint8_t  has_specular = 0;
};

void nv2a_shaders_init(void* dx_dev_or_null, bool opengl_mode, const std::string& cache_dir);
void nv2a_shaders_shutdown();

Nv2aVertexDecl nv2a_parse_declaration(uint32_t decl_va);
Nv2aVertexDecl nv2a_decl_from_fvf(uint32_t fvf);
uint32_t nv2a_fvf_stride(uint32_t fvf);

const Nv2aCompiledVS* nv2a_get_xvu_vs(const Nv2aVertexDecl& decl, const uint8_t* xvu_bytes, size_t byte_len);
const Nv2aCompiledVS* nv2a_get_ff_vs(const Nv2aVertexDecl& decl);
const Nv2aCompiledPS* nv2a_get_xpu_ps(const uint32_t psdef_words[60]);
const Nv2aCompiledPS* nv2a_get_ff_ps(const Nv2aFFPSKey& cfg);
