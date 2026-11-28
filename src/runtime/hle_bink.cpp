#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
using ProcHandle = HANDLE;
using PipeHandle = HANDLE;
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
using ProcHandle = pid_t;
using PipeHandle = int;
#endif
#include "hle_common.h"
#include "launcher_ui.h"
#include "xiso_vfs.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

// Declared in hle_audio.cpp to play decoded Bink PCM stereo 44.1kHz stream
extern uint32_t audio_bink_open_stream(uint32_t sample_rate, uint32_t channels);
extern void     audio_bink_push_pcm(uint32_t stream_id, const int16_t* samples, size_t byte_len);
extern void     audio_bink_set_volume(uint32_t stream_id, float vol_0_to_1);
extern void     audio_bink_pause(uint32_t stream_id, bool paused);
extern void     audio_bink_close(uint32_t stream_id);
extern void     audio_bink_flush(uint32_t stream_id);
extern double   audio_bink_get_clock_sec(uint32_t stream_id);

namespace {

struct BinkStreamState {
    uint32_t guest_va = 0;
    uint32_t width = 640;
    uint32_t height = 480;
    uint32_t total_frames = 1;
    uint32_t cur_frame = 1; // 1-based
    double   fps = 29.97;
    bool     has_alpha = false;
    bool     has_audio = false;
    uint32_t num_audio_tracks = 0;
    uint32_t audio_track = 0;
    bool     video_on = true;
    bool     sound_on = true;
    bool     paused = false;
    float    volume = 1.0f;
    std::string tmp_bik_path;
    uint32_t audio_stream_id = 0;
    bool     audio_started = false;
    std::atomic<bool> aud_eof{false};

#ifdef _WIN32
    ProcHandle vid_proc = nullptr;
    PipeHandle vid_pipe = nullptr;
    ProcHandle aud_proc = nullptr;
    PipeHandle aud_pipe = nullptr;
#else
    ProcHandle vid_proc = -1;
    PipeHandle vid_pipe = -1;
    ProcHandle aud_proc = -1;
    PipeHandle aud_pipe = -1;
#endif
    std::thread vid_thread;
    std::thread aud_thread;
    std::atomic<bool> stop_flag{false};

    std::mutex frame_mtx;
    std::condition_variable frame_cv;
    std::vector<uint8_t> cur_bgra;
    std::vector<uint8_t> next_bgra;
    std::vector<std::vector<uint8_t>> cached_frames;
    bool cache_enabled = false;
    bool cache_complete = false;
    bool next_ready = false;
    bool eof_reached = false;
    bool clock_started = false;
    double seek_base_sec = 0.0;
    double last_aud_sec = 0.0;
    bool audio_handoff_done = false;
    std::chrono::steady_clock::time_point clock_origin{};
    std::chrono::steady_clock::time_point paused_at_tp{};
    std::chrono::steady_clock::time_point last_wait_tp{};
    uint32_t wait_spins = 0;
};

std::mutex g_bink_mtx;
std::unordered_map<uint32_t, BinkStreamState*> g_binks;
std::atomic<uint32_t> g_bink_id{1};
uint32_t g_pending_fps_num = 0;
uint32_t g_pending_fps_den = 0;
uint32_t g_pending_track_count = 1;
uint32_t g_pending_track_0 = 0;

double compute_playback_elapsed_sec(BinkStreamState* s, std::chrono::steady_clock::time_point now) {
    double wall_sec = s->seek_base_sec + std::chrono::duration<double>(now - s->clock_origin).count();
    if (s->audio_stream_id && s->sound_on && s->has_audio && s->audio_started) {
        if (!s->aud_eof.load(std::memory_order_acquire)) {
            double aud_rel = audio_bink_get_clock_sec(s->audio_stream_id);
            if (aud_rel > 0.005) {
                s->last_aud_sec = aud_rel;
                return s->seek_base_sec + aud_rel;
            }
        } else if (!s->audio_handoff_done && s->last_aud_sec > 0.0) {
            s->clock_origin = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(s->last_aud_sec));
            s->audio_handoff_done = true;
            return s->seek_base_sec + s->last_aud_sec;
        }
    }
    return wall_sec;
}

std::string find_ffmpeg() {
#ifdef _WIN32
    char exe_buf[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, exe_buf, MAX_PATH) > 0) {
        fs::path ed = fs::path(exe_buf).parent_path();
        for (const fs::path& cand : {ed / "ffmpeg.exe", ed / "bin" / "ffmpeg.exe"}) {
            std::error_code ec;
            if (fs::exists(cand, ec)) return cand.string();
        }
    }
    char buf[MAX_PATH]{};
    if (SearchPathA(nullptr, "ffmpeg.exe", nullptr, MAX_PATH, buf, nullptr)) {
        return std::string(buf);
    }
    return "ffmpeg.exe";
#else
    std::error_code ec;
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && self.has_parent_path()) {
        fs::path ed = self.parent_path();
        for (const fs::path& cand : {ed / "ffmpeg", ed / "bin" / "ffmpeg"}) {
            if (::access(cand.c_str(), X_OK) == 0) return cand.string();
        }
    }
    const char* candidates[] = {"/usr/bin/ffmpeg", "/usr/local/bin/ffmpeg"};
    for (const char* c : candidates) {
        if (::access(c, X_OK) == 0) return c;
    }
    return "ffmpeg";
#endif
}

bool pipe_read_chunk(PipeHandle h, void* dst, size_t max_bytes, size_t* out_got) {
    *out_got = 0;
#ifdef _WIN32
    DWORD n = 0;
    if (!ReadFile(h, dst, static_cast<DWORD>(max_bytes), &n, nullptr) || n == 0) return false;
    *out_got = n;
    return true;
#else
    while (true) {
        ssize_t r = ::read(h, dst, max_bytes);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        *out_got = static_cast<size_t>(r);
        return true;
    }
#endif
}

bool spawn_ffmpeg_pipe(const std::vector<std::string>& argv_list, ProcHandle& out_proc, PipeHandle& out_read_pipe) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 4 * 1024 * 1024)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = nul;

    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + find_ffmpeg() + "\"";
    for (const auto& a : argv_list) {
        bool has_space = a.find(' ') != std::string::npos;
        cmd += ' ';
        if (has_space) cmd += '"';
        cmd += a;
        if (has_space) cmd += '"';
    }
    std::vector<char> mut_cmd(cmd.begin(), cmd.end());
    mut_cmd.push_back('\0');

    BOOL ok = CreateProcessA(nullptr, mut_cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }
    CloseHandle(pi.hThread);
    out_proc = pi.hProcess;
    out_read_pipe = rd;
    return true;
#else
    std::string exe = find_ffmpeg();
    int fds[2]{-1, -1};
    if (::pipe(fds) != 0) return false;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }
    if (pid == 0) {
        ::dup2(fds[1], STDOUT_FILENO);
        int nul = ::open("/dev/null", O_RDWR);
        if (nul >= 0) {
            ::dup2(nul, STDIN_FILENO);
            ::dup2(nul, STDERR_FILENO);
            if (nul > 2) ::close(nul);
        }
        ::close(fds[0]);
        ::close(fds[1]);
        std::vector<char*> cargs;
        cargs.push_back(const_cast<char*>(exe.c_str()));
        for (const auto& a : argv_list) cargs.push_back(const_cast<char*>(a.c_str()));
        cargs.push_back(nullptr);
        ::execvp(exe.c_str(), cargs.data());
        ::_exit(127);
    }
    ::close(fds[1]);
    out_proc = pid;
    out_read_pipe = fds[0];
    return true;
#endif
}

void start_ffmpeg_decoders(BinkStreamState* s, double start_sec = 0.0) {
    s->stop_flag = false;
    s->eof_reached = false;
    s->next_ready = false;
    s->clock_started = false;
    s->audio_started = false;
    s->aud_eof = false;
    s->audio_handoff_done = false;
    s->last_aud_sec = 0.0;
    s->seek_base_sec = std::max(0.0, start_sec);
    s->wait_spins = 0;

    std::vector<std::string> base_args = {"-loglevel", "quiet"};
    if (start_sec > 0.01) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", start_sec);
        base_args.push_back("-ss");
        base_args.push_back(buf);
    }
    base_args.push_back("-i");
    base_args.push_back(s->tmp_bik_path);

    // Video BGRA pipe
    std::vector<std::string> vargs = base_args;
    vargs.insert(vargs.end(), {"-map", "0:v:0", "-f", "rawvideo", "-pix_fmt", "bgra", "-"});
    if (spawn_ffmpeg_pipe(vargs, s->vid_proc, s->vid_pipe)) {
        size_t frame_bytes = static_cast<size_t>(s->width) * s->height * 4u;
        if (s->cur_bgra.size() != frame_bytes) s->cur_bgra.assign(frame_bytes, 0);
        s->next_bgra.assign(frame_bytes, 0);
        s->vid_thread = std::thread([s, frame_bytes]() {
            std::vector<uint8_t> tmp(frame_bytes);
            while (!s->stop_flag.load()) {
                size_t got = 0;
                while (got < frame_bytes && !s->stop_flag.load()) {
                    size_t n = 0;
                    if (!pipe_read_chunk(s->vid_pipe, tmp.data() + got, frame_bytes - got, &n) || n == 0) {
                        goto vdone;
                    }
                    got += n;
                }
                if (got == frame_bytes) {
                    if (s->cache_enabled && !s->cache_complete) {
                        std::lock_guard<std::mutex> lk(s->frame_mtx);
                        s->cached_frames.push_back(tmp);
                    }
                    std::unique_lock<std::mutex> lk(s->frame_mtx);
                    s->frame_cv.wait(lk, [s]() { return !s->next_ready || s->stop_flag.load(); });
                    if (s->stop_flag.load()) break;
                    s->next_bgra.swap(tmp);
                    s->next_ready = true;
                    s->frame_cv.notify_all();
                }
            }
        vdone:
            {
                std::lock_guard<std::mutex> lk(s->frame_mtx);
                s->eof_reached = true;
                if (s->cache_enabled && !s->cached_frames.empty()) {
                    s->cache_complete = true;
                    if (!s->tmp_bik_path.empty()) {
                        std::error_code ec;
                        fs::remove(s->tmp_bik_path, ec);
                    }
                }
                s->frame_cv.notify_all();
            }
        });
    } else {
        {
            std::lock_guard<std::mutex> lk(s->frame_mtx);
            s->eof_reached = true;
            s->frame_cv.notify_all();
        }
        static std::atomic<bool> s_warned{false};
        if (!s_warned.exchange(true)) {
            std::fprintf(stderr, "[BINK] Warning: ffmpeg not found (%s); FMV playback skipped cleanly.\n", find_ffmpeg().c_str());
            hle_log_note("[BINK] Warning: ffmpeg not found (%s); FMV playback skipped cleanly.", find_ffmpeg().c_str());
            ui_show_toast("ffmpeg not found: FMV videos skipped cleanly.", 5.0);
        }
    }

    // Audio s16le stereo 44100 Hz pipe (skip silent Binks cleanly)
    if (s->total_frames > 1 && s->has_audio) {
        s->audio_stream_id = audio_bink_open_stream(44100, 2);
        audio_bink_set_volume(s->audio_stream_id, s->sound_on ? s->volume : 0.0f);
        audio_bink_pause(s->audio_stream_id, true);

        std::vector<std::string> aargs = base_args;
        aargs.insert(aargs.end(), {"-map", "0:a:" + std::to_string(s->audio_track),
                                   "-f", "s16le", "-ac", "2", "-ar", "44100", "-"});
        if (spawn_ffmpeg_pipe(aargs, s->aud_proc, s->aud_pipe)) {
            uint32_t sid = s->audio_stream_id;
            s->aud_thread = std::thread([s, sid]() {
                uint8_t raw[8196];
                size_t carry = 0;
                while (!s->stop_flag.load()) {
                    size_t n = 0;
                    if (!pipe_read_chunk(s->aud_pipe, raw + carry, sizeof(raw) - carry, &n) || n == 0) break;
                    size_t total = carry + n;
                    size_t aligned = total & ~size_t(3);
                    if (aligned > 0) {
                        audio_bink_push_pcm(sid, reinterpret_cast<const int16_t*>(raw), aligned);
                    }
                    carry = total - aligned;
                    if (carry > 0) {
                        std::memmove(raw, raw + aligned, carry);
                    }
                }
                s->aud_eof.store(true, std::memory_order_release);
            });
        } else {
            audio_bink_close(s->audio_stream_id);
            s->audio_stream_id = 0;
        }
    }
    s->clock_origin = std::chrono::steady_clock::now();
}

void stop_ffmpeg_decoders(BinkStreamState* s, bool delete_tmp) {
    s->stop_flag = true;
    s->frame_cv.notify_all();
    if (s->audio_stream_id) { audio_bink_close(s->audio_stream_id); s->audio_stream_id = 0; }
#ifdef _WIN32
    if (s->vid_pipe) { CancelIoEx(s->vid_pipe, nullptr); CloseHandle(s->vid_pipe); s->vid_pipe = nullptr; }
    if (s->aud_pipe) { CancelIoEx(s->aud_pipe, nullptr); CloseHandle(s->aud_pipe); s->aud_pipe = nullptr; }
    if (s->vid_proc) { TerminateProcess(s->vid_proc, 0); WaitForSingleObject(s->vid_proc, 500); CloseHandle(s->vid_proc); s->vid_proc = nullptr; }
    if (s->aud_proc) { TerminateProcess(s->aud_proc, 0); WaitForSingleObject(s->aud_proc, 500); CloseHandle(s->aud_proc); s->aud_proc = nullptr; }
#else
    if (s->vid_pipe >= 0) { ::close(s->vid_pipe); s->vid_pipe = -1; }
    if (s->aud_pipe >= 0) { ::close(s->aud_pipe); s->aud_pipe = -1; }
    if (s->vid_proc > 0) { ::kill(s->vid_proc, SIGKILL); ::waitpid(s->vid_proc, nullptr, 0); s->vid_proc = -1; }
    if (s->aud_proc > 0) { ::kill(s->aud_proc, SIGKILL); ::waitpid(s->aud_proc, nullptr, 0); s->aud_proc = -1; }
#endif
    if (s->vid_thread.joinable()) s->vid_thread.join();
    if (s->aud_thread.joinable()) s->aud_thread.join();
    if (delete_tmp && !s->tmp_bik_path.empty()) {
        std::error_code ec;
        fs::remove(s->tmp_bik_path, ec);
    }
}

BinkStreamState* find_bink(uint32_t bnk_va) {
    std::lock_guard<std::mutex> lk(g_bink_mtx);
    auto it = g_binks.find(bnk_va);
    return it != g_binks.end() ? it->second : nullptr;
}

void sync_guest_bink(BinkStreamState* s) {
    if (!s || !s->guest_va) return;
    wr32(s->guest_va + 0x00u, s->width);
    wr32(s->guest_va + 0x04u, s->height);
    wr32(s->guest_va + 0x08u, s->total_frames);
    wr32(s->guest_va + 0x0Cu, s->cur_frame);
    wr32(s->guest_va + 0x10u, s->total_frames > 0 ? s->total_frames - 1u : 0);
    wr32(s->guest_va + 0xC4u, s->has_alpha ? 1u : 0u);
    wr32(s->guest_va + 0x284u, s->paused ? 1u : 0u);
}

void hle_BinkSetSoundSystem(CpuContext& ctx) { hle_ret_u32(ctx, 1, 8); }
void hle_BinkSetFrameRate(CpuContext& ctx) {
    uint32_t rate = hle_arg(ctx, 0);
    uint32_t div  = hle_arg(ctx, 1);
    {
        std::lock_guard<std::mutex> lk(g_bink_mtx);
        g_pending_fps_num = rate;
        g_pending_fps_den = div ? div : 1u;
    }
    hle_ret(ctx, 8);
}
void hle_BinkSetSoundTrack(CpuContext& ctx) {
    uint32_t total     = hle_arg(ctx, 0);
    uint32_t tracks_va = hle_arg(ctx, 1);
    {
        std::lock_guard<std::mutex> lk(g_bink_mtx);
        g_pending_track_count = std::min<uint32_t>(total, 8u);
        g_pending_track_0 = 0u;
        if (g_pending_track_count > 0 && tracks_va && mem_ptr(tracks_va)) {
            uint32_t t0 = rd32(tracks_va);
            // Multi-track surround arrays in Whacked may pass [base..base+5] where track 0 is stereo mixdown.
            g_pending_track_0 = (g_pending_track_count > 1u && t0 <= 1u) ? 0u : t0;
        }
    }
    hle_ret(ctx, 8);
}
void hle_BinkSetIO(CpuContext& ctx)          { hle_ret(ctx, 8); }

void hle_BinkOpen(CpuContext& ctx) {
    uint32_t name_va = hle_arg(ctx, 0);
    uint32_t flags   = hle_arg(ctx, 1);
    std::vector<uint8_t> bik_bytes;
    std::string desc_name;
    bool loaded = false;

    if ((flags & 0x00800000u) != 0) {
        // BINKFILEHANDLE: name_va is an open Xbox HANDLE seeked to start of BIK data (e.g. inside Movies.pak)
        uint64_t off = 0;
        loaded = hle_file_read_bink_from_handle(name_va, bik_bytes, &off);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "HANDLE(0x%X@%llu)", name_va, static_cast<unsigned long long>(off));
        desc_name = buf;
    } else if ((flags & 0x04000000u) != 0) {
        // BINKFROMMEMORY: name_va points directly to BIK header in guest RAM
        if (name_va && name_va + 48u < XBOX_MEM_SIZE && std::memcmp(mem_ptr(name_va), "BIK", 3) == 0) {
            uint32_t sz = rd32(name_va + 4u) + 8u;
            if (sz >= 48u && name_va + sz <= XBOX_MEM_SIZE) {
                bik_bytes.assign(mem_ptr(name_va), mem_ptr(name_va) + sz);
                loaded = true;
            }
        }
        char buf[48];
        std::snprintf(buf, sizeof(buf), "MEM(0x%08X)", name_va);
        desc_name = buf;
    } else {
        desc_name = xbox_read_cstr(name_va, 260);
        std::string dev, rel;
        if (XisoVfs::instance().resolve_path(desc_name, dev, rel)) {
            if (dev == "iso") {
                loaded = XisoVfs::instance().read_iso_file(rel, bik_bytes);
            } else {
                std::string hp = XisoVfs::instance().host_path_for(dev, rel);
                FILE* f = std::fopen(hp.c_str(), "rb");
                if (f) {
                    std::fseek(f, 0, SEEK_END);
                    long sz = std::ftell(f);
                    std::fseek(f, 0, SEEK_SET);
                    if (sz > 48) {
                        bik_bytes.resize(static_cast<size_t>(sz));
                        loaded = (std::fread(bik_bytes.data(), 1, bik_bytes.size(), f) == bik_bytes.size());
                    }
                    std::fclose(f);
                }
            }
        }
    }

    if (!loaded || bik_bytes.size() < 48 || std::memcmp(bik_bytes.data(), "BIK", 3) != 0) {
        std::fprintf(stderr, "[BINK] Failed to open %s (flags=0x%08X)\n", desc_name.c_str(), flags);
        hle_ret_u32(ctx, 0, 8);
        return;
    }

    auto u32le = [&](size_t off) -> uint32_t {
        uint32_t v; std::memcpy(&v, bik_bytes.data() + off, 4); return v;
    };

    uint32_t req_fps_num = 0, req_fps_den = 0;
    uint32_t req_track_count = 1, req_track_0 = 0;
    {
        std::lock_guard<std::mutex> lk(g_bink_mtx);
        if ((flags & 0x00001000u) != 0) {
            req_fps_num = g_pending_fps_num;
            req_fps_den = g_pending_fps_den;
        }
        g_pending_fps_num = 0;
        g_pending_fps_den = 0;
        if ((flags & 0x00004000u) != 0) {
            req_track_count = g_pending_track_count;
            req_track_0 = g_pending_track_0;
        }
        g_pending_track_count = 1u;
        g_pending_track_0 = 0u;
    }

    auto* s = new BinkStreamState();
    s->total_frames = std::max(1u, u32le(0x08));
    s->width        = std::max(16u, u32le(0x14));
    s->height       = std::max(16u, u32le(0x18));
    uint32_t fps_n  = (req_fps_num && req_fps_den) ? req_fps_num : u32le(0x1C);
    uint32_t fps_d  = (req_fps_num && req_fps_den) ? req_fps_den : u32le(0x20);
    s->fps          = (fps_n && fps_d) ? (static_cast<double>(fps_n) / fps_d) : 29.97;
    if (s->fps < 5.0 || s->fps > 120.0) s->fps = 29.97;
    s->has_alpha    = ((u32le(0x24) & 0x00100000u) != 0) || ((flags & 0x00100000u) != 0);
    uint32_t num_audio = (bik_bytes.size() >= 0x2C) ? u32le(0x28) : 0u;
    s->num_audio_tracks = (num_audio <= 16u) ? num_audio : 0u;
    s->has_audio    = (s->num_audio_tracks > 0 && req_track_count > 0);
    s->audio_track  = (s->has_audio && req_track_0 < s->num_audio_tracks) ? req_track_0 : 0u;
    uint64_t total_raw_bytes = static_cast<uint64_t>(s->width) * s->height * 4ull * s->total_frames;
    s->cache_enabled = (total_raw_bytes <= 24ull * 1024ull * 1024ull);

    uint32_t id = g_bink_id.fetch_add(1u);
    s->tmp_bik_path = XisoVfs::instance().host_path_for("Z", "_bink_" + std::to_string(id) + ".bik");
    {
        FILE* f = std::fopen(s->tmp_bik_path.c_str(), "wb");
        if (f) {
            std::fwrite(bik_bytes.data(), 1, bik_bytes.size(), f);
            std::fclose(f);
        }
    }

    s->guest_va = xbox_guest_alloc(0x400u, 16u, true);
    s->cur_frame = 1;
    sync_guest_bink(s);
    {
        std::lock_guard<std::mutex> lk(g_bink_mtx);
        g_binks[s->guest_va] = s;
    }
    start_ffmpeg_decoders(s, 0.0);
    std::printf("[BINK] Opened %s (%ux%u, %u frames @ %.2f fps, alpha=%d)\n",
                desc_name.c_str(), s->width, s->height, s->total_frames, s->fps, s->has_alpha ? 1 : 0);
    hle_ret_u32(ctx, s->guest_va, 8);
}

void hle_BinkClose(CpuContext& ctx) {
    uint32_t va = hle_arg(ctx, 0);
    BinkStreamState* s = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_bink_mtx);
        auto it = g_binks.find(va);
        if (it != g_binks.end()) { s = it->second; g_binks.erase(it); }
    }
    if (s) {
        stop_ffmpeg_decoders(s, true);
        delete s;
        xbox_guest_free(va);
    }
    hle_ret(ctx, 4);
}

void hle_BinkWait(CpuContext& ctx) {
    BinkStreamState* s = find_bink(hle_arg(ctx, 0));
    if (!s || s->paused) { hle_ret_u32(ctx, 1, 4); return; }
    if (s->total_frames <= 1 || !s->clock_started) {
        s->wait_spins = 0;
        hle_ret_u32(ctx, 0, 4);
        return;
    }
    auto now = std::chrono::steady_clock::now();
    double elapsed_sec = compute_playback_elapsed_sec(s, now);
    double target_sec = static_cast<double>(s->cur_frame > 0 ? s->cur_frame - 1u : 0u) / s->fps;
    if (elapsed_sec + 0.0005 < target_sec) {
        if (s->last_wait_tp.time_since_epoch().count() != 0 &&
            std::chrono::duration<double, std::milli>(now - s->last_wait_tp).count() < 1.0) {
            if (++s->wait_spins >= 32u) {
                s->wait_spins = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } else {
            s->wait_spins = 0;
        }
        s->last_wait_tp = now;
        hle_ret_u32(ctx, 1, 4);
        return;
    }
    s->wait_spins = 0;
    hle_ret_u32(ctx, 0, 4);
}

void hle_BinkDoFrame(CpuContext& ctx) {
    BinkStreamState* s = find_bink(hle_arg(ctx, 0));
    if (!s) { hle_ret_u32(ctx, 0, 4); return; }
    {
        std::unique_lock<std::mutex> lk(s->frame_mtx);
        if (s->cache_complete && !s->cached_frames.empty()) {
            size_t idx = (s->cur_frame >= 1 && s->cur_frame <= s->cached_frames.size()) ? (s->cur_frame - 1) : 0;
            s->cur_bgra = s->cached_frames[idx];
        } else {
            if (!s->next_ready && !s->eof_reached) {
                s->frame_cv.wait_for(lk, std::chrono::milliseconds(50),
                                     [s]() { return s->next_ready || s->eof_reached || s->stop_flag.load(); });
            }
            if (s->next_ready) {
                s->cur_bgra.swap(s->next_bgra);
                s->next_ready = false;
                s->frame_cv.notify_all();
            }
        }
    }
    if (!s->clock_started && !s->paused) {
        s->clock_origin = std::chrono::steady_clock::now();
        s->clock_started = true;
        s->audio_started = true;
        if (s->audio_stream_id) audio_bink_pause(s->audio_stream_id, false);
    }
    hle_ret_u32(ctx, 0, 4);
}

void hle_BinkCopyToBuffer(CpuContext& ctx) {
    BinkStreamState* s = find_bink(hle_arg(ctx, 0));
    uint32_t dest_va  = hle_arg(ctx, 1);
    int32_t  pitch    = static_cast<int32_t>(hle_arg(ctx, 2));
    uint32_t dest_h   = hle_arg(ctx, 3);
    uint32_t dx       = hle_arg(ctx, 4);
    uint32_t dy       = hle_arg(ctx, 5);
    uint32_t flags    = hle_arg(ctx, 6);
    if (!s || !dest_va) { hle_ret_u32(ctx, 0, 28); return; }

    uint32_t fmt = flags & 0xFFu;
    uint32_t copy_w = s->width;
    uint32_t copy_h = std::min(s->height, dest_h ? dest_h : s->height);
    std::lock_guard<std::mutex> lk(s->frame_mtx);
    if (s->cur_bgra.empty()) { hle_ret_u32(ctx, 0, 28); return; }

    uint8_t* dst_base = mem_ptr(dest_va);
    for (uint32_t y = 0; y < copy_h; ++y) {
        const uint8_t* src = s->cur_bgra.data() + static_cast<size_t>(y) * s->width * 4u;
        uint8_t* dst = dst_base + static_cast<ptrdiff_t>(dy + y) * pitch;
        if (fmt == 3 || fmt == 0) {
            // 32-bit BGRX (force opaque alpha = 255)
            uint32_t* d32 = reinterpret_cast<uint32_t*>(dst) + dx;
            const uint32_t* s32 = reinterpret_cast<const uint32_t*>(src);
            for (uint32_t x = 0; x < copy_w; ++x) d32[x] = s32[x] | 0xFF000000u;
        } else if (fmt == 5) {
            // 32-bit BGRA (preserve alpha)
            std::memcpy(dst + dx * 4u, src, copy_w * 4u);
        } else if (fmt == 1 || fmt == 2) {
            // 24-bit BGR/RGB
            uint8_t* d24 = dst + dx * 3u;
            for (uint32_t x = 0; x < copy_w; ++x) {
                d24[x * 3 + 0] = (fmt == 1) ? src[x * 4 + 0] : src[x * 4 + 2];
                d24[x * 3 + 1] = src[x * 4 + 1];
                d24[x * 3 + 2] = (fmt == 1) ? src[x * 4 + 2] : src[x * 4 + 0];
            }
        } else if (fmt == 8) {
            // YUY2 (16-bit macroblocks)
            uint8_t* dyuv = dst + dx * 2u;
            for (uint32_t x = 0; x + 1 < copy_w; x += 2) {
                int r0 = src[x*4+2], g0 = src[x*4+1], b0 = src[x*4+0];
                int r1 = src[(x+1)*4+2], g1 = src[(x+1)*4+1], b1 = src[(x+1)*4+0];
                int y0 = ((66*r0 + 129*g0 + 25*b0 + 128) >> 8) + 16;
                int y1 = ((66*r1 + 129*g1 + 25*b1 + 128) >> 8) + 16;
                int rm = (r0 + r1) >> 1, gm = (g0 + g1) >> 1, bm = (b0 + b1) >> 1;
                int u  = ((-38*rm - 74*gm + 112*bm + 128) >> 8) + 128;
                int v  = ((112*rm - 94*gm - 18*bm + 128) >> 8) + 128;
                dyuv[x*2 + 0] = static_cast<uint8_t>(std::clamp(y0, 0, 255));
                dyuv[x*2 + 1] = static_cast<uint8_t>(std::clamp(u, 0, 255));
                dyuv[x*2 + 2] = static_cast<uint8_t>(std::clamp(y1, 0, 255));
                dyuv[x*2 + 3] = static_cast<uint8_t>(std::clamp(v, 0, 255));
            }
        } else if (fmt == 10 || fmt == 9) {
            // RGB565 (10) or RGB555 (9)
            uint16_t* d16 = reinterpret_cast<uint16_t*>(dst) + dx;
            for (uint32_t x = 0; x < copy_w; ++x) {
                uint8_t b = src[x * 4 + 0], g = src[x * 4 + 1], r = src[x * 4 + 2];
                d16[x] = (fmt == 10)
                    ? static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
                    : static_cast<uint16_t>(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
            }
        } else {
            std::memcpy(dst + dx * 4u, src, copy_w * 4u);
        }
    }
    hle_d3d8_mark_bink_frame(dest_va);
    hle_ret_u32(ctx, 0, 28);
}

void hle_BinkNextFrame(CpuContext& ctx) {
    BinkStreamState* s = find_bink(hle_arg(ctx, 0));
    if (s) {
        if (s->total_frames <= 1) {
            s->cur_frame = 1;
        } else if (s->cur_frame >= s->total_frames) {
            s->cur_frame = 1;
            s->clock_started = false;
            s->audio_started = false;
            s->last_aud_sec = 0.0;
            s->audio_handoff_done = false;
            if (!s->cache_complete) {
                stop_ffmpeg_decoders(s, false);
                start_ffmpeg_decoders(s, 0.0);
            } else {
                s->seek_base_sec = 0.0;
                if (s->audio_stream_id) audio_bink_flush(s->audio_stream_id);
            }
        } else {
            ++s->cur_frame;
        }
        sync_guest_bink(s);
    }
    hle_ret_u32(ctx, 0, 4);
}

void hle_BinkGoto(CpuContext& ctx) {
    BinkStreamState* s = find_bink(hle_arg(ctx, 0));
    uint32_t frame = hle_arg(ctx, 1);
    if (s) {
        stop_ffmpeg_decoders(s, false);
        s->cur_frame = std::clamp(frame, 1u, s->total_frames);
        start_ffmpeg_decoders(s, (s->cur_frame - 1u) / s->fps);
        sync_guest_bink(s);
    }
    hle_ret(ctx, 12);
}

void hle_BinkPause(CpuContext& ctx) {
    BinkStreamState* s = find_bink(hle_arg(ctx, 0));
    uint32_t pause = hle_arg(ctx, 1);
    if (s) {
        bool want = (pause != 0);
        auto now = std::chrono::steady_clock::now();
        if (want && !s->paused) {
            s->paused_at_tp = now;
        } else if (!want && s->paused && s->clock_started) {
            s->clock_origin += (now - s->paused_at_tp);
        }
        s->paused = want;
        if (s->audio_stream_id) audio_bink_pause(s->audio_stream_id, s->paused || !s->audio_started);
        sync_guest_bink(s);
    }
    hle_ret_u32(ctx, pause ? 1u : 0u, 8);
}

void hle_BinkSetVolume(CpuContext& ctx) {
    BinkStreamState* s = find_bink(hle_arg(ctx, 0));
    int32_t vol = static_cast<int32_t>(hle_arg(ctx, 2)); // 0..32768
    if (s) {
        s->volume = std::clamp(vol / 32768.0f, 0.0f, 1.0f);
        if (s->audio_stream_id) audio_bink_set_volume(s->audio_stream_id, s->sound_on ? s->volume : 0.0f);
    }
    hle_ret(ctx, 12);
}

void hle_BinkSetPan(CpuContext& ctx)          { hle_ret(ctx, 12); }
void hle_BinkSetMixBins(CpuContext& ctx)      { hle_ret(ctx, 16); }
void hle_BinkSetVideoOnOff(CpuContext& ctx)   {
    if (auto* s = find_bink(hle_arg(ctx, 0))) s->video_on = (hle_arg(ctx, 1) != 0);
    hle_ret(ctx, 8);
}
void hle_BinkSetSoundOnOff(CpuContext& ctx)   {
    if (auto* s = find_bink(hle_arg(ctx, 0))) {
        s->sound_on = (hle_arg(ctx, 1) != 0);
        if (s->audio_stream_id) audio_bink_set_volume(s->audio_stream_id, s->sound_on ? s->volume : 0.0f);
    }
    hle_ret(ctx, 8);
}

} // namespace

void hle_bink_shutdown() {
    std::vector<BinkStreamState*> rem;
    {
        std::lock_guard<std::mutex> lk(g_bink_mtx);
        for (auto& kv : g_binks) rem.push_back(kv.second);
        g_binks.clear();
    }
    for (BinkStreamState* s : rem) {
        if (s) {
            stop_ffmpeg_decoders(s, true);
            delete s;
        }
    }
}

void hle_register_bink() {
    std::string zdir = XisoVfs::instance().host_path_for("Z", "");
    std::error_code ec;
    if (fs::exists(zdir, ec)) {
        for (auto& de : fs::directory_iterator(zdir, ec)) {
            auto fn = de.path().filename().string();
            if (fn.rfind("_bink_", 0) == 0) fs::remove(de.path(), ec);
        }
    }
    recomp_register_fn(0x002A1110u, hle_BinkSetSoundSystem);
    recomp_register_fn(0x002A14D0u, hle_BinkSetFrameRate);
    recomp_register_fn(0x002A1520u, hle_BinkSetSoundTrack);
    recomp_register_fn(0x002A2000u, hle_BinkDoFrame);
    recomp_register_fn(0x002A2760u, hle_BinkWait);
    recomp_register_fn(0x002A2970u, hle_BinkPause);
    recomp_register_fn(0x002A3440u, hle_BinkSetVolume);
    recomp_register_fn(0x002A3490u, hle_BinkSetMixBins);
    recomp_register_fn(0x002A3530u, hle_BinkSetPan);
    recomp_register_fn(0x002A3850u, hle_BinkSetVideoOnOff);
    recomp_register_fn(0x002A3870u, hle_BinkSetSoundOnOff);
    recomp_register_fn(0x002A3A70u, hle_BinkOpen);
    recomp_register_fn(0x002A49B0u, hle_BinkCopyToBuffer);
    recomp_register_fn(0x002A49F0u, hle_BinkNextFrame);
    recomp_register_fn(0x002A4B90u, hle_BinkGoto);
    recomp_register_fn(0x002A4D50u, hle_BinkClose);
    recomp_register_fn(0x002A4E80u, hle_BinkSetIO);
}
