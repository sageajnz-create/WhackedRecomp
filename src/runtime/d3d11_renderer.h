#pragma once

#include "gfx_backend.h"
#include "nv2a_shaders.h"

#ifdef _WIN32
struct ID3D11Device;
struct ID3D11DeviceContext;

bool d3d11_init(void* hwnd, const RendererSettings& settings);
void d3d11_shutdown();
void d3d11_resize(int width, int height);
void d3d11_set_fullscreen(bool fs);
ID3D11Device* d3d11_device();
ID3D11DeviceContext* d3d11_context();
RendererSettings& d3d11_settings();
void d3d11_clear(uint32_t flags, uint32_t color_argb, float z, uint32_t stencil, GpuTextureObj* custom_rt = nullptr);
void d3d11_draw(const DrawStateSnapshot& st);
void d3d11_upload_texture(GpuTextureObj* t, uint32_t palette_va);
void d3d11_blit_rgba_rect(const void* bgra, int src_w, int src_h, int dst_x, int dst_y, int dst_w, int dst_h);
void d3d11_present();
void d3d11_request_screenshot(const std::string& filename);
void d3d11_retire_texture(GpuTextureObj* tex);
void d3d11_retire_buffer(uint32_t obj_va);
double d3d11_get_fps();
int d3d11_get_last_draw_calls();
void* d3d11_create_ui_texture_rgba(const uint8_t* rgba, int w, int h);
void  d3d11_destroy_ui_texture(void* tex_handle);
#endif
