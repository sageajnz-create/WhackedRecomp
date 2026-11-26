#pragma once

#include "cpu_context.h"
#include <cstdint>
#include <string>
#include <vector>

struct HleKernelDataLayout {
    static constexpr uint32_t KPCR_PRIMARY_VA = 0x00001000u;
    static constexpr uint32_t KPRCB_PRIMARY_VA = 0x00001200u;
    static constexpr uint32_t KTHREAD_PRIMARY_VA = 0x00001800u;
    static constexpr uint32_t TLS_ARRAY_PRIMARY_VA = 0x00002000u;
    static constexpr uint32_t KERNEL_EXPORTS_BASE = 0x00004000u;
    static constexpr uint32_t KERNEL_STUB_BASE = 0x7F000000u; // + ordinal
    static constexpr uint32_t ARENA_ALLOC_BASE = 0x02000000u;
    static constexpr uint32_t ARENA_ALLOC_END  = 0x0F000000u;
};

struct XbeSectionInfo {
    std::string name;
    uint32_t flags;
    uint32_t va;
    uint32_t vsz;
    uint32_t raw_off;
    uint32_t raw_sz;
};

struct XbeImageInfo {
    uint32_t base_addr = 0x00010000u;
    uint32_t headers_size = 0;
    uint32_t image_size = 0;
    uint32_t entry_va = 0;
    uint32_t thunk_va = 0;
    uint32_t tls_va = 0;
    uint32_t stack_commit = 0x00020000u;
    uint32_t title_id = 0x4D530027u;
    std::vector<XbeSectionInfo> sections;
};

extern XbeImageInfo g_xbe_info;

bool xbox_mem_init();
void xbox_mem_shutdown();
bool xbox_load_xbe_from_memory(const uint8_t* xbe_bytes, size_t size);
void xbox_mem_protect_readonly();

// Thread TEB/KPCR creation & cleanup in guest space
uint32_t xbox_alloc_teb(uint32_t stack_base_va, uint32_t stack_limit_va, uint32_t tls_size, uint32_t thread_id);
void     xbox_free_teb(uint32_t kpcr_va);

// Guest allocators (thread-safe, sorted coalescing)
uint32_t xbox_guest_alloc(uint32_t size, uint32_t align = 4096u, bool zero_init = true);
bool     xbox_guest_free(uint32_t va);
uint32_t xbox_guest_alloc_size(uint32_t va);
void     xbox_guest_query_stats(uint32_t* total_pages, uint32_t* avail_pages);
uint64_t xbox_mem_used_bytes();
size_t   xbox_mem_free_block_count();

// Read/write guest C strings cleanly
std::string xbox_read_cstr(uint32_t va, size_t max_len = 512);
std::string xbox_read_ansi_string(uint32_t pan_va);
void        xbox_write_cstr(uint32_t va, const char* s, size_t max_buf);
