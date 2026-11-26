#include "runtime/app_config.h"
#include "runtime/cpu_context.h"
#include "runtime/hle_common.h"
#include "runtime/net_lobby.h"
#include "runtime/xbox_mem.h"
#include "runtime/xiso_vfs.h"
#include "rl_achievements.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Headless runtime stubs for symbols provided by main/recomp/UI modules
bool g_hle_trace = false;
void recomp_register_fn(uint32_t, RecompFn) {}
void recomp_dispatch_call(CpuContext&, uint32_t) {}
void hle_kernel_dump_stall_state() {}
void hle_kernel_signal_handle(uint32_t) {}
bool hle_file_read(uint32_t, void*, uint32_t, uint32_t*) { return false; }
bool hle_file_seek(uint32_t, int64_t, uint32_t, uint64_t*) { return false; }
uint64_t hle_file_size(uint32_t) { return 0; }
bool ui_is_launcher_active() { return false; }
bool ui_is_overlay_open() { return false; }
bool ui_wants_keyboard() { return false; }
void ui_toggle_overlay() {}

namespace {

int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
        ++g_failures; \
    } else { \
        std::printf("PASS: %s\n", (msg)); \
    } \
} while (0)

void test_xbox_mem_coalescing() {
    CHECK(xbox_mem_init(), "xbox_mem_init succeeds");
    const uint64_t base_used = xbox_mem_used_bytes();

    uint32_t a = xbox_guest_alloc(4096, 4096, true);
    uint32_t b = xbox_guest_alloc(8192, 4096, true);
    uint32_t c = xbox_guest_alloc(4096, 4096, true);
    CHECK(a != 0 && b == a + 4096 && c == b + 8192, "contiguous bump allocation");
    CHECK(xbox_mem_used_bytes() == base_used + 4096 + 8192 + 4096, "used_bytes accounts allocations");

    // Free middle then ends in order that exercises bidirectional neighbor coalescing
    CHECK(xbox_guest_free(b), "free middle block");
    CHECK(xbox_mem_free_block_count() == 1, "one free block before edge coalesce");
    CHECK(xbox_guest_free(a), "free left neighbor coalesces into [a, c)");
    CHECK(xbox_mem_free_block_count() == 1, "left neighbor coalesced cleanly");
    CHECK(xbox_guest_free(c), "free top block collapses entire frontier back to bump");
    CHECK(xbox_mem_free_block_count() == 0, "free map empty after frontier reclaim");
    CHECK(xbox_mem_used_bytes() == base_used, "used_bytes returns to baseline");

    // Reallocate coalesced span as a single larger block
    uint32_t big = xbox_guest_alloc(16384, 4096, true);
    CHECK(big == a, "reclaimed arena address reused identically");
    CHECK(xbox_guest_free(big), "free coalesced block");

    // Thread TEB alloc + reclamation
    uint32_t teb1 = xbox_alloc_teb(0x02100000u, 0x02000000u, 0x400u, 10);
    uint32_t teb2 = xbox_alloc_teb(0x02200000u, 0x02100000u, 0x400u, 11);
    CHECK(teb1 != 0 && teb2 != 0 && teb1 != teb2, "TEB slots distinct");
    xbox_free_teb(teb1);
    uint32_t teb1_reused = xbox_alloc_teb(0x02100000u, 0x02000000u, 0x400u, 12);
    CHECK(teb1_reused == teb1, "freed TEB slot reclaimed cleanly");
    xbox_free_teb(teb1_reused);
    xbox_free_teb(teb2);
    xbox_mem_shutdown();
}

void test_xiso_path_resolution() {
    auto& vfs = XisoVfs::instance();
    std::string dev, rel;
    CHECK(vfs.resolve_path("D:\\Global\\Textures.pak", dev, rel) && dev == "iso" && rel == "global\\textures.pak",
          "D:\\ normalizes to iso lowercase");
    CHECK(vfs.resolve_path("\\??\\D:\\Movies\\Intro.bik", dev, rel) && dev == "iso" && rel == "movies\\intro.bik",
          "\\??\\D:\\ strips NT prefix");
    CHECK(vfs.resolve_path("\\Device\\Cdrom0\\default.xbe", dev, rel) && dev == "iso" && rel == "default.xbe",
          "\\Device\\Cdrom0\\ maps to iso");
    CHECK(vfs.resolve_path("U:\\4d530027\\Profile.bin", dev, rel) && dev == "U" && rel == "4d530027\\profile.bin",
          "U:\\ maps to UDATA");
    CHECK(vfs.resolve_path("T:\\Cache.dat", dev, rel) && dev == "T" && rel == "cache.dat",
          "T:\\ maps to TDATA");
    CHECK(vfs.resolve_path("Z:\\scratch.tmp", dev, rel) && dev == "Z" && rel == "scratch.tmp",
          "Z:\\ maps to utility scratch drive");
    CHECK(vfs.resolve_path("$Global\\foo", dev, rel) && dev == "iso" && rel == "global\\foo",
          "$ relative prefix maps to iso root");
}

void test_app_config_roundtrip() {
    fs::path tmp = fs::temp_directory_path() / "whacked_test_cfg";
    std::error_code ec;
    fs::remove_all(tmp, ec);

    auto& cfg = app_config();
    cfg = AppConfig{};
    cfg.renderer = "opengl";
    cfg.width = 1920;
    cfg.height = 800;
    cfg.widescreen = true;
    cfg.hd720 = true;
    cfg.unstretched_hud = true;
    cfg.fps_cap = 240;
    cfg.sfx_volume = 0.75f;
    cfg.music_volume = 0.55f;
    cfg.muted = true;
    cfg.deadzone_left = 0.22f;
    cfg.deadzone_right = 0.18f;
    cfg.invert_cam_y = true;
    cfg.cam_sensitivity = 1.75f;
    cfg.pad_preset = 1;
    cfg.mp_mode = 2;
    cfg.mp_tunnel_port = 31337;
    cfg.mp_join_code = "WHK-TEST-CODE-1234";

    CHECK(app_config_save(tmp.string()), "app_config_save writes INI");
    cfg = AppConfig{};
    CHECK(app_config_load(tmp.string()), "app_config_load parses INI");
    CHECK(cfg.renderer == "opengl" && cfg.width == 1920 && cfg.height == 800, "graphics resolution & renderer roundtrip");
    CHECK(cfg.unstretched_hud && cfg.fps_cap == 240, "unstretched_hud & 240 fps cap roundtrip");
    CHECK(std::fabs(cfg.sfx_volume - 0.75f) < 0.01f && std::fabs(cfg.music_volume - 0.55f) < 0.01f && cfg.muted,
          "audio volumes & mute roundtrip");
    CHECK(cfg.invert_cam_y && std::fabs(cfg.cam_sensitivity - 1.75f) < 0.01f && cfg.pad_preset == 1,
          "camera invert, sensitivity & preset roundtrip");
    CHECK(cfg.mp_mode == 2 && cfg.mp_tunnel_port == 31337 && cfg.mp_join_code == "WHK-TEST-CODE-1234",
          "multiplayer settings roundtrip");

    fs::remove_all(tmp, ec);
}

void test_input_2d_deadzone() {
    int16_t ox = 0, oy = 0;
    hle_input_apply_deadzone_2d(2500, 2500, 0.15f, 1.0f, false, &ox, &oy);
    CHECK(ox == 0 && oy == 0, "small input inside 2D radius zeros both axes");

    // Diagonal outside inner deadzone should preserve 45 deg direction evenly
    hle_input_apply_deadzone_2d(16000, 16000, 0.15f, 1.0f, false, &ox, &oy);
    CHECK(ox > 10000 && std::abs(ox - oy) <= 1, "diagonal radial deadzone preserves angle symmetrically");

    // Invert Y + sensitivity boost clamps cleanly without clipping into corners
    hle_input_apply_deadzone_2d(20000, 20000, 0.15f, 2.0f, true, &ox, &oy);
    CHECK(ox > 20000 && oy < -20000 && std::abs(ox + oy) <= 2, "sensitivity + invert_y preserves circular clamp");
}

void test_audio_dsp_and_adpcm() {
    CHECK(std::fabs(hle_audio_soft_limit(0.5f) - 0.5f) < 1e-6f, "soft limiter linear below knee");
    float mid = hle_audio_soft_limit(1.05f);
    float hi = hle_audio_soft_limit(4.0f);
    float lo = hle_audio_soft_limit(-4.0f);
    CHECK(mid > 0.90f && mid < 1.0f && hi >= 0.99f && hi <= 1.0f && lo <= -0.99f && lo >= -1.0f,
          "soft limiter compresses peaks smoothly within [-1, 1]");

    // Construct a minimal 1-channel 36-byte Xbox ADPCM block (4-byte header + 32 nibble bytes = 64 samples)
    uint8_t blk[36]{};
    // predictor = 1000 (0x03E8 LE), step_index = 10
    blk[0] = 0xE8; blk[1] = 0x03; blk[2] = 10; blk[3] = 0;
    for (size_t i = 4; i < 36; ++i) blk[i] = 0x01; // positive small deltas
    std::vector<int16_t> pcm;
    size_t got = hle_audio_decode_xbox_adpcm(blk, sizeof(blk), 1, pcm);
    CHECK(got == 64 && pcm[0] > 950 && pcm.back() > pcm[0],
          "Xbox 0x0069 mono ADPCM decodes 64 samples per block cleanly");
}

void test_net_room_code_crc8() {
    std::string code;
    CHECK(net_encode_room_code("203.0.113.42", 30740, code), "encode room code succeeds");
    CHECK(code.rfind("WHK-", 0) == 0, "room code starts with WHK-");
    std::string ip;
    uint16_t port = 0;
    CHECK(net_decode_room_code(code, ip, port) && ip == "203.0.113.42" && port == 30740,
          "decode recovers exact IPv4 + UDP port");

    // Flip last character -> CRC8 must reject
    std::string bad = code;
    bad.back() = (bad.back() == 'A') ? 'B' : 'A';
    CHECK(!net_decode_room_code(bad, ip, port), "corrupted CRC8 room code rejected");
}

void test_rl_achievements_rules() {
    static uint8_t ram[64]{};
    static uint32_t last_unlock = 0;
    auto reader = [](uint32_t addr, uint8_t* buf, uint32_t n, void*) -> uint32_t {
        if (addr >= sizeof(ram)) return 0;
        if (addr + n > sizeof(ram)) n = static_cast<uint32_t>(sizeof(ram) - addr);
        std::memcpy(buf, ram + addr, n);
        return n;
    };
    auto on_unlock = [](uint32_t id, void*) { last_unlock = id; };

    CHECK(rl_ach_init("xbox", reader, nullptr) == 0, "rl_ach_init works offline without RL_SESSION");
    rl_ach_on_unlock(on_unlock, nullptr);
    ram[0x10] = 5; // starts already met -> enters waiting state until reset/re-earned
    int n = rl_ach_load_list("{\"achievements\":[{\"id\":7,\"trigger\":\"0xH0010=5\"}]}");
    CHECK(n == 1, "rl_ach_load_list parses achievement rule");
    for (int i = 0; i < 10; ++i) rl_ach_frame();
    CHECK(last_unlock == 0, "pre-satisfied rule stays waiting on load");
    ram[0x10] = 1; rl_ach_frame();
    ram[0x10] = 5; rl_ach_frame();
    CHECK(last_unlock == 7, "rule unlocks cleanly on false -> true transition");
    rl_ach_shutdown();
}

} // namespace

int main() {
    test_xbox_mem_coalescing();
    test_xiso_path_resolution();
    test_app_config_roundtrip();
    test_input_2d_deadzone();
    test_audio_dsp_and_adpcm();
    test_net_room_code_crc8();
    test_rl_achievements_rules();

    std::printf("\nTotal failures: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
