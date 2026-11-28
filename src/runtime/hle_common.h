#pragma once

#include "cpu_context.h"
#include "xbox_mem.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

extern bool g_hle_trace;

inline uint32_t hle_arg(const CpuContext& ctx, int idx) {
    return rd32(ctx.esp + 4u + static_cast<uint32_t>(idx) * 4u);
}

inline void hle_ret(CpuContext& ctx, uint32_t pop_bytes = 0) {
    ctx.esp += 4u + pop_bytes;
}

inline void hle_ret_u32(CpuContext& ctx, uint32_t val, uint32_t pop_bytes = 0) {
    ctx.eax = val;
    ctx.esp += 4u + pop_bytes;
}

inline void hle_ret_u64(CpuContext& ctx, uint64_t val, uint32_t pop_bytes = 0) {
    ctx.eax = static_cast<uint32_t>(val);
    ctx.edx = static_cast<uint32_t>(val >> 32);
    ctx.esp += 4u + pop_bytes;
}

void hle_register_kernel();
void hle_register_xapi_crt();
void hle_register_input();
void hle_register_audio();
void hle_register_bink();
void hle_register_net();
void hle_register_d3d8();
void hle_register_all();

void hle_kernel_update_ticks();
void hle_kernel_request_shutdown();
bool hle_kernel_is_shutting_down();
bool hle_kernel_wait_for_guest_threads(uint32_t timeout_ms);
void hle_kernel_shutdown();
void hle_bink_shutdown();
void hle_input_shutdown();

bool hle_file_read(uint32_t handle, void* dst, uint32_t len, uint32_t* out_got);
bool hle_file_seek(uint32_t handle, int64_t offset, uint32_t origin, uint64_t* out_pos);
uint64_t hle_file_size(uint32_t handle);
bool hle_file_read_bink_from_handle(uint32_t handle, std::vector<uint8_t>& out_bik, uint64_t* out_off = nullptr);
void hle_input_set_script(const std::string& spec);
void hle_d3d8_set_hd720(bool enable);
void hle_d3d8_mark_bink_frame(uint32_t va);
void hle_kernel_dump_stall_state();
void hle_kernel_signal_handle(uint32_t handle);
void hle_kernel_notify_frame();
struct GuestPowerOff {};
void app_request_quit();
uint64_t hle_kernel_get_stall_seq();
void hle_input_set_frame(uint64_t frame_num);
int  hle_input_get_primary_pad_type(); // 0=Xbox, 1=PlayStation, 2=Nintendo
bool hle_input_get_port_info(int port, bool* out_connected, char* out_name, size_t max_name);
void hle_input_apply_deadzone_2d(int16_t rx_in, int16_t ry_in, float inner_dz, float sensitivity,
                                 bool invert_y, int16_t* out_x, int16_t* out_y);
float hle_audio_soft_limit(float x);
void audio_set_mix_levels(float master, float sfx, float music, bool muted);
size_t hle_audio_decode_xbox_adpcm(const uint8_t* src, size_t src_bytes, int channels,
                                   std::vector<int16_t>& out_pcm);
void hle_set_log_sink(void (*sink)(const char*));
void hle_set_thread_exit_hook(void (*hook)());
void hle_log_note(const char* fmt, ...);
