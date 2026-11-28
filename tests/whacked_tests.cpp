#include "runtime/app_config.h"
#include "runtime/cpu_context.h"
#include "runtime/hle_common.h"
#include "runtime/net_lobby.h"
#include "runtime/xbox_mem.h"
#include "runtime/xiso_vfs.h"
#include "rl_achievements.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

std::array<float, 2> test_audio_mix_bin_gains(uint32_t routes, uint32_t volumes, uint32_t headroom);
int16_t test_audio_mix_sample(unsigned effects, bool music);
int32_t test_audio_get_buffer_volume(uint32_t obj_va, bool* out_found);
int32_t test_audio_get_stream_volume(uint32_t obj_va, bool* out_found);
void test_audio_clear_objects();
uint64_t test_audio_get_bink_queued_frames(uint32_t stream_id);
void test_audio_pump_frames(int frames);
uint32_t audio_bink_open_stream(uint32_t sample_rate, uint32_t channels);
void audio_bink_push_pcm(uint32_t stream_id, const int16_t* samples, size_t byte_len);
void audio_bink_pause(uint32_t stream_id, bool paused);
void audio_bink_flush(uint32_t stream_id);
double audio_bink_get_clock_sec(uint32_t stream_id);
void audio_bink_close(uint32_t stream_id);
std::string test_kernel_resolve_attrs(uint32_t obj_attr_va);
void test_kernel_io_create_symlink(CpuContext& ctx);
void test_kernel_io_delete_symlink(CpuContext& ctx);
void test_kernel_nt_create_file(CpuContext& ctx);
void test_kernel_nt_close(CpuContext& ctx);

// Headless runtime stubs & function registration recorder
static std::unordered_map<uint32_t, RecompFn> g_registered_fns;
void recomp_register_fn(uint32_t va, RecompFn fn) { g_registered_fns[va] = fn; }
void recomp_dispatch_call(CpuContext&, uint32_t) {}
void app_request_quit() {}
extern void audio_shutdown();
bool ui_is_launcher_active() { return false; }
bool ui_is_overlay_open() { return false; }
bool ui_wants_keyboard() { return false; }
bool ui_quit_prompt_captures_input() { return false; }
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
    CHECK(vfs.resolve_path("\\Device\\Harddisk0\\Partition1\\UDATA\\4d530027\\11c90f515484\\whackedsavedata.sav", dev, rel) &&
          dev == "U" && rel == "11c90f515484\\whackedsavedata.sav",
          "Partition1\\UDATA\\4d530027 strips device/title prefix cleanly");
    CHECK(vfs.resolve_path("\\Device\\Harddisk0\\Partition1\\UDATA\\4d530027", dev, rel) &&
          dev == "U" && rel.empty(),
          "Bare Partition1\\UDATA\\4d530027 resolves to UDATA root");
    CHECK(vfs.resolve_path("\\Device\\Harddisk0\\Partition1\\TDATA\\4d530027\\settings.cfg", dev, rel) &&
          dev == "T" && rel == "settings.cfg",
          "Partition1\\TDATA\\4d530027 maps to TDATA root");
    CHECK(vfs.resolve_path("\\Device\\Harddisk0\\Partition0\\cache\\scratch.bin", dev, rel) &&
          dev == "Z" && rel == "cache\\scratch.bin",
          "Partition0 maps to utility Z cache drive");
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

void test_audio_mix_bin_routing() {
    if (!xbox_mem_init()) {
        CHECK(false, "guest memory available for audio mix-bin test");
        return;
    }
    const uint32_t routes = xbox_guest_alloc(24, 4, true);
    const uint32_t volumes = xbox_guest_alloc(24, 4, true);
    wr32(routes, 0xFFFFFF02u); // Count is stored in the low byte.
    wr32(routes + 4, routes + 8);
    wr32(routes + 8, 0);  // Front left
    wr32(routes + 16, 1); // Front right
    wr32(volumes, 2);
    wr32(volumes + 4, volumes + 8);
    wr32(volumes + 8, 0);
    wr32(volumes + 12, static_cast<uint32_t>(-2000));
    wr32(volumes + 16, 12); // An effects send must not replace the dry routing.
    auto [left, right] = test_audio_mix_bin_gains(routes, volumes, 600);
    CHECK(std::fabs(left - 0.05012f) < 0.0001f && std::fabs(right - 0.50119f) < 0.0001f,
          "speaker updates preserve routes and apply distance attenuation and voice headroom");

    wr32(routes + 8, 12);
    wr32(routes + 16, 13);
    auto [effects_left, effects_right] = test_audio_mix_bin_gains(routes, 0, 0);
    CHECK(effects_left == 0.0f && effects_right == 0.0f,
          "effects-only routing does not leak an unattenuated dry signal");

    // In stereo mode, games often keep bin 2 (Center) at 0 dB alongside panned FL/FR (bins 0/1).
    // Center must NOT fold into L/R when FL/FR routes exist, only when FL/FR are absent.
    wr32(routes, 3);
    wr32(routes + 8, 0);  wr32(routes + 12, static_cast<uint32_t>(-2000));
    wr32(routes + 16, 1); wr32(routes + 20, 0);
    wr32(routes + 24, 2); wr32(routes + 28, 0);
    // Use volumes table for clean per-bin levels; test helper reads 24-byte allocations safely
    const uint32_t r3 = xbox_guest_alloc(32, 4, true);
    const uint32_t v3 = xbox_guest_alloc(32, 4, true);
    wr32(r3, 3); wr32(r3 + 4, r3 + 8);
    wr32(r3 + 8, 0);  wr32(r3 + 16, 1); wr32(r3 + 24, 2);
    wr32(v3, 3); wr32(v3 + 4, v3 + 8);
    wr32(v3 + 8, 0);  wr32(v3 + 12, static_cast<uint32_t>(-2000));
    wr32(v3 + 16, 1); wr32(v3 + 20, 0);
    wr32(v3 + 24, 2); wr32(v3 + 28, 0);
    auto [st_l, st_r] = test_audio_mix_bin_gains(r3, v3, 0);
    CHECK(std::fabs(st_l - 0.1f) < 0.01f && std::fabs(st_r - 1.0f) < 0.01f,
          "active FL/FR stereo routes isolate left/right panning without center bleed");

    wr32(r3, 1); wr32(r3 + 8, 2);
    auto [ctr_l, ctr_r] = test_audio_mix_bin_gains(r3, v3, 0);
    CHECK(std::fabs(ctr_l - 0.7071f) < 0.01f && std::fabs(ctr_r - 0.7071f) < 0.01f,
          "center-only route falls back to -3 dB L/R folddown");
    xbox_mem_shutdown();
}

void test_audio_thunk_dispatch_and_volumes() {
    if (!xbox_mem_init()) {
        CHECK(false, "guest memory available for audio thunk test");
        return;
    }
    g_registered_fns.clear();
    hle_register_audio();

    // Buffer vs Stream thunks must never cross over onto the wrong object type
    CHECK(g_registered_fns[0x0026302Eu] && g_registered_fns[0x0026329Au] &&
          g_registered_fns[0x0026302Eu] != g_registered_fns[0x0026329Au],
          "Buffer::SetVolume and Stream::SetVolume thunks map to distinct handlers");
    CHECK(g_registered_fns[0x002630BAu] && g_registered_fns[0x002632B3u] &&
          g_registered_fns[0x002630BAu] != g_registered_fns[0x002632B3u],
          "Buffer::SetHeadroom and Stream::SetHeadroom thunks map to distinct handlers");
    CHECK(g_registered_fns[0x002630D6u] && g_registered_fns[0x002632B8u] &&
          g_registered_fns[0x002630D6u] != g_registered_fns[0x002632B8u],
          "Buffer::SetMixBins and Stream::SetMixBins thunks map to distinct handlers");
    CHECK(g_registered_fns[0x002630F2u] && g_registered_fns[0x002632BDu] &&
          g_registered_fns[0x002630F2u] != g_registered_fns[0x002632BDu],
          "Buffer::SetMixBinVolumes and Stream::SetMixBinVolumes thunks map to distinct handlers");
    CHECK(g_registered_fns[0x00264CB3u] != nullptr && g_registered_fns[0x00264F2Bu] != nullptr,
          "Buffer::SetMode_8 and Stream::SetMode_8 thunks are registered");
    {
        CpuContext mctx{};
        mctx.esp = 0x02200000u;
        g_registered_fns[0x00264CB3u](mctx);
        uint32_t buf_esp = mctx.esp;
        uint32_t buf_eax = mctx.eax;
        mctx.esp = 0x02200000u;
        g_registered_fns[0x00264F2Bu](mctx);
        CHECK(buf_eax == 0u && buf_esp == 0x0220000Cu && mctx.eax == 0u && mctx.esp == 0x0220000Cu,
              "Buffer::SetMode_8 and Stream::SetMode_8 both return DS_OK and pop 8 stack bytes");
    }
    CHECK(g_registered_fns[0x00265E0Au] != nullptr,
          "IDirectSound::CreateSoundBuffer COM thunk is registered");

    auto call_stdcall = [](uint32_t va, std::initializer_list<uint32_t> args) -> uint32_t {
        CpuContext ctx{};
        ctx.esp = 0x02200000u;
        uint32_t i = 0;
        for (uint32_t a : args) wr32(ctx.esp + 4u + (i++) * 4u, a);
        g_registered_fns[va](ctx);
        return ctx.eax;
    };

    // Create buffer via IDirectSound::CreateSoundBuffer and stream via DirectSoundCreateStream
    uint32_t ds_va = 0, buf_va = 0, strm_va = 0;
    const uint32_t out_ds = xbox_guest_alloc(16, 4, true);
    const uint32_t wfx = xbox_guest_alloc(32, 4, true);
    wr16(wfx + 0, 1); wr16(wfx + 2, 1); wr32(wfx + 4, 44100); wr32(wfx + 8, 88200);
    wr16(wfx + 12, 2); wr16(wfx + 14, 16);
    const uint32_t dsbd = xbox_guest_alloc(64, 4, true);
    wr32(dsbd + 0, 44); wr32(dsbd + 8, 4096); wr32(dsbd + 12, wfx);
    const uint32_t ds_sd = xbox_guest_alloc(64, 4, true);
    wr32(ds_sd + 4, wfx);

    CHECK(call_stdcall(0x00265FC4u, {0, out_ds, 0}) == 0u, "DirectSoundCreate succeeds");
    ds_va = rd32(out_ds);
    const uint32_t out_buf = xbox_guest_alloc(16, 4, true);
    const uint32_t out_strm = xbox_guest_alloc(16, 4, true);
    CHECK(call_stdcall(0x00265E0Au, {ds_va, dsbd, out_buf, 0}) == 0u, "IDirectSound::CreateSoundBuffer succeeds");
    buf_va = rd32(out_buf);
    CHECK(call_stdcall(0x00266062u, {ds_sd, out_strm, 0}) == 0u, "DirectSoundCreateStream succeeds");
    strm_va = rd32(out_strm);

    CHECK(call_stdcall(0x0026302Eu, {buf_va, static_cast<uint32_t>(-1800)}) == 0u, "Buffer SetVolume succeeds");
    CHECK(call_stdcall(0x0026329Au, {strm_va, static_cast<uint32_t>(-2700)}) == 0u, "Stream SetVolume succeeds");
    bool bf = false, sf = false;
    int32_t bv = test_audio_get_buffer_volume(buf_va, &bf);
    int32_t sv = test_audio_get_stream_volume(strm_va, &sf);
    CHECK(bf && bv == -1800 && sf && sv == -2700,
          "Buffer and Stream SetVolume update their respective objects cleanly");

    test_audio_clear_objects();
    xbox_mem_shutdown();
}

void test_kernel_symlinks_and_file_ops() {
    fs::path tmp = fs::temp_directory_path() / "whacked_test_krnl";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    XisoVfs::instance().set_userdata_root(tmp.string());

    if (!xbox_mem_init()) {
        CHECK(false, "guest memory available for kernel test");
        return;
    }

    auto make_ansi = [](const std::string& s) -> uint32_t {
        uint32_t astr = xbox_guest_alloc(16, 4, true);
        uint32_t buf = xbox_guest_alloc(static_cast<uint32_t>(s.size() + 4), 4, true);
        std::memcpy(mem_ptr(buf), s.c_str(), s.size() + 1);
        wr16(astr + 0, static_cast<uint16_t>(s.size()));
        wr16(astr + 2, static_cast<uint16_t>(s.size() + 1));
        wr32(astr + 4, buf);
        return astr;
    };
    auto make_obj_attrs = [&](const std::string& s) -> uint32_t {
        uint32_t oa = xbox_guest_alloc(16, 4, true);
        wr32(oa + 0, 0);
        wr32(oa + 4, make_ansi(s));
        wr32(oa + 8, 0x40u);
        return oa;
    };
    auto run_fn = [](void (*fn)(CpuContext&), std::initializer_list<uint32_t> args) -> uint32_t {
        CpuContext ctx{};
        ctx.esp = 0x02210000u;
        uint32_t i = 0;
        for (uint32_t a : args) wr32(ctx.esp + 4u + (i++) * 4u, a);
        fn(ctx);
        return ctx.eax;
    };

    // Case-insensitive symlink creation, bare drive resolution, and trailing-slash-insensitive delete
    CHECK(run_fn(test_kernel_io_create_symlink, {make_ansi("\\??\\U:"), make_ansi("\\Device\\Harddisk0\\Partition1\\UDATA\\4d530027")}) == 0u,
          "IoCreateSymbolicLink succeeds");
    std::string r_bare = test_kernel_resolve_attrs(make_obj_attrs("\\??\\u:"));
    std::string r_sub  = test_kernel_resolve_attrs(make_obj_attrs("\\??\\U:\\11c90f515484\\whackedsavedata.sav"));
    CHECK(r_bare == "\\Device\\Harddisk0\\Partition1\\UDATA\\4d530027",
          "Bare \\??\\u: resolves case-insensitively");
    CHECK(r_sub == "\\Device\\Harddisk0\\Partition1\\UDATA\\4d530027\\11c90f515484\\whackedsavedata.sav",
          "Nested \\??\\U:\\... expands symlink cleanly");
    CHECK(run_fn(test_kernel_io_delete_symlink, {make_ansi("\\??\\U:\\")}) == 0u,
          "IoDeleteSymbolicLink succeeds with trailing backslash");
    std::string r_after = test_kernel_resolve_attrs(make_obj_attrs("\\??\\U:\\foo"));
    CHECK(r_after == "\\??\\U:\\foo",
          "Deleted symlink no longer rewrites paths");

    // Directory creation collision (FILE_CREATE) & FILE_DELETE_ON_CLOSE recursive cleanup
    uint32_t ph = xbox_guest_alloc(8, 4, true);
    uint32_t iosb = xbox_guest_alloc(16, 4, true);
    uint32_t oa_dir = make_obj_attrs("U:\\prof1");
    CHECK(run_fn(test_kernel_nt_create_file, {ph, 0x100001u, oa_dir, iosb, 0, 0x10u, 3u, 2u /* FILE_CREATE */, 0x01u /* FILE_DIRECTORY_FILE */}) == 0u &&
          rd32(iosb + 4) == 2u,
          "NtCreateFile FILE_CREATE directory sets FILE_CREATED");
    uint32_t h_dir = rd32(ph);
    CHECK(run_fn(test_kernel_nt_close, {h_dir}) == 0u, "NtClose directory handle succeeds");
    CHECK(run_fn(test_kernel_nt_create_file, {ph, 0x100001u, oa_dir, iosb, 0, 0x10u, 3u, 2u /* FILE_CREATE */, 0x01u}) == 0xC0000035u,
          "NtCreateFile FILE_CREATE on existing directory returns collision");

    // Populate child file, open dir with FILE_DELETE_ON_CLOSE (0x1000), close -> whole directory removed
    uint32_t oa_child = make_obj_attrs("U:\\prof1\\save.dat");
    CHECK(run_fn(test_kernel_nt_create_file, {ph, 0xC0000000u, oa_child, iosb, 0, 0x80u, 3u, 2u, 0x40u}) == 0u &&
          rd32(iosb + 4) == 2u,
          "NtCreateFile FILE_CREATE regular file sets FILE_CREATED");
    run_fn(test_kernel_nt_close, {rd32(ph)});
    CHECK(run_fn(test_kernel_nt_create_file, {ph, 0x100001u, oa_dir, iosb, 0, 0x10u, 3u, 1u /* FILE_OPEN */, 0x1001u /* DIR | DELETE_ON_CLOSE */}) == 0u,
          "Open directory with FILE_DELETE_ON_CLOSE succeeds");
    run_fn(test_kernel_nt_close, {rd32(ph)});
    CHECK(!fs::exists(tmp / "UDATA" / "prof1", ec),
          "Closing FILE_DELETE_ON_CLOSE directory removes non-empty save folder");

    xbox_mem_shutdown();
    fs::remove_all(tmp, ec);
}

void test_audio_mixer_controls() {
    audio_set_mix_levels(1.0f, 1.0f, 1.0f, false);
    CHECK(std::abs(test_audio_mix_sample(1, false) - 16383) <= 1,
          "effects bus leaves headroom at 100 percent");
    audio_set_mix_levels(0.25f, 1.0f, 1.0f, false);
    CHECK(std::abs(test_audio_mix_sample(8, false) - 8191) <= 1,
          "master volume caps even a heavily limited mix");
    audio_set_mix_levels(0.5f, 0.0f, 1.0f, false);
    CHECK(std::abs(test_audio_mix_sample(4, true) - 8191) <= 1,
          "effects slider can silence effects independently of music");
    audio_set_mix_levels(0.5f, 1.0f, 1.0f, true);
    CHECK(test_audio_mix_sample(1, true) == 0, "mute silences both buses");
    audio_set_mix_levels(0.5f, 1.0f, 1.0f, false);
    CHECK(std::abs(test_audio_mix_sample(0, true) - 8191) <= 1,
          "unmute restores the configured master level");
    audio_set_mix_levels(1.0f, 1.0f, 1.0f, false);
}

void test_net_room_code_crc8() {
    std::string code;
    CHECK(net_encode_room_code("203.0.113.42", 30740, code), "encode room code succeeds");
    CHECK(code.rfind("WHK-", 0) == 0, "room code starts with WHK-");
    std::string ip;
    uint16_t port = 0;
    CHECK(net_decode_room_code(code, ip, port) && ip == "203.0.113.42" && port == 30740,
          "decode recovers exact IPv4 + UDP port");

    CHECK(net_decode_room_code("\r\n  " + code + "\r\n", ip, port) && port == 30740,
          "room codes pasted with line endings decode");
    CHECK(!net_decode_room_code("127.0.0.1:30740junk", ip, port), "trailing port garbage rejected");
    CHECK(!net_decode_room_code("127.0.0.1:", ip, port), "missing port rejected");

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

void test_bink_audio_backpressure_and_clock() {
    uint32_t sid = audio_bink_open_stream(44100u, 2u);
    CHECK(sid != 0, "audio_bink_open_stream returns non-zero id");
    audio_bink_pause(sid, true);

    std::vector<int16_t> chunk(1024u * 2u, 1000);
    std::atomic<int> pushed{0};
    std::atomic<bool> done{false};
    std::thread prod([&]() {
        for (int i = 0; i < 64; ++i) {
            audio_bink_push_pcm(sid, chunk.data(), chunk.size() * sizeof(int16_t));
            pushed.fetch_add(1, std::memory_order_release);
        }
        done.store(true, std::memory_order_release);
    });

    // Wait briefly until queue hits the 1s cap (~44,100 frames = 44 chunks of 1024)
    auto t0 = std::chrono::steady_clock::now();
    while (pushed.load(std::memory_order_acquire) < 44 &&
           std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 1500) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    int blocked_count = pushed.load(std::memory_order_acquire);
    CHECK(blocked_count >= 43 && blocked_count < 64 && !done.load(std::memory_order_acquire),
          "audio_bink_push_pcm applies backpressure instead of silently dropping chunks");
    test_audio_pump_frames(4096);
    CHECK(std::abs(audio_bink_get_clock_sec(sid)) < 1e-6,
          "paused Bink audio stream does not advance audio clock during mix");

    // Unpause and pump until all 64 chunks complete without loss
    audio_bink_pause(sid, false);
    t0 = std::chrono::steady_clock::now();
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 3000) {
        test_audio_pump_frames(4096);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!done.load(std::memory_order_acquire)) audio_bink_close(sid);
    prod.join();
    CHECK(done.load(std::memory_order_acquire) && pushed.load(std::memory_order_acquire) == 64,
          "unpausing drains queued Bink chunks cleanly without drops");
    double clk = audio_bink_get_clock_sec(sid);
    CHECK(clk > 0.2, "Bink audio clock advances monotonically as frames are consumed");
    audio_bink_flush(sid);
    CHECK(test_audio_get_bink_queued_frames(sid) == 0 && std::abs(audio_bink_get_clock_sec(sid)) < 1e-6,
          "audio_bink_flush clears queued frames and resets audio clock");

    // Closing a stream must immediately unblock a waiting producer thread
    audio_bink_pause(sid, true);
    std::atomic<bool> closed_done{false};
    std::thread blocked_prod([&]() {
        for (int i = 0; i < 64; ++i) {
            audio_bink_push_pcm(sid, chunk.data(), chunk.size() * sizeof(int16_t));
        }
        closed_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    audio_bink_close(sid);
    blocked_prod.join();
    CHECK(closed_done.load(std::memory_order_acquire),
          "audio_bink_close unblocks saturated producer thread immediately");
}

void test_kernel_shutdown_and_wait() {
    CHECK(xbox_mem_init(), "xbox_mem_init succeeds for kernel shutdown test");
    hle_register_kernel();
    CHECK(!hle_kernel_is_shutting_down(), "hle_register_kernel resets shutdown state");
    CHECK(hle_kernel_wait_for_guest_threads(50), "no stray guest threads before spawn");
    hle_kernel_request_shutdown();
    CHECK(hle_kernel_is_shutting_down(), "hle_kernel_request_shutdown flips shutdown flag");
    hle_kernel_shutdown();
    hle_register_kernel();
    CHECK(!hle_kernel_is_shutting_down(), "hle_register_kernel re-arms after shutdown");
    hle_kernel_shutdown();
    xbox_mem_shutdown();
}

} // namespace

int main() {
    test_xbox_mem_coalescing();
    test_xiso_path_resolution();
    test_app_config_roundtrip();
    test_input_2d_deadzone();
    test_audio_dsp_and_adpcm();
    test_audio_mix_bin_routing();
    test_audio_thunk_dispatch_and_volumes();
    test_audio_mixer_controls();
    test_bink_audio_backpressure_and_clock();
    test_kernel_symlinks_and_file_ops();
    test_kernel_shutdown_and_wait();
    test_net_room_code_crc8();
    test_rl_achievements_rules();

    audio_shutdown();
    std::printf("\nTotal failures: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
