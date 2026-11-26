#pragma once

#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>
#include <cstddef>
#include <cstdint>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
#ifndef GL_PROGRAM_BINARY_RETRIEVABLE_HINT
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257
#endif
#ifndef GL_PROGRAM_BINARY_LENGTH
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#endif

// Minimal cross-platform OpenGL 3.3 Core function table loaded via SDL_GL_GetProcAddress
#define WHACKED_GL_LIST(X) \
    X(PFNGLCREATEPROGRAMPROC,             glCreateProgram) \
    X(PFNGLDELETEPROGRAMPROC,             glDeleteProgram) \
    X(PFNGLCREATESHADERPROC,              glCreateShader) \
    X(PFNGLDELETESHADERPROC,              glDeleteShader) \
    X(PFNGLSHADERSOURCEPROC,              glShaderSource) \
    X(PFNGLCOMPILESHADERPROC,             glCompileShader) \
    X(PFNGLGETSHADERIVPROC,               glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC,          glGetShaderInfoLog) \
    X(PFNGLATTACHSHADERPROC,              glAttachShader) \
    X(PFNGLLINKPROGRAMPROC,               glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC,              glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC,         glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC,                glUseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC,        glGetUniformLocation) \
    X(PFNGLUNIFORM1IPROC,                 glUniform1i) \
    X(PFNGLGETUNIFORMBLOCKINDEXPROC,      glGetUniformBlockIndex) \
    X(PFNGLUNIFORMBLOCKBINDINGPROC,       glUniformBlockBinding) \
    X(PFNGLGENVERTEXARRAYSPROC,           glGenVertexArrays) \
    X(PFNGLDELETEVERTEXARRAYSPROC,        glDeleteVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC,           glBindVertexArray) \
    X(PFNGLGENBUFFERSPROC,                glGenBuffers) \
    X(PFNGLDELETEBUFFERSPROC,             glDeleteBuffers) \
    X(PFNGLBINDBUFFERPROC,                glBindBuffer) \
    X(PFNGLBINDBUFFERBASEPROC,            glBindBufferBase) \
    X(PFNGLBUFFERDATAPROC,                glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC,             glBufferSubData) \
    X(PFNGLMAPBUFFERPROC,                 glMapBuffer) \
    X(PFNGLMAPBUFFERRANGEPROC,            glMapBufferRange) \
    X(PFNGLUNMAPBUFFERPROC,               glUnmapBuffer) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC,   glEnableVertexAttribArray) \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC,  glDisableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC,       glVertexAttribPointer) \
    X(PFNGLVERTEXATTRIBIPOINTERPROC,      glVertexAttribIPointer) \
    X(PFNGLACTIVETEXTUREPROC,             glActiveTexture) \
    X(PFNGLCOMPRESSEDTEXIMAGE2DPROC,      glCompressedTexImage2D) \
    X(PFNGLGENERATEMIPMAPPROC,            glGenerateMipmap) \
    X(PFNGLGENFRAMEBUFFERSPROC,           glGenFramebuffers) \
    X(PFNGLDELETEFRAMEBUFFERSPROC,        glDeleteFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC,           glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC,      glFramebufferTexture2D) \
    X(PFNGLGENRENDERBUFFERSPROC,          glGenRenderbuffers) \
    X(PFNGLDELETERENDERBUFFERSPROC,       glDeleteRenderbuffers) \
    X(PFNGLBINDRENDERBUFFERPROC,          glBindRenderbuffer) \
    X(PFNGLRENDERBUFFERSTORAGEPROC,       glRenderbufferStorage) \
    X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC,   glFramebufferRenderbuffer) \
    X(PFNGLBLITFRAMEBUFFERPROC,           glBlitFramebuffer) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC,    glCheckFramebufferStatus) \
    X(PFNGLGENSAMPLERSPROC,               glGenSamplers) \
    X(PFNGLDELETESAMPLERSPROC,            glDeleteSamplers) \
    X(PFNGLBINDSAMPLERPROC,               glBindSampler) \
    X(PFNGLSAMPLERPARAMETERIPROC,         glSamplerParameteri) \
    X(PFNGLSAMPLERPARAMETERFPROC,         glSamplerParameterf) \
    X(PFNGLBLENDEQUATIONPROC,             glBlendEquation) \
    X(PFNGLBLENDFUNCSEPARATEPROC,         glBlendFuncSeparate) \
    X(PFNGLDRAWELEMENTSBASEVERTEXPROC,    glDrawElementsBaseVertex)

struct WhackedGLProcs {
#define DECLARE_GL(type, name) type name = nullptr;
    WHACKED_GL_LIST(DECLARE_GL)
#undef DECLARE_GL
    PFNGLGETPROGRAMBINARYPROC  glGetProgramBinary = nullptr;
    PFNGLPROGRAMBINARYPROC     glProgramBinary = nullptr;
    PFNGLPROGRAMPARAMETERIPROC glProgramParameteri = nullptr;
    bool loaded = false;
    bool has_s3tc = false;
    bool has_aniso = false;
    bool has_prog_binary = false;
};

inline WhackedGLProcs& whacked_gl() {
    static WhackedGLProcs g;
    return g;
}

inline bool whacked_gl_load() {
    auto& g = whacked_gl();
    if (g.loaded) return true;
    bool ok = true;
#define LOAD_GL(type, name) \
    g.name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name)); \
    if (!g.name) ok = false;
    WHACKED_GL_LIST(LOAD_GL)
#undef LOAD_GL
    g.glGetProgramBinary  = reinterpret_cast<PFNGLGETPROGRAMBINARYPROC>(SDL_GL_GetProcAddress("glGetProgramBinary"));
    g.glProgramBinary     = reinterpret_cast<PFNGLPROGRAMBINARYPROC>(SDL_GL_GetProcAddress("glProgramBinary"));
    g.glProgramParameteri = reinterpret_cast<PFNGLPROGRAMPARAMETERIPROC>(SDL_GL_GetProcAddress("glProgramParameteri"));
    g.has_prog_binary = (g.glGetProgramBinary != nullptr && g.glProgramBinary != nullptr);
    g.has_s3tc = SDL_GL_ExtensionSupported("GL_EXT_texture_compression_s3tc") != SDL_FALSE;
    g.has_aniso = SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic") != SDL_FALSE;
    g.loaded = ok;
    return ok;
}
