#define SDL_MAIN_HANDLED
#include <SDL.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#else
#include <execinfo.h>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#endif

#include "runtime/hle_common.h"
#include "runtime/xiso_vfs.h"
#include "runtime/gfx_backend.h"
#include "runtime/app_config.h"
#include "runtime/launcher_ui.h"
#include "runtime/net_lobby.h"
#include "runtime/cpu_context.h"
#include "runtime/xbox_mem.h"
#include "rl_achievements.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

extern void audio_shutdown();
extern void hle_d3d8_set_hd720(bool enable);
extern void hle_input_poll_ui_snapshot();

namespace {

constexpr const char* kAppVersion = "1.0.4";

SDL_Window* g_sdl_win = nullptr;
std::atomic<bool> g_quit_requested{false};

std::string g_log_path;
std::mutex g_log_mtx;

void log_note(const char* fmt, ...) {
    if (g_log_path.empty()) return;
    char buf[1024]{};
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_log_mtx);
    std::ofstream f(g_log_path, std::ios::app);
    if (f.is_open()) f << buf << "\n";
}

void log_sink_raw(const char* line) {
    if (line) log_note("%s", line);
}

void init_rotating_log(const std::string& userdata_root) {
    std::error_code ec;
    fs::path ldir = fs::path(userdata_root) / "logs";
    fs::create_directories(ldir, ec);
    fs::path p0 = ldir / "whacked.log";
    fs::path p1 = ldir / "whacked.1.log";
    fs::path p2 = ldir / "whacked.2.log";
    if (fs::exists(p1, ec)) fs::rename(p1, p2, ec);
    if (fs::exists(p0, ec)) fs::rename(p0, p1, ec);
    g_log_path = p0.string();
    std::ofstream trunc(g_log_path, std::ios::trunc);
    if (trunc.is_open()) {
        trunc << "=== WhackedRecomp " << kAppVersion << " Log ===\n";
    }
}

uint64_t g_max_frames = 0; // 0 = unlimited
std::string g_shot_out_dir = "screenshots";
std::unordered_set<uint64_t> g_shot_frames;
std::string g_final_shot_path;
std::string g_shot_launcher_path;
uint64_t g_shot_overlay_frame = 0;
std::string g_shot_overlay_path;
bool g_fullscreen_state = false;

void toggle_fullscreen_window() {
    g_fullscreen_state = !g_fullscreen_state;
    app_config().fullscreen = g_fullscreen_state;
    gfx_set_fullscreen(g_fullscreen_state);
}

fs::path get_exe_dir(const char* argv0) {
    std::error_code ec;
#ifdef _WIN32
    char exe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path();
#else
    char exe[1024]{};
    ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        return fs::path(exe).parent_path();
    }
    if (argv0 && argv0[0]) return fs::path(argv0).parent_path();
    return fs::current_path(ec);
#endif
}

bool is_dir_writable(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return false;
    fs::path probe = dir / ".whacked_write_test";
    {
        std::ofstream f(probe);
        if (!f.is_open()) return false;
    }
    fs::remove(probe, ec);
    return true;
}

std::string resolve_userdata_root(const std::string& cli_data, const char* argv0) {
    if (!cli_data.empty()) return cli_data;
    if (const char* rdata = std::getenv("RECOMPLAUNCHER_DATA")) {
        if (rdata[0]) return std::string(rdata);
    }

    std::error_code ec;
    fs::path cur = fs::current_path(ec);
    if (fs::exists(cur / "CMakeLists.txt", ec)) {
        return (cur / "userdata").string();
    }

    fs::path exe_dir = get_exe_dir(argv0);
    if (fs::exists(exe_dir / "portable.txt", ec) || fs::exists(exe_dir / "userdata", ec)) {
        return (exe_dir / "userdata").string();
    }

    std::string low_dir = exe_dir.string();
    for (char& c : low_dir) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#ifdef _WIN32
    bool sys_dir = (low_dir.find("program files") != std::string::npos);
    if (!sys_dir && is_dir_writable(exe_dir)) {
        return (exe_dir / "userdata").string();
    }
    if (const char* appdata = std::getenv("APPDATA")) {
        if (appdata[0]) return (fs::path(appdata) / "WhackedRecomp").string();
    }
#else
    bool sys_dir = (low_dir.rfind("/usr/", 0) == 0 || low_dir.rfind("/opt/", 0) == 0);
    if (!sys_dir && is_dir_writable(exe_dir)) {
        return (exe_dir / "userdata").string();
    }
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
        if (xdg[0]) return (fs::path(xdg) / "WhackedRecomp").string();
    }
    if (const char* home = std::getenv("HOME")) {
        if (home[0]) return (fs::path(home) / ".local" / "share" / "WhackedRecomp").string();
    }
#endif
    return (exe_dir / "userdata").string();
}

std::string resolve_initial_game_source(const std::string& explicit_iso, const fs::path& exe_dir, bool allow_discover) {
    std::error_code ec;
    if (!explicit_iso.empty() && fs::exists(explicit_iso, ec)) return explicit_iso;
    if (!allow_discover) return explicit_iso;
    auto found = xiso_auto_discover(exe_dir.string());
    if (!found.empty()) return found.front();
    return explicit_iso;
}

void print_help() {
    std::printf(
        "WhackedRecomp v%s - Whacked! (Original Xbox) Static Recompilation\n"
        "Usage: WhackedRecomp [options] [path-to-iso-or-folder]\n\n"
        "Setup & Launch:\n"
        "  --game, --iso <path>       Path to Whacked! .iso, .xiso.iso, Redump DVD, or extracted folder\n"
        "  --data, --userdata <dir>   Override save/config/cache/log directory (also RECOMPLAUNCHER_DATA)\n"
        "  --verify-iso <path>        Validate a disc image/folder for TitleID 0x4D530027 and exit (0=OK, 2=Bad, 3=WrongGame)\n"
        "  --launcher                 Open interactive launcher window even if skip_launcher is saved\n"
        "  --no-launcher              Launch straight into game without launcher UI\n"
        "  --version, -v              Print version information\n"
        "  --help, -h                 Show this help message\n\n"
        "Graphics & Display:\n"
        "  --renderer <api>           auto | d3d11 (Windows) | opengl | vulkan\n"
        "  --width <px> --height <px> Window resolution (e.g. 1280 720, 1920 1080)\n"
        "  --fullscreen <0|1>         Windowed or desktop fullscreen\n"
        "  --widescreen <0|1>         True 16:9 Hor+ widescreen (default: 1)\n"
        "  --hd720 <0|1>              Native 720p Xbox mode flag (default: 1)\n"
        "  --unstretched-hud <0|1>    Keep 2D HUD overlays 4:3 centered (default: 0)\n"
        "  --msaa <0|1>               4x MSAA anti-aliasing (default: 1)\n"
        "  --af <0|1>                 16x anisotropic filtering (default: 1)\n"
        "  --bloom <0|1>              Post-FX bloom & sharpen pass (default: 1)\n"
        "  --vsync <0|1>              Vertical sync (default: 1)\n"
        "  --fps-cap <N>              Frame rate limit (0 = uncapped, default: 60)\n\n"
        "Online Multiplayer (Virtual System Link):\n"
        "  --host <port>              Host a UDP tunnel room (generates WHK-... room code)\n"
        "  --join <code-or-ip:port>   Join a hosted room by WHK-... code or IP:Port\n",
        kAppVersion);
}

#ifdef _WIN32
void attach_cli_console() {
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* f = nullptr;
            freopen_s(&f, "CONOUT$", "w", stdout);
            freopen_s(&f, "CONOUT$", "w", stderr);
        }
    }
}

LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    std::fprintf(stderr, "\n[CRASH] Code=0x%08lX Addr=%p Mem=%p Flags=0x%llX\n",
                 ep->ExceptionRecord->ExceptionCode, addr,
                 g_xbox_mem_base,
                 ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0ull);
    log_note("[CRASH] Code=0x%08lX Addr=%p", ep->ExceptionRecord->ExceptionCode, addr);
    void* frames[32]{};
    WORD n = CaptureStackBackTrace(0, 32, frames, nullptr);
    char sym_buf[sizeof(SYMBOL_INFO) + 256]{};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    IMAGEHLP_LINE64 line{sizeof(line)};
    DWORD disp = 0;
    for (WORD i = 0; i < n; ++i) {
        DWORD64 a = reinterpret_cast<DWORD64>(frames[i]);
        DWORD64 sd = 0;
        const char* nm = SymFromAddr(proc, a, &sd, sym) ? sym->Name : "??";
        if (SymGetLineFromAddr64(proc, a, &disp, &line)) {
            std::fprintf(stderr, "  #%02u %s + 0x%llX (%s:%lu)\n", i, nm, sd, line.FileName, line.LineNumber);
            log_note("  #%02u %s + 0x%llX (%s:%lu)", i, nm, sd, line.FileName, line.LineNumber);
        } else {
            std::fprintf(stderr, "  #%02u %s + 0x%llX (0x%llX)\n", i, nm, sd, a);
            log_note("  #%02u %s + 0x%llX (0x%llX)", i, nm, sd, a);
        }
    }
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
void posix_crash_handler(int sig) {
    std::fprintf(stderr, "\n[CRASH] Signal %d Mem=%p\n", sig, g_xbox_mem_base);
    log_note("[CRASH] Signal %d", sig);
    void* bt[32]{};
    int n = backtrace(bt, 32);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    std::fflush(stderr);
    _exit(128 + sig);
}
#endif

void pump_sdl_events() {
    SDL_Event ev{};
    while (SDL_PollEvent(&ev)) {
        const bool quit_shortcut = ui_handle_quit_shortcut(&ev);
        ui_process_sdl_event(&ev);
        if (ev.type == SDL_QUIT) {
            g_quit_requested.store(true);
            break;
        } else if (ev.type == SDL_DROPFILE && ev.drop.file) {
            std::string dropped = ev.drop.file;
            SDL_free(ev.drop.file);
            if (ui_is_launcher_active()) {
                ui_notify_dropped_path(dropped);
            } else {
                ui_show_toast("Dropped: open Launcher before switching game image.");
            }
        } else if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            hle_input_poll_ui_snapshot();
        } else if (ev.type == SDL_WINDOWEVENT) {
            if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                int w = ev.window.data1, h = ev.window.data2;
                if (w > 0 && h > 0) {
                    app_config().width = w;
                    app_config().height = h;
                    gfx_resize(w, h);
                }
            }
        } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
            if (ev.key.keysym.scancode == SDL_SCANCODE_RETURN && (ev.key.keysym.mod & KMOD_ALT)) {
                toggle_fullscreen_window();
            } else if (!quit_shortcut && ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE &&
                       g_fullscreen_state && !ui_is_overlay_open() && !ui_is_launcher_active()) {
                toggle_fullscreen_window();
            }
        }
    }
    if (ui_consume_quit_requested()) {
        g_quit_requested.store(true);
    }
}

} // namespace

void app_request_quit() {
    g_quit_requested.store(true);
}

void app_on_frame_presented(uint64_t frame_num) {
    hle_input_set_frame(frame_num);
    if (frame_num == 120 || frame_num == 600 || frame_num == 1800) {
        log_note("[GFX] Frame %llu: %.1f FPS, %d draws",
                 static_cast<unsigned long long>(frame_num), gfx_get_fps(), gfx_get_last_draw_calls());
    }
    if (g_shot_frames.count(frame_num)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s/frame_%05llu.png",
                      g_shot_out_dir.c_str(), static_cast<unsigned long long>(frame_num));
        gfx_request_screenshot(buf);
    }
    if (g_shot_overlay_frame > 0 && !g_shot_overlay_path.empty()) {
        if (frame_num == g_shot_overlay_frame) {
            ui_set_overlay_open(true);
            gfx_request_screenshot(g_shot_overlay_path);
        } else if (frame_num == g_shot_overlay_frame + 1) {
            ui_set_overlay_open(false);
        }
    }
    if (g_max_frames > 0 && frame_num >= g_max_frames) {
        if (!g_final_shot_path.empty() && frame_num == g_max_frames) {
            gfx_request_screenshot(g_final_shot_path);
        }
        bool need_post_shot = (!g_final_shot_path.empty() ||
                               g_shot_frames.count(g_max_frames) != 0 ||
                               g_shot_overlay_frame == g_max_frames);
        if (!need_post_shot || frame_num > g_max_frames) {
            g_quit_requested.store(true);
        }
    }
}

int app_main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_handler);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#else
    signal(SIGSEGV, posix_crash_handler);
    signal(SIGABRT, posix_crash_handler);
#endif

    // Pre-scan --help / --version / --verify-iso / --data
    std::string cli_data;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { print_help(); return 0; }
        if (a == "--version" || a == "-v") {
            std::printf("WhackedRecomp %s (Original Xbox TitleID 0x4D530027)\n", kAppVersion);
            return 0;
        }
        if (a == "--verify-iso" && i + 1 < argc) {
            std::string path = argv[++i];
            XisoProbeResult pr = xiso_probe_game(path);
            std::printf("%s\n", pr.message.c_str());
            if (!pr.readable || !pr.is_xbox_media || !pr.has_xbe) return 2;
            return pr.is_whacked ? 0 : 3;
        }
        if ((a == "--data" || a == "--userdata" || a == "--portable-dir") && i + 1 < argc) {
            cli_data = argv[++i];
        }
    }

    fs::path exe_dir = get_exe_dir(argc > 0 ? argv[0] : nullptr);
    std::string userdata_root = resolve_userdata_root(cli_data, argc > 0 ? argv[0] : nullptr);
    init_rotating_log(userdata_root);
    hle_set_log_sink(log_sink_raw);

    bool had_config = app_config_load(userdata_root);
    auto& cfg = app_config();

    // Respect RecompLauncher environment integration
    const char* rl_active = std::getenv("RECOMPLAUNCHER");
    const char* rl_mode = std::getenv("RECOMPLAUNCHER_MODE");
    const char* rl_pad = std::getenv("RECOMPLAUNCHER_PAD");
    if (!had_config && rl_pad && std::string(rl_pad) == "playstation") {
        cfg.pad_preset = 1;
    }

    std::string cli_iso;
    bool cli_no_launcher = (rl_active && rl_active[0] == '1');
    bool cli_force_launcher = false;
    bool cli_auto_script = false;
    bool cli_fullscreen_set = false;
    bool cli_gfx_override = false;
    int cli_host_port = 0;
    std::string cli_join_code;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--iso" || a == "--game") && i + 1 < argc) cli_iso = argv[++i];
        else if ((a == "--data" || a == "--userdata" || a == "--portable-dir") && i + 1 < argc) ++i;
        else if (a == "--width" && i + 1 < argc) { cfg.width = std::atoi(argv[++i]); cli_gfx_override = true; }
        else if (a == "--height" && i + 1 < argc) { cfg.height = std::atoi(argv[++i]); cli_gfx_override = true; }
        else if (a == "--widescreen" && i + 1 < argc) { cfg.widescreen = (std::atoi(argv[++i]) != 0); cli_gfx_override = true; }
        else if (a == "--hd720" && i + 1 < argc) { cfg.hd720 = (std::atoi(argv[++i]) != 0); cli_gfx_override = true; }
        else if (a == "--unstretched-hud" && i + 1 < argc) { cfg.unstretched_hud = (std::atoi(argv[++i]) != 0); cli_gfx_override = true; }
        else if (a == "--msaa" && i + 1 < argc) { cfg.msaa_4x = (std::atoi(argv[++i]) != 0); cli_gfx_override = true; }
        else if (a == "--af" && i + 1 < argc) { cfg.aniso_16x = (std::atoi(argv[++i]) != 0); cli_gfx_override = true; }
        else if (a == "--bloom" && i + 1 < argc) { cfg.bloom_sharpen = (std::atoi(argv[++i]) != 0); cli_gfx_override = true; }
        else if (a == "--vsync" && i + 1 < argc) { cfg.vsync = (std::atoi(argv[++i]) != 0); cli_gfx_override = true; }
        else if (a == "--fps-cap" && i + 1 < argc) { cfg.fps_cap = std::atoi(argv[++i]); cli_gfx_override = true; }
        else if (a == "--fullscreen" && i + 1 < argc) { cfg.fullscreen = (std::atoi(argv[++i]) != 0); cli_fullscreen_set = true; cli_gfx_override = true; }
        else if (a == "--renderer" && i + 1 < argc) { cfg.renderer = argv[++i]; cli_gfx_override = true; }
        else if (a == "--launcher") cli_force_launcher = true;
        else if (a == "--no-launcher") cli_no_launcher = true;
        else if (a == "--host" && i + 1 < argc) cli_host_port = std::atoi(argv[++i]);
        else if (a == "--join" && i + 1 < argc) cli_join_code = argv[++i];
        else if (a == "--auto-input" && i + 1 < argc) { hle_input_set_script(argv[++i]); cli_auto_script = true; }
        else if (a == "--frames" && i + 1 < argc) g_max_frames = static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10));
        else if (a == "--screenshot" && i + 1 < argc) {
            std::string p = argv[++i];
            if (p.size() > 4 && p.substr(p.size() - 4) == ".png") g_final_shot_path = p;
            else g_shot_out_dir = p;
        } else if (a == "--shot-launcher" && i + 1 < argc) {
            g_shot_launcher_path = argv[++i];
        } else if (a == "--shot-overlay" && i + 1 < argc) {
            std::string spec = argv[++i];
            size_t col = spec.find(':');
            if (col != std::string::npos) {
                g_shot_overlay_frame = static_cast<uint64_t>(std::strtoull(spec.substr(0, col).c_str(), nullptr, 10));
                g_shot_overlay_path = spec.substr(col + 1);
            }
        } else if (a == "--shot-frames" && i + 1 < argc) {
            std::string list = argv[++i];
            size_t pos = 0;
            while (pos < list.size()) {
                size_t comma = list.find(',', pos);
                std::string tok = (comma == std::string::npos) ? list.substr(pos) : list.substr(pos, comma - pos);
                if (!tok.empty()) g_shot_frames.insert(std::strtoull(tok.c_str(), nullptr, 10));
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        } else if (a == "--trace") {
            g_hle_trace = true;
        } else if (!a.empty() && a[0] != '-' && cli_iso.empty()) {
            cli_iso = a;
        }
    }

    if (!cli_fullscreen_set && rl_mode && std::string(rl_mode) == "bigpicture") {
        cfg.fullscreen = true;
    }
    if (!cli_iso.empty()) cfg.iso_path = cli_iso;
    std::string iso_path = resolve_initial_game_source(cfg.iso_path, exe_dir, cli_iso.empty());
    cfg.iso_path = iso_path;

    std::printf("=== WhackedRecomp v%s (Xbox Original -> PC & Linux x64) ===\n", kAppVersion);
    std::printf("[BOOT] UserData: %s\n", userdata_root.c_str());
    std::printf("[BOOT] ISO Candidate: %s\n", iso_path.c_str());
    log_note("[BOOT] UserData=%s ISO=%s", userdata_root.c_str(), iso_path.c_str());

    if (!xbox_mem_init()) return 1;

    XisoProbeResult probe{};
    bool iso_ok = false;
    if (!iso_path.empty()) {
        probe = xiso_probe_game(iso_path);
        if (probe.is_whacked) {
            iso_ok = XisoVfs::instance().mount_iso(iso_path, userdata_root);
            if (iso_ok) {
                cfg.iso_path = XisoVfs::instance().mounted_source();
                if (!had_config) app_config_save(userdata_root);
            }
        } else {
            log_note("[ISO] Probe failed: %s", probe.message.c_str());
        }
    } else {
        probe.message = "No Whacked! disc image or extracted folder selected.";
    }

    bool headless_run = !cli_force_launcher &&
                        (g_max_frames > 0 || cli_auto_script || cli_no_launcher || (cfg.skip_launcher && iso_ok)) &&
                        g_shot_launcher_path.empty();
    if (headless_run && !iso_ok) {
        std::fprintf(stderr, "[FATAL] %s\n", probe.message.c_str());
        log_note("[FATAL] %s", probe.message.c_str());
        return (probe.has_xbe && !probe.is_whacked) ? 3 : 2;
    }

    SDL_SetMainReady();
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
#ifndef _WIN32
    if (void* x11 = ::dlopen("libX11.so.6", RTLD_LAZY | RTLD_GLOBAL)) {
        using XInitThreadsFn = int (*)();
        if (auto fn = reinterpret_cast<XInitThreadsFn>(::dlsym(x11, "XInitThreads"))) fn();
    }
#endif
#ifndef _WIN32
    if (cfg.renderer == "vulkan") {
        SDL_setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
        SDL_setenv("GALLIUM_DRIVER", "zink", 1);
    }
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "[FATAL] SDL_Init failed: %s\n", SDL_GetError());
        log_note("[FATAL] SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    bool use_gl = true;
#ifdef _WIN32
    use_gl = (cfg.renderer == "opengl");
#endif
    if (use_gl) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    }

    Uint32 win_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN;
    if (use_gl) win_flags |= SDL_WINDOW_OPENGL;
    if (cfg.fullscreen) {
        win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        g_fullscreen_state = true;
    }

    g_sdl_win = SDL_CreateWindow(
        "Whacked! [Recompiled x64] (F1 Overlay | F12 Screenshot | Alt+Enter Fullscreen)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        std::max(640, cfg.width), std::max(480, cfg.height), win_flags);
    if (!g_sdl_win) {
        std::fprintf(stderr, "[FATAL] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    if (!cfg.fullscreen) fit_window_to_display(g_sdl_win, cfg.width, cfg.height);
    SDL_GetWindowSize(g_sdl_win, &cfg.width, &cfg.height);

    // RecompLauncher-compatible cache path ({data}/cache/shaders), migrating legacy shader_cache if present
    fs::path new_cache = fs::path(userdata_root) / "cache" / "shaders";
    fs::path old_cache = fs::path(userdata_root) / "shader_cache";
    std::error_code ec;
    std::string chosen_cache = fs::exists(new_cache, ec) ? new_cache.string()
                             : (fs::exists(old_cache, ec) ? old_cache.string() : new_cache.string());

    RendererSettings rcfg{};
    rcfg.width           = std::max(640, cfg.width);
    rcfg.height          = std::max(480, cfg.height);
    rcfg.widescreen      = cfg.widescreen;
    rcfg.unstretched_hud = cfg.unstretched_hud;
    rcfg.msaa_4x         = cfg.msaa_4x;
    rcfg.aniso_16x       = cfg.aniso_16x;
    rcfg.bloom_sharpen   = cfg.bloom_sharpen;
    rcfg.vsync           = cfg.vsync;
    rcfg.fps_cap         = cfg.fps_cap;
    rcfg.show_fps        = cfg.show_fps;
    rcfg.backend         = cfg.renderer == "vulkan" ? GfxBackendKind::Vulkan : (use_gl ? GfxBackendKind::OpenGL : GfxBackendKind::D3D11);
    rcfg.screenshot_dir  = g_shot_out_dir;
    rcfg.cache_dir       = chosen_cache;

    if (!gfx_init(g_sdl_win, rcfg)) {
        std::fprintf(stderr, "[FATAL] Graphics backend initialization failed!\n");
        log_note("[FATAL] Graphics backend init failed");
        SDL_DestroyWindow(g_sdl_win);
        SDL_Quit();
        return 1;
    }

    ui_set_userdata_root(userdata_root);
    audio_set_mix_levels(cfg.master_volume, cfg.sfx_volume, cfg.music_volume, cfg.muted);

    if (cli_host_port > 0) {
        cfg.mp_mode = 1;
        cfg.mp_tunnel_port = cli_host_port;
        net_start_host(static_cast<uint16_t>(cli_host_port), cfg.mp_upnp, cfg.player_name);
    } else if (!cli_join_code.empty()) {
        cfg.mp_mode = 2;
        cfg.mp_join_code = cli_join_code;
        net_join_room(cli_join_code, cfg.player_name);
    } else if (cfg.mp_mode == 1) {
        net_start_host(static_cast<uint16_t>(cfg.mp_tunnel_port), cfg.mp_upnp, cfg.player_name);
    } else if (cfg.mp_mode == 2 && !cfg.mp_join_code.empty()) {
        net_join_room(cfg.mp_join_code, cfg.player_name);
    } else {
        net_start_offline_lan(cfg.player_name);
    }

    ui_set_iso_status(iso_path, iso_ok, probe.message);

    bool show_launcher = !headless_run || !g_shot_launcher_path.empty();
    if (show_launcher) {
        ui_set_launcher_active(true);
        uint64_t lframe = 0;
        while (!g_quit_requested.load()) {
            pump_sdl_events();
            if (g_quit_requested.load()) break;
            if (!g_shot_launcher_path.empty()) {
                if (lframe == 8) gfx_request_screenshot(g_shot_launcher_path);
                else if (lframe >= 10) {
                    gfx_shutdown();
                    ui_finish_network_setup();
                    net_shutdown();
                    audio_shutdown();
                    XisoVfs::instance().unmount();
                    SDL_DestroyWindow(g_sdl_win);
                    SDL_Quit();
                    return 0;
                }
            }
            if (ui_consume_launch_requested() && XisoVfs::instance().is_mounted()) {
                break;
            }
            gfx_present();
            ++lframe;
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        ui_set_launcher_active(false);
    }

    if (g_quit_requested.load()) {
        gfx_shutdown();
        ui_finish_network_setup();
        net_shutdown();
        audio_shutdown();
        XisoVfs::instance().unmount();
        SDL_DestroyWindow(g_sdl_win);
        SDL_Quit();
        return 0;
    }

    std::vector<uint8_t> xbe_bytes;
    if (!XisoVfs::instance().read_iso_file("default.xbe", xbe_bytes)) {
        std::fprintf(stderr, "[FATAL] default.xbe not found inside game source\n");
        log_note("[FATAL] default.xbe missing inside game source");
        gfx_shutdown();
        ui_finish_network_setup();
        net_shutdown();
        return 2;
    }
    if (!xbox_load_xbe_from_memory(xbe_bytes.data(), xbe_bytes.size())) return 2;
    if (g_xbe_info.title_id != 0x4D530027u) {
        std::fprintf(stderr, "[FATAL] TitleID 0x%08X does not match Whacked! (0x4D530027)\n", g_xbe_info.title_id);
        log_note("[FATAL] Wrong TitleID 0x%08X", g_xbe_info.title_id);
        gfx_shutdown();
        ui_finish_network_setup();
        net_shutdown();
        return 3;
    }

    if (cfg.hd720 && cfg.widescreen && cfg.width >= 1280) {
        hle_d3d8_set_hd720(true);
        wr32(0x0037D900u, 4u);
    } else {
        hle_d3d8_set_hd720(false);
    }

    recomp_register_all();
    hle_set_thread_exit_hook(gfx_release_context_for_game);
    gfx_release_context_for_game();

    rl_ach_set_info("whackedrecomp", kAppVersion);
    rl_ach_on_unlock([](uint32_t, void*) {
        if (!rl_ach_quiet_toasts()) {
            ui_show_toast("Achievement Unlocked!", 4.0);
        }
    }, nullptr);
    rl_ach_init("xbox", [](uint32_t addr, uint8_t* buf, uint32_t sz, void*) -> uint32_t {
        if (!g_xbox_mem_base || !buf || sz == 0 || addr >= XBOX_MEM_SIZE) return 0;
        uint32_t avail = std::min(sz, XBOX_MEM_SIZE - addr);
        uint32_t copied = 0;
        if (addr < 0x1000u) {
            uint32_t z = std::min(avail, 0x1000u - addr);
            std::memset(buf, 0, z);
            copied += z;
            addr += z;
            buf += z;
            avail -= z;
        }
        if (avail > 0) {
            std::memcpy(buf, mem_ptr(addr), avail);
            copied += avail;
        }
        return copied;
    }, nullptr);
    rl_ach_presence("Starting Whacked!");

    uint32_t stack_size = 0x00200000u;
    uint32_t stack_base = xbox_guest_alloc(stack_size, 4096u, true);
    uint32_t teb0 = xbox_alloc_teb(stack_base + stack_size, stack_base, 0x400u, 1);

    std::thread entry_thr([=]() {
        CpuContext ctx{};
        ctx.fs_base = teb0;
        ctx.esp = stack_base + stack_size - 64u;
        ctx.push32(0xDEAD0000u);
        std::printf("[BOOT] Entering XBE Entry 0x%08X...\n", g_xbe_info.entry_va);
        log_note("[BOOT] Entering XBE Entry 0x%08X", g_xbe_info.entry_va);
        try {
            recomp_dispatch_call(ctx, g_xbe_info.entry_va);
        } catch (const GuestPowerOff&) {
            gfx_release_context_for_game();
            app_request_quit();
            return;
        }
        gfx_release_context_for_game();
        std::printf("[BOOT] XbeEntry returned cleanly (main game thread spawned).\n");
    });

    auto last_beat = std::chrono::steady_clock::now();
    uint64_t last_seq = hle_kernel_get_stall_seq();
    bool dumped_stall = false;
    while (!g_quit_requested.load()) {
        pump_sdl_events();
        if (g_quit_requested.load()) break;

        auto now = std::chrono::steady_clock::now();
        uint64_t seq = hle_kernel_get_stall_seq();
        if (seq != last_seq) {
            last_seq = seq;
            last_beat = now;
            dumped_stall = false;
        } else if (seq > 0 && !dumped_stall &&
                   std::chrono::duration_cast<std::chrono::milliseconds>(now - last_beat).count() >= 4000) {
            dumped_stall = true;
            hle_kernel_dump_stall_state();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    if (!cli_gfx_override && g_max_frames == 0 && !cli_auto_script && g_shot_launcher_path.empty() &&
        g_shot_overlay_path.empty() && g_final_shot_path.empty()) {
        app_config_save(userdata_root);
    }
    log_note("[BOOT] Shutting down");
    hle_kernel_request_shutdown();
    hle_bink_shutdown();
    if (entry_thr.joinable()) entry_thr.join();
    if (!hle_kernel_wait_for_guest_threads(2000)) {
        log_note("[BOOT] Warning: guest threads still active after 2000ms shutdown wait");
    }
    hle_input_shutdown();
    rl_ach_shutdown();
    audio_shutdown();
    hle_kernel_shutdown();
    ui_finish_network_setup();
    net_shutdown();
    gfx_shutdown();
    XisoVfs::instance().unmount();
    SDL_DestroyWindow(g_sdl_win);
    SDL_Quit();
    log_note("[BOOT] Clean shutdown");
    std::fflush(stdout);
    std::fflush(stderr);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 0);
#else
    _exit(0);
#endif
    return 0;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    attach_cli_console();
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> storage;
    std::vector<char*> argv_ptrs;
    storage.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(len > 0 ? len - 1 : 0, '\0');
        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), len, nullptr, nullptr);
        storage.push_back(std::move(s));
    }
    if (wargv) LocalFree(wargv);
    for (auto& s : storage) argv_ptrs.push_back(s.data());
    return app_main(argc, argv_ptrs.data());
}
#else
int main(int argc, char** argv) {
    return app_main(argc, argv);
}
#endif
