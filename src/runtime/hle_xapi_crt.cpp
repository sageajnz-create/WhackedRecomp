#include "hle_common.h"
#include "xiso_vfs.h"
#include <cstdio>

namespace {
// Bink allocator thunks 0x002A5000 & 0x002A5070 (called by XAPILIB FLS/TLS init at 0x001E4467)
void hle_radmalloc(CpuContext& ctx) {
    uint32_t sz = hle_arg(ctx, 0);
    uint32_t p = xbox_guest_alloc(sz ? sz : 16, 16, true);
    hle_ret_u32(ctx, p, 4);
}
void hle_radfree(CpuContext& ctx) {
    xbox_guest_free(hle_arg(ctx, 0));
    hle_ret(ctx, 4);
}

// Engine Assert / DbgOut helper? Let's keep assert silent-return in release unless traced
} // namespace

void hle_register_xapi_crt() {
    recomp_register_fn(0x002A5000u, hle_radmalloc);
    recomp_register_fn(0x002A5070u, hle_radfree);
}

void hle_register_all() {
    hle_register_kernel();
    hle_register_xapi_crt();
    hle_register_input();
    hle_register_audio();
    hle_register_bink();
    hle_register_net();
    hle_register_d3d8();
    std::printf("[HLE] All Xbox HLE subsystems registered.\n");
}
