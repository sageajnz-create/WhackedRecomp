#include "hle_common.h"
#include "app_config.h"
#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t XB_DS_OK = 0;
constexpr uint32_t XB_E_FAIL = 0x80004005u;
constexpr uint32_t XB_E_OUTOFMEMORY = 0x8007000Eu;
constexpr uint32_t XB_E_INVALIDARG = 0x80070057u;
constexpr uint32_t XB_XMEDIAPACKET_STATUS_SUCCESS = 0u;
constexpr uint32_t XB_XMEDIAPACKET_STATUS_PENDING = 0x8000000Au;

constexpr uint32_t HLE_DS_OBJ_MAGIC    = 0x44533830u;
constexpr uint32_t HLE_DSB_OBJ_MAGIC   = 0x44534238u;
constexpr uint32_t HLE_DSS_OBJ_MAGIC   = 0x44535338u;
constexpr uint32_t HLE_FXMO_OBJ_MAGIC  = 0x46584D4Fu;

constexpr uint32_t VTBL_DS_VA       = 0x0F100000u;
constexpr uint32_t VTBL_DSB_VA      = 0x0F100100u;
constexpr uint32_t VTBL_DSS_VA      = 0x0F100300u;
constexpr uint32_t VTBL_FXMO_VA     = 0x0F100500u;

constexpr uint32_t FN_DS_BASE       = 0xFE200000u;
constexpr uint32_t FN_DSB_BASE      = 0xFE201000u;
constexpr uint32_t FN_DSS_BASE      = 0xFE202000u;
constexpr uint32_t FN_FXMO_BASE     = 0xFE203000u;

constexpr uint32_t MIXER_RATE = 44100u;

struct AudioFormat {
    uint16_t format_tag = 1; // 1 = PCM, 0x69 = Xbox ADPCM, 3 = IEEE_FLOAT
    uint16_t channels = 2;
    uint32_t sample_rate = 44100;
    uint32_t avg_bytes_per_sec = 176400;
    uint16_t block_align = 4;
    uint16_t bits_per_sample = 16;
};

struct PendingPacket {
    uint32_t status_va = 0;
    uint32_t completed_va = 0;
    uint32_t event_handle = 0;
    uint32_t byte_len = 0;
    std::vector<int16_t> pcm; // interleaved channels
    double cursor_frames = 0.0;
};

struct DSBufferState {
    uint32_t guest_va = 0;
    int ref_count = 1;
    AudioFormat fmt{};
    std::vector<uint8_t> raw_data;
    std::vector<int16_t> pcm_data;
    double cursor_frames = 0.0;
    int32_t volume_mb = 0;
    uint32_t headroom = 0;
    uint32_t freq_hz = 44100;
    bool looping = false;
    bool paused = false;
    bool active_started = false;
};

struct DSStreamState {
    uint32_t guest_va = 0;
    int ref_count = 1;
    AudioFormat fmt{};
    uint32_t max_packets = 4;
    uint32_t cb_fn_va = 0;
    uint32_t cb_ctx_va = 0;
    int32_t volume_mb = 0;
    bool paused = false;
    std::vector<std::unique_ptr<PendingPacket>> pending;
};

struct FileXmoState {
    uint32_t guest_va = 0;
    int ref_count = 1;
    uint32_t file_handle = 0;
};

struct BinkStreamState {
    uint32_t id = 0;
    uint32_t sample_rate = 44100;
    uint32_t channels = 2;
    float volume = 1.0f;
    bool paused = false;
    struct Chunk {
        std::vector<int16_t> pcm;
        double cursor_frames = 0.0;
    };
    std::vector<Chunk> queue;
};

struct SignalItem {
    uint32_t status_va = 0;
    uint32_t completed_va = 0;
    uint32_t event_handle = 0;
    uint32_t byte_len = 0;
};

std::mutex g_audio_mtx;
SDL_AudioDeviceID g_audio_dev = 0;
bool g_audio_init_tried = false;
std::atomic<bool> g_silent_pump_running{false};
std::thread g_silent_pump_thr;
uint32_t g_primary_ds_va = 0;
int g_ds_refs = 0;

std::unordered_map<uint32_t, std::unique_ptr<DSBufferState>> g_buffers;
std::unordered_map<uint32_t, std::unique_ptr<DSStreamState>> g_streams;
std::unordered_map<uint32_t, std::unique_ptr<FileXmoState>>  g_fxmos;
std::unordered_map<uint32_t, std::unique_ptr<BinkStreamState>> g_bink_streams;
uint32_t g_next_bink_id = 1;

std::vector<float> g_mix_buf;

float mb_to_linear(int32_t mb) {
    if (mb >= 0) return 1.0f;
    if (mb <= -10000) return 0.0f;
    return std::clamp(std::pow(10.0f, static_cast<float>(mb) / 2000.0f), 0.0f, 1.0f);
}

inline void sample_pcm_linear(const int16_t* pcm, size_t total_frames, uint16_t ch, double pos, float gain, float& outL, float& outR) {
    size_t i0 = static_cast<size_t>(pos);
    if (i0 >= total_frames) return;
    size_t i1 = (i0 + 1 < total_frames) ? (i0 + 1) : i0;
    float t = static_cast<float>(pos - static_cast<double>(i0));
    constexpr float inv = 1.0f / 32768.0f;
    if (ch == 1) {
        float s0 = pcm[i0] * inv;
        float s1 = pcm[i1] * inv;
        float s = (s0 + (s1 - s0) * t) * gain;
        outL += s;
        outR += s;
    } else {
        float l0 = pcm[i0 * 2 + 0] * inv, r0 = pcm[i0 * 2 + 1] * inv;
        float l1 = pcm[i1 * 2 + 0] * inv, r1 = pcm[i1 * 2 + 1] * inv;
        outL += (l0 + (l1 - l0) * t) * gain;
        outR += (r0 + (r1 - r0) * t) * gain;
    }
}

void mix_audio_frames(int16_t* out_stereo, size_t out_frames) {
    std::vector<SignalItem> completed;
    const AppConfig& cfg = app_config();
    float master = cfg.muted ? 0.0f : cfg.master_volume;
    float sfx_mul = master * cfg.sfx_volume;
    float mus_mul = master * cfg.music_volume;

    if (g_mix_buf.size() < out_frames * 2) g_mix_buf.assign(out_frames * 2, 0.0f);
    else std::memset(g_mix_buf.data(), 0, out_frames * 2 * sizeof(float));

    {
        std::lock_guard<std::mutex> lk(g_audio_mtx);

        // 1. Mix DirectSound Buffers (SFX / UI sounds)
        for (auto& kv : g_buffers) {
            DSBufferState* b = kv.second.get();
            if (!b->active_started || b->paused || b->pcm_data.empty()) continue;
            uint16_t ch = std::max<uint16_t>(1, b->fmt.channels);
            size_t total_frames = b->pcm_data.size() / ch;
            if (total_frames == 0) continue;

            uint32_t hz = b->freq_hz ? b->freq_hz : (b->fmt.sample_rate ? b->fmt.sample_rate : MIXER_RATE);
            double step = static_cast<double>(hz) / static_cast<double>(MIXER_RATE);
            float gain = mb_to_linear(b->volume_mb - static_cast<int32_t>(b->headroom)) * sfx_mul;

            for (size_t i = 0; i < out_frames; ++i) {
                if (b->cursor_frames >= static_cast<double>(total_frames)) {
                    if (b->looping) {
                        b->cursor_frames = std::fmod(b->cursor_frames, static_cast<double>(total_frames));
                    } else {
                        b->active_started = false;
                        b->cursor_frames = 0.0;
                        break;
                    }
                }
                if (gain > 0.0001f) {
                    sample_pcm_linear(b->pcm_data.data(), total_frames, ch, b->cursor_frames,
                                      gain, g_mix_buf[i * 2 + 0], g_mix_buf[i * 2 + 1]);
                }
                b->cursor_frames += step;
            }
        }

        // 2. Mix DirectSound Streams (Music / Audio XMO Streams)
        for (auto& kv : g_streams) {
            DSStreamState* st = kv.second.get();
            if (st->paused || st->pending.empty()) continue;
            uint16_t ch = std::max<uint16_t>(1, st->fmt.channels);
            double step = static_cast<double>(st->fmt.sample_rate ? st->fmt.sample_rate : MIXER_RATE) / static_cast<double>(MIXER_RATE);
            float gain = mb_to_linear(st->volume_mb) * mus_mul;

            size_t out_i = 0;
            while (out_i < out_frames && !st->pending.empty()) {
                auto& pkt = st->pending.front();
                size_t pkt_frames = pkt->pcm.size() / ch;
                if (pkt_frames == 0 || pkt->cursor_frames >= static_cast<double>(pkt_frames)) {
                    completed.push_back({pkt->status_va, pkt->completed_va, pkt->event_handle, pkt->byte_len});
                    st->pending.erase(st->pending.begin());
                    continue;
                }
                while (out_i < out_frames && pkt->cursor_frames < static_cast<double>(pkt_frames)) {
                    if (gain > 0.0001f) {
                        sample_pcm_linear(pkt->pcm.data(), pkt_frames, ch, pkt->cursor_frames,
                                          gain, g_mix_buf[out_i * 2 + 0], g_mix_buf[out_i * 2 + 1]);
                    }
                    pkt->cursor_frames += step;
                    ++out_i;
                }
                if (pkt->cursor_frames >= static_cast<double>(pkt_frames)) {
                    completed.push_back({pkt->status_va, pkt->completed_va, pkt->event_handle, pkt->byte_len});
                    st->pending.erase(st->pending.begin());
                }
            }
        }

        // 3. Mix Bink FMV Audio Streams
        for (auto& kv : g_bink_streams) {
            BinkStreamState* bs = kv.second.get();
            if (bs->paused || bs->queue.empty()) continue;
            uint16_t ch = std::max<uint16_t>(1, static_cast<uint16_t>(bs->channels));
            double step = static_cast<double>(bs->sample_rate) / static_cast<double>(MIXER_RATE);
            float gain = bs->volume * mus_mul;

            size_t out_i = 0;
            while (out_i < out_frames && !bs->queue.empty()) {
                auto& chunk = bs->queue.front();
                size_t cf = chunk.pcm.size() / ch;
                if (cf == 0 || chunk.cursor_frames >= static_cast<double>(cf)) {
                    bs->queue.erase(bs->queue.begin());
                    continue;
                }
                while (out_i < out_frames && chunk.cursor_frames < static_cast<double>(cf)) {
                    if (gain > 0.0001f) {
                        sample_pcm_linear(chunk.pcm.data(), cf, ch, chunk.cursor_frames,
                                          gain, g_mix_buf[out_i * 2 + 0], g_mix_buf[out_i * 2 + 1]);
                    }
                    chunk.cursor_frames += step;
                    ++out_i;
                }
                if (chunk.cursor_frames >= static_cast<double>(cf)) {
                    bs->queue.erase(bs->queue.begin());
                }
            }
        }
    }

    // Signal completed XMediaPacket events outside g_audio_mtx
    for (const auto& s : completed) {
        if (s.completed_va && mem_ptr(s.completed_va)) wr32(s.completed_va, s.byte_len);
        if (s.status_va && mem_ptr(s.status_va)) wr32(s.status_va, XB_XMEDIAPACKET_STATUS_SUCCESS);
        if (s.event_handle) hle_kernel_signal_handle(s.event_handle);
    }

    if (out_stereo) {
        for (size_t i = 0; i < out_frames * 2; ++i) {
            float v = std::clamp(g_mix_buf[i], -1.0f, 1.0f);
            out_stereo[i] = static_cast<int16_t>(v * 32767.0f);
        }
    }
}

void sdl_audio_cb(void*, Uint8* stream, int len) {
    size_t frames = static_cast<size_t>(std::max(0, len)) / 4u;
    if (frames > 0) {
        mix_audio_frames(reinterpret_cast<int16_t*>(stream), frames);
    }
}

void ensure_audio_mixer() {
    if (g_audio_init_tried) return;
    g_audio_init_tried = true;

    if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0) {
        SDL_InitSubSystem(SDL_INIT_AUDIO);
    }

    SDL_AudioSpec want{}, have{};
    want.freq = MIXER_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = sdl_audio_cb;

    g_audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_audio_dev > 0) {
        SDL_PauseAudioDevice(g_audio_dev, 0);
        std::printf("[AUDIO] SDL2 real-time stereo mixer ready (%d Hz).\n", have.freq);
    } else {
        std::printf("[AUDIO] No hardware audio device (%s); running headless timing pump.\n", SDL_GetError());
        if (!g_silent_pump_running.exchange(true)) {
            g_silent_pump_thr = std::thread([]() {
                while (g_silent_pump_running.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    mix_audio_frames(nullptr, 706);
                }
            });
            g_silent_pump_thr.detach();
        }
    }
}

AudioFormat read_wfx(uint32_t va) {
    AudioFormat f{};
    if (!va || !mem_ptr(va)) return f;
    f.format_tag        = rd16(va + 0);
    f.channels          = std::max<uint16_t>(1, std::min<uint16_t>(8, rd16(va + 2)));
    f.sample_rate       = std::clamp<uint32_t>(rd32(va + 4), 4000u, 96000u);
    f.avg_bytes_per_sec = rd32(va + 8);
    f.block_align       = rd16(va + 12);
    f.bits_per_sample   = rd16(va + 14);
    return f;
}

static const int32_t IMA_INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int32_t IMA_STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
    2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767
};

inline int16_t decode_ima_nibble(uint8_t nibble, int32_t& predictor, int32_t& index) {
    int32_t step = IMA_STEP_TABLE[index];
    int32_t diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) diff = -diff;
    predictor = std::clamp(predictor + diff, -32768, 32767);
    index = std::clamp(index + IMA_INDEX_TABLE[nibble & 0xF], 0, 88);
    return static_cast<int16_t>(predictor);
}

void decode_to_pcm16(const AudioFormat& fmt, const uint8_t* src, size_t len, std::vector<int16_t>& out) {
    out.clear();
    if (!src || len == 0) return;

    if (fmt.format_tag == 0x0069) {
        size_t ch = fmt.channels >= 2 ? 2 : 1;
        size_t blk_sz = 36 * ch;
        size_t nblocks = len / blk_sz;
        out.reserve(nblocks * 64 * ch);
        for (size_t b = 0; b < nblocks; ++b) {
            const uint8_t* blk = src + b * blk_sz;
            if (ch == 1) {
                int32_t pred = static_cast<int16_t>(blk[0] | (blk[1] << 8));
                int32_t idx  = std::clamp<int32_t>(blk[2], 0, 88);
                for (size_t i = 0; i < 32; ++i) {
                    uint8_t v = blk[4 + i];
                    out.push_back(decode_ima_nibble(v & 0xF, pred, idx));
                    out.push_back(decode_ima_nibble(v >> 4,  pred, idx));
                }
            } else {
                int32_t pred0 = static_cast<int16_t>(blk[0] | (blk[1] << 8));
                int32_t idx0  = std::clamp<int32_t>(blk[2], 0, 88);
                int32_t pred1 = static_cast<int16_t>(blk[4] | (blk[5] << 8));
                int32_t idx1  = std::clamp<int32_t>(blk[6], 0, 88);
                const uint8_t* pay = blk + 8;
                for (size_t r = 0; r < 8; ++r) {
                    const uint8_t* pL = pay + r * 8;
                    const uint8_t* pR = pay + r * 8 + 4;
                    for (size_t j = 0; j < 4; ++j) {
                        uint8_t bL = pL[j], bR = pR[j];
                        out.push_back(decode_ima_nibble(bL & 0xF, pred0, idx0));
                        out.push_back(decode_ima_nibble(bR & 0xF, pred1, idx1));
                        out.push_back(decode_ima_nibble(bL >> 4,  pred0, idx0));
                        out.push_back(decode_ima_nibble(bR >> 4,  pred1, idx1));
                    }
                }
            }
        }
        return;
    }

    if (fmt.bits_per_sample == 8) {
        out.resize(len);
        for (size_t i = 0; i < len; ++i) out[i] = static_cast<int16_t>((static_cast<int>(src[i]) - 128) << 8);
        return;
    }
    if (fmt.format_tag == 3 && fmt.bits_per_sample == 32) {
        size_t n = len / 4;
        out.resize(n);
        for (size_t i = 0; i < n; ++i) {
            float f; std::memcpy(&f, src + i * 4, 4);
            out[i] = static_cast<int16_t>(std::clamp(f, -1.0f, 1.0f) * 32767.0f);
        }
        return;
    }
    size_t n = len / 2;
    out.resize(n);
    std::memcpy(out.data(), src, n * 2);
}

DSBufferState* find_buffer(uint32_t va) { auto it = g_buffers.find(va); return it != g_buffers.end() ? it->second.get() : nullptr; }
DSStreamState* find_stream(uint32_t va) { auto it = g_streams.find(va); return it != g_streams.end() ? it->second.get() : nullptr; }
FileXmoState*  find_fxmo(uint32_t va)   { auto it = g_fxmos.find(va);   return it != g_fxmos.end()   ? it->second.get() : nullptr; }

uint32_t ds_buffer_create_impl(uint32_t desc_va) {
    ensure_audio_mixer();
    auto buf = std::make_unique<DSBufferState>();
    if (desc_va && mem_ptr(desc_va)) buf->fmt = read_wfx(rd32(desc_va + 0x0C));
    buf->freq_hz = buf->fmt.sample_rate ? buf->fmt.sample_rate : MIXER_RATE;
    uint32_t va = xbox_guest_alloc(16, 16, true);
    if (!va) return 0;
    wr32(va + 0, VTBL_DSB_VA);
    wr32(va + 4, HLE_DSB_OBJ_MAGIC);
    wr32(va + 8, va);
    buf->guest_va = va;
    g_buffers[va] = std::move(buf);
    return va;
}

uint32_t ds_stream_create_impl(uint32_t desc_va) {
    ensure_audio_mixer();
    auto st = std::make_unique<DSStreamState>();
    if (desc_va && mem_ptr(desc_va)) {
        st->max_packets = std::max<uint32_t>(1u, rd32(desc_va + 4));
        st->fmt = read_wfx(rd32(desc_va + 8));
        st->cb_fn_va = rd32(desc_va + 12);
        st->cb_ctx_va = rd32(desc_va + 16);
    }
    uint32_t va = xbox_guest_alloc(16, 16, true);
    if (!va) return 0;
    wr32(va + 0, VTBL_DSS_VA);
    wr32(va + 4, HLE_DSS_OBJ_MAGIC);
    wr32(va + 8, va);
    st->guest_va = va;
    g_streams[va] = std::move(st);
    return va;
}

void ds_DirectSoundCreate(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    ensure_audio_mixer();
    uint32_t ppDS = hle_arg(ctx, 1);
    if (!g_primary_ds_va) {
        g_primary_ds_va = xbox_guest_alloc(16, 16, true);
        wr32(g_primary_ds_va + 0, VTBL_DS_VA);
        wr32(g_primary_ds_va + 4, HLE_DS_OBJ_MAGIC);
    }
    ++g_ds_refs;
    if (ppDS && mem_ptr(ppDS)) wr32(ppDS, g_primary_ds_va);
    hle_ret_u32(ctx, XB_DS_OK, 12);
}

void ds_DirectSoundCreateBuffer(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    uint32_t ppBuf = hle_arg(ctx, 1);
    uint32_t va = ds_buffer_create_impl(hle_arg(ctx, 0));
    if (ppBuf && mem_ptr(ppBuf)) wr32(ppBuf, va);
    hle_ret_u32(ctx, va ? XB_DS_OK : XB_E_OUTOFMEMORY, 8);
}

void ds_DirectSoundCreateStream(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    uint32_t ppStr = hle_arg(ctx, 1);
    uint32_t va = ds_stream_create_impl(hle_arg(ctx, 0));
    if (ppStr && mem_ptr(ppStr)) wr32(ppStr, va);
    hle_ret_u32(ctx, va ? XB_DS_OK : XB_E_OUTOFMEMORY, 8);
}

void ds_DirectSoundDoWork(CpuContext& ctx) { hle_ret(ctx, 0); }

void ds_DownloadEffectsImage_4(CpuContext& ctx) {
    uint32_t ppDesc = hle_arg(ctx, 3);
    if (ppDesc && mem_ptr(ppDesc)) wr32(ppDesc, 0);
    hle_ret_u32(ctx, XB_DS_OK, 16);
}
void ds_IDirectSound_DownloadEffectsImage(CpuContext& ctx) {
    uint32_t ppDesc = hle_arg(ctx, 4);
    if (ppDesc && mem_ptr(ppDesc)) wr32(ppDesc, 0);
    hle_ret_u32(ctx, XB_DS_OK, 20);
}
void ds_IDirectSound_GetSpeakerConfig(CpuContext& ctx) {
    uint32_t pdw = hle_arg(ctx, 1);
    if (pdw && mem_ptr(pdw)) wr32(pdw, 0);
    hle_ret_u32(ctx, XB_DS_OK, 8);
}
void ds_IDirectSound_SetMixBinHeadroom(CpuContext& ctx) { hle_ret_u32(ctx, XB_DS_OK, 12); }

void ds_Buffer_SetBufferData(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    DSBufferState* buf = find_buffer(hle_arg(ctx, 0));
    uint32_t pvData = hle_arg(ctx, 1);
    uint32_t dwBytes = hle_arg(ctx, 2);
    if (!buf) { hle_ret_u32(ctx, XB_E_INVALIDARG, 12); return; }
    buf->active_started = false;
    buf->cursor_frames = 0.0;
    if (!pvData || dwBytes == 0 || !mem_ptr(pvData)) {
        buf->raw_data.clear();
        buf->pcm_data.clear();
    } else {
        buf->raw_data.resize(dwBytes);
        std::memcpy(buf->raw_data.data(), mem_ptr(pvData), dwBytes);
        decode_to_pcm16(buf->fmt, buf->raw_data.data(), dwBytes, buf->pcm_data);
    }
    hle_ret_u32(ctx, XB_DS_OK, 12);
}

void ds_Buffer_Play(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    DSBufferState* buf = find_buffer(hle_arg(ctx, 0));
    uint32_t flags = hle_arg(ctx, 3);
    if (!buf) { hle_ret_u32(ctx, XB_E_INVALIDARG, 16); return; }
    buf->looping = (flags & 1u) != 0;
    buf->paused = false;
    buf->cursor_frames = 0.0;
    buf->active_started = !buf->pcm_data.empty();
    hle_ret_u32(ctx, XB_DS_OK, 16);
}

void ds_Buffer_Stop(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    if (DSBufferState* buf = find_buffer(hle_arg(ctx, 0))) {
        buf->active_started = false;
        buf->paused = false;
        buf->cursor_frames = 0.0;
    }
    hle_ret_u32(ctx, XB_DS_OK, 4);
}

void ds_Buffer_GetStatus(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    DSBufferState* buf = find_buffer(hle_arg(ctx, 0));
    uint32_t pdwStatus = hle_arg(ctx, 1);
    uint32_t st = 0;
    if (buf && buf->active_started) {
        st |= 1u;
        if (buf->paused) st |= 2u;
        if (buf->looping) st |= 4u;
    }
    if (pdwStatus && mem_ptr(pdwStatus)) wr32(pdwStatus, st);
    hle_ret_u32(ctx, XB_DS_OK, 8);
}

void ds_Buffer_SetCurrentPosition(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    if (DSBufferState* buf = find_buffer(hle_arg(ctx, 0))) {
        uint32_t pos_bytes = hle_arg(ctx, 1);
        uint32_t frame_bytes = std::max<uint32_t>(2u, buf->fmt.channels * 2u);
        buf->cursor_frames = static_cast<double>(pos_bytes / frame_bytes);
    }
    hle_ret_u32(ctx, XB_DS_OK, 8);
}

void ds_Buffer_SetVolume(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    if (DSBufferState* buf = find_buffer(hle_arg(ctx, 0))) buf->volume_mb = static_cast<int32_t>(hle_arg(ctx, 1));
    hle_ret_u32(ctx, XB_DS_OK, 8);
}

void ds_Buffer_SetHeadroom(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    if (DSBufferState* buf = find_buffer(hle_arg(ctx, 0))) buf->headroom = hle_arg(ctx, 1);
    hle_ret_u32(ctx, XB_DS_OK, 8);
}

void ds_Buffer_SetFrequency(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    if (DSBufferState* buf = find_buffer(hle_arg(ctx, 0))) {
        uint32_t hz = hle_arg(ctx, 1);
        buf->freq_hz = hz ? std::clamp<uint32_t>(hz, 4000u, 192000u) : buf->fmt.sample_rate;
    }
    hle_ret_u32(ctx, XB_DS_OK, 8);
}

void ds_Buffer_SetMixBinVolumes(CpuContext& ctx) { hle_ret_u32(ctx, XB_DS_OK, 8); }
void ds_Buffer_SetMixBins(CpuContext& ctx)       { hle_ret_u32(ctx, XB_DS_OK, 8); }
void ds_Buffer_SetMode(CpuContext& ctx)          { hle_ret_u32(ctx, XB_DS_OK, 12); }
void ds_Buffer_SetMode_8(CpuContext& ctx)        { hle_ret_u32(ctx, XB_DS_OK, 8); }

void ds_Stream_SetVolume(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    if (DSStreamState* st = find_stream(hle_arg(ctx, 0))) st->volume_mb = static_cast<int32_t>(hle_arg(ctx, 1));
    hle_ret_u32(ctx, XB_DS_OK, 8);
}
void ds_Stream_SetMixBinVolumes(CpuContext& ctx) { hle_ret_u32(ctx, XB_DS_OK, 8); }
void ds_Stream_Pause(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    if (DSStreamState* st = find_stream(hle_arg(ctx, 0))) st->paused = (hle_arg(ctx, 1) != 0);
    hle_ret_u32(ctx, XB_DS_OK, 8);
}
void ds_Stream_SetMode_8(CpuContext& ctx) { hle_ret_u32(ctx, XB_DS_OK, 8); }

void flush_stream_packets(DSStreamState* st) {
    if (!st) return;
    std::vector<std::unique_ptr<PendingPacket>> tmp = std::move(st->pending);
    st->pending.clear();
    for (auto& p : tmp) {
        if (p->completed_va && mem_ptr(p->completed_va)) wr32(p->completed_va, p->byte_len);
        if (p->status_va && mem_ptr(p->status_va)) wr32(p->status_va, XB_XMEDIAPACKET_STATUS_SUCCESS);
        if (p->event_handle) hle_kernel_signal_handle(p->event_handle);
    }
}

void ds_Stream_Flush(CpuContext& ctx) {
    std::vector<SignalItem> items;
    {
        std::lock_guard<std::mutex> lk(g_audio_mtx);
        if (DSStreamState* st = find_stream(hle_arg(ctx, 0))) {
            for (auto& p : st->pending) items.push_back({p->status_va, p->completed_va, p->event_handle, p->byte_len});
            st->pending.clear();
        }
    }
    for (const auto& s : items) {
        if (s.completed_va && mem_ptr(s.completed_va)) wr32(s.completed_va, s.byte_len);
        if (s.status_va && mem_ptr(s.status_va)) wr32(s.status_va, XB_XMEDIAPACKET_STATUS_SUCCESS);
        if (s.event_handle) hle_kernel_signal_handle(s.event_handle);
    }
    hle_ret_u32(ctx, XB_DS_OK, 4);
}

void ds_Stream_FlushEx(CpuContext& ctx) {
    std::vector<SignalItem> items;
    {
        std::lock_guard<std::mutex> lk(g_audio_mtx);
        if (DSStreamState* st = find_stream(hle_arg(ctx, 0))) {
            for (auto& p : st->pending) items.push_back({p->status_va, p->completed_va, p->event_handle, p->byte_len});
            st->pending.clear();
        }
    }
    for (const auto& s : items) {
        if (s.completed_va && mem_ptr(s.completed_va)) wr32(s.completed_va, s.byte_len);
        if (s.status_va && mem_ptr(s.status_va)) wr32(s.status_va, XB_XMEDIAPACKET_STATUS_SUCCESS);
        if (s.event_handle) hle_kernel_signal_handle(s.event_handle);
    }
    hle_ret_u32(ctx, XB_DS_OK, 16);
}

void ds_Stream_GetInfo(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    DSStreamState* st = find_stream(hle_arg(ctx, 0));
    uint32_t info_va = hle_arg(ctx, 1);
    if (info_va && mem_ptr(info_va)) {
        wr32(info_va + 0, 1u);
        wr32(info_va + 4, st && st->fmt.block_align ? st->fmt.block_align : 4u);
        wr32(info_va + 8, 0u);
        wr32(info_va + 12, 0u);
    }
    hle_ret_u32(ctx, XB_DS_OK, 8);
}

void ds_Stream_GetStatus(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    DSStreamState* st = find_stream(hle_arg(ctx, 0));
    uint32_t pdwStatus = hle_arg(ctx, 1);
    uint32_t status = (!st || st->pending.size() < st->max_packets) ? 1u : 0u;
    if (pdwStatus && mem_ptr(pdwStatus)) wr32(pdwStatus, status);
    hle_ret_u32(ctx, XB_DS_OK, 8);
}

void ds_Stream_Process(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    DSStreamState* st = find_stream(hle_arg(ctx, 0));
    uint32_t in_pkt_va = hle_arg(ctx, 1);
    if (!st || !in_pkt_va || !mem_ptr(in_pkt_va)) {
        hle_ret_u32(ctx, XB_E_INVALIDARG, 12);
        return;
    }
    uint32_t pvBuffer  = rd32(in_pkt_va + 0);
    uint32_t dwMaxSize = rd32(in_pkt_va + 4);
    uint32_t pdwComp   = rd32(in_pkt_va + 8);
    uint32_t pdwStatus = rd32(in_pkt_va + 12);
    uint32_t hEvt      = rd32(in_pkt_va + 16);

    if (pdwStatus && mem_ptr(pdwStatus)) wr32(pdwStatus, XB_XMEDIAPACKET_STATUS_PENDING);

    auto pkt = std::make_unique<PendingPacket>();
    pkt->status_va    = pdwStatus;
    pkt->completed_va = pdwComp;
    pkt->event_handle = hEvt;
    pkt->byte_len     = dwMaxSize;
    if (pvBuffer && dwMaxSize > 0 && mem_ptr(pvBuffer)) {
        decode_to_pcm16(st->fmt, mem_ptr(pvBuffer), dwMaxSize, pkt->pcm);
    }
    st->pending.push_back(std::move(pkt));
    hle_ret_u32(ctx, XB_DS_OK, 12);
}

void ds_Stream_Discontinuity(CpuContext& ctx) { hle_ret_u32(ctx, XB_DS_OK, 4); }

void ds_Object_AddRef(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    uint32_t obj = hle_arg(ctx, 0);
    if (obj == g_primary_ds_va) { hle_ret_u32(ctx, static_cast<uint32_t>(++g_ds_refs), 4); return; }
    if (auto* b = find_buffer(obj)) { hle_ret_u32(ctx, static_cast<uint32_t>(++b->ref_count), 4); return; }
    if (auto* s = find_stream(obj)) { hle_ret_u32(ctx, static_cast<uint32_t>(++s->ref_count), 4); return; }
    if (auto* f = find_fxmo(obj))   { hle_ret_u32(ctx, static_cast<uint32_t>(++f->ref_count), 4); return; }
    hle_ret_u32(ctx, 1u, 4);
}

void ds_Object_Release(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    uint32_t obj = hle_arg(ctx, 0);
    if (obj == g_primary_ds_va) {
        if (g_ds_refs > 0) --g_ds_refs;
        hle_ret_u32(ctx, static_cast<uint32_t>(g_ds_refs), 4);
        return;
    }
    if (auto it = g_buffers.find(obj); it != g_buffers.end()) {
        int r = --it->second->ref_count;
        if (r <= 0) { g_buffers.erase(it); xbox_guest_free(obj); hle_ret_u32(ctx, 0u, 4); return; }
        hle_ret_u32(ctx, static_cast<uint32_t>(r), 4); return;
    }
    if (auto it = g_streams.find(obj); it != g_streams.end()) {
        int r = --it->second->ref_count;
        if (r <= 0) { g_streams.erase(it); xbox_guest_free(obj); hle_ret_u32(ctx, 0u, 4); return; }
        hle_ret_u32(ctx, static_cast<uint32_t>(r), 4); return;
    }
    if (auto it = g_fxmos.find(obj); it != g_fxmos.end()) {
        int r = --it->second->ref_count;
        if (r <= 0) { g_fxmos.erase(it); xbox_guest_free(obj); hle_ret_u32(ctx, 0u, 4); return; }
        hle_ret_u32(ctx, static_cast<uint32_t>(r), 4); return;
    }
    hle_ret_u32(ctx, 0u, 4);
}

void ds_XFileCreateMediaObject(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    uint32_t hFile = hle_arg(ctx, 0);
    uint32_t ppXmo = hle_arg(ctx, 1);
    uint32_t va = xbox_guest_alloc(16, 16, true);
    if (!va) { hle_ret_u32(ctx, XB_E_OUTOFMEMORY, 8); return; }
    wr32(va + 0, VTBL_FXMO_VA);
    wr32(va + 4, HLE_FXMO_OBJ_MAGIC);
    auto st = std::make_unique<FileXmoState>();
    st->guest_va = va; st->file_handle = hFile;
    g_fxmos[va] = std::move(st);
    if (ppXmo && mem_ptr(ppXmo)) wr32(ppXmo, va);
    hle_ret_u32(ctx, XB_DS_OK, 8);
}

void fxmo_GetInfo(CpuContext& ctx) {
    uint32_t info_va = hle_arg(ctx, 1);
    if (info_va && mem_ptr(info_va)) {
        wr32(info_va + 0, 1u); wr32(info_va + 4, 1u); wr32(info_va + 8, 1u); wr32(info_va + 12, 0u);
    }
    hle_ret_u32(ctx, XB_DS_OK, 8);
}
void fxmo_GetStatus(CpuContext& ctx) {
    uint32_t pdw = hle_arg(ctx, 1);
    if (pdw && mem_ptr(pdw)) wr32(pdw, 3u);
    hle_ret_u32(ctx, XB_DS_OK, 8);
}
void fxmo_Process(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    FileXmoState* fx = find_fxmo(hle_arg(ctx, 0));
    uint32_t out_pkt = hle_arg(ctx, 2);
    if (!fx || !out_pkt || !mem_ptr(out_pkt)) { hle_ret_u32(ctx, XB_E_FAIL, 12); return; }
    uint32_t pvBuf  = rd32(out_pkt + 0);
    uint32_t maxSz  = rd32(out_pkt + 4);
    uint32_t pComp  = rd32(out_pkt + 8);
    uint32_t pStat  = rd32(out_pkt + 12);
    uint32_t got = 0;
    bool ok = (pvBuf && maxSz > 0 && mem_ptr(pvBuf)) ? hle_file_read(fx->file_handle, mem_ptr(pvBuf), maxSz, &got) : true;
    if (pComp && mem_ptr(pComp)) wr32(pComp, got);
    if (pStat && mem_ptr(pStat)) wr32(pStat, XB_XMEDIAPACKET_STATUS_SUCCESS);
    hle_ret_u32(ctx, ok ? XB_DS_OK : XB_E_FAIL, 12);
}
void fxmo_Seek(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    FileXmoState* fx = find_fxmo(hle_arg(ctx, 0));
    int32_t offset = static_cast<int32_t>(hle_arg(ctx, 1));
    uint32_t origin = hle_arg(ctx, 2);
    uint32_t pdwAbs = hle_arg(ctx, 3);
    if (!fx) { hle_ret_u32(ctx, XB_E_INVALIDARG, 16); return; }
    uint64_t newpos = 0;
    bool ok = hle_file_seek(fx->file_handle, static_cast<int64_t>(offset), origin, &newpos);
    if (ok && pdwAbs && mem_ptr(pdwAbs)) wr32(pdwAbs, static_cast<uint32_t>(newpos));
    hle_ret_u32(ctx, ok ? XB_DS_OK : XB_E_FAIL, 16);
}
void fxmo_GetLength(CpuContext& ctx) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    FileXmoState* fx = find_fxmo(hle_arg(ctx, 0));
    uint32_t pdwLen = hle_arg(ctx, 1);
    uint64_t fsz = fx ? hle_file_size(fx->file_handle) : 0;
    if (pdwLen && mem_ptr(pdwLen)) wr32(pdwLen, static_cast<uint32_t>(fsz));
    hle_ret_u32(ctx, fx ? XB_DS_OK : XB_E_INVALIDARG, 8);
}

void ds_InternalStub_4(CpuContext& ctx)  { hle_ret_u32(ctx, 0, 4); }
void ds_InternalStub_8(CpuContext& ctx)  { hle_ret_u32(ctx, 0, 8); }
void ds_InternalStub_12(CpuContext& ctx) { hle_ret_u32(ctx, 0, 12); }
void ds_InternalStub_16(CpuContext& ctx) { hle_ret_u32(ctx, 0, 16); }
void ds_InternalStub_20(CpuContext& ctx) { hle_ret_u32(ctx, 0, 20); }

void ds_StaticInit_2679A4(CpuContext& ctx) { wr32(0x00286290u, 0x00285BBCu); hle_ret(ctx, 0); }
void ds_StaticInit_2679AF(CpuContext& ctx) { wr32(0x0028628Cu, 0x00285BC0u); hle_ret(ctx, 0); }
void ds_StaticInit_26EDF2(CpuContext& ctx) { wr32(0x0028630Cu, 0x00285BC4u); hle_ret(ctx, 0); }
void ds_StaticInit_26EDFD(CpuContext& ctx) { wr32(0x00286308u, 0x00285BC8u); hle_ret(ctx, 0); }

void wma_WmaCreateDecoder(CpuContext& ctx) {
    uint32_t ppDec = hle_arg(ctx, 7);
    if (ppDec && mem_ptr(ppDec)) wr32(ppDec, 0);
    hle_ret_u32(ctx, XB_E_FAIL, 32);
}
void wma_dBToLinear(CpuContext& ctx) {
    int32_t db = static_cast<int32_t>(hle_arg(ctx, 0));
    ctx.fpu_push(std::pow(10.0, static_cast<double>(db) * 0.05));
    hle_ret(ctx, 4);
}

void populate_vtables() {
    auto write_vtbl = [](uint32_t vt_va, const std::vector<std::pair<uint32_t, RecompFn>>& entries, uint32_t base_fn) {
        for (size_t i = 0; i < entries.size(); ++i) {
            uint32_t fn_va = base_fn + static_cast<uint32_t>(i * 4u);
            recomp_register_fn(fn_va, entries[i].second);
            wr32(vt_va + static_cast<uint32_t>(i * 4u), fn_va);
        }
    };
    write_vtbl(VTBL_DS_VA, {
        {0x00, ds_Object_AddRef}, {0x04, ds_Object_Release}, {0x08, ds_DirectSoundCreateBuffer},
        {0x0C, ds_InternalStub_12}, {0x10, ds_InternalStub_8}, {0x14, ds_IDirectSound_GetSpeakerConfig},
        {0x18, ds_InternalStub_8}, {0x1C, ds_IDirectSound_DownloadEffectsImage},
    }, FN_DS_BASE);
    write_vtbl(VTBL_DSB_VA, {
        {0x00, ds_Object_AddRef}, {0x04, ds_Object_Release}, {0x08, ds_Buffer_SetBufferData},
        {0x0C, ds_InternalStub_12}, {0x10, ds_InternalStub_12}, {0x14, ds_Buffer_GetStatus},
        {0x18, ds_InternalStub_12}, {0x1C, ds_Buffer_SetCurrentPosition}, {0x20, ds_Buffer_Play},
        {0x24, ds_InternalStub_16}, {0x28, ds_Buffer_Stop}, {0x2C, ds_InternalStub_16},
        {0x30, ds_Buffer_SetVolume}, {0x34, ds_Buffer_SetFrequency}, {0x38, ds_Buffer_SetMixBins},
        {0x3C, ds_Buffer_SetMixBinVolumes}, {0x40, ds_Buffer_SetHeadroom}, {0x44, ds_Buffer_SetMode},
    }, FN_DSB_BASE);
    write_vtbl(VTBL_DSS_VA, {
        {0x00, ds_Object_AddRef}, {0x04, ds_Object_Release}, {0x08, ds_Stream_GetInfo},
        {0x0C, ds_Stream_GetStatus}, {0x10, ds_Stream_Process}, {0x14, ds_Stream_Discontinuity},
        {0x18, ds_Stream_Flush}, {0x1C, ds_InternalStub_8}, {0x20, ds_Stream_SetVolume},
        {0x24, ds_Stream_Pause}, {0x28, ds_Stream_FlushEx},
    }, FN_DSS_BASE);
    write_vtbl(VTBL_FXMO_VA, {
        {0x00, ds_Object_AddRef}, {0x04, ds_Object_Release}, {0x08, fxmo_GetInfo},
        {0x0C, fxmo_GetStatus}, {0x10, fxmo_Process}, {0x14, ds_InternalStub_4},
        {0x18, ds_InternalStub_4}, {0x1C, fxmo_Seek}, {0x20, fxmo_GetLength},
        {0x24, ds_InternalStub_4}, {0x28, ds_InternalStub_8},
    }, FN_FXMO_BASE);
}

} // namespace

uint32_t audio_bink_open_stream(uint32_t sample_rate, uint32_t channels) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    ensure_audio_mixer();
    uint32_t id = g_next_bink_id++;
    auto st = std::make_unique<BinkStreamState>();
    st->id = id;
    st->sample_rate = std::clamp<uint32_t>(sample_rate, 8000u, 96000u);
    st->channels = std::max<uint32_t>(1u, std::min<uint32_t>(2u, channels));
    g_bink_streams[id] = std::move(st);
    return id;
}

void audio_bink_push_pcm(uint32_t stream_id, const int16_t* samples, size_t byte_len) {
    if (!samples || byte_len == 0) return;
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    auto it = g_bink_streams.find(stream_id);
    if (it == g_bink_streams.end()) return;
    BinkStreamState* b = it->second.get();
    if (b->queue.size() > 64) b->queue.erase(b->queue.begin());
    size_t count = byte_len / sizeof(int16_t);
    BinkStreamState::Chunk ch;
    ch.pcm.assign(samples, samples + count);
    b->queue.push_back(std::move(ch));
}

void audio_bink_set_volume(uint32_t stream_id, float vol_0_to_1) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    auto it = g_bink_streams.find(stream_id);
    if (it != g_bink_streams.end()) it->second->volume = std::clamp(vol_0_to_1, 0.0f, 1.0f);
}

void audio_bink_pause(uint32_t stream_id, bool paused) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    auto it = g_bink_streams.find(stream_id);
    if (it != g_bink_streams.end()) it->second->paused = paused;
}

void audio_bink_close(uint32_t stream_id) {
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    g_bink_streams.erase(stream_id);
}

void audio_set_master_volume(float vol_0_to_1) {
    app_config().master_volume = std::clamp(vol_0_to_1, 0.0f, 1.0f);
}
float audio_get_master_volume() { return app_config().master_volume; }

void audio_shutdown() {
    g_silent_pump_running = false;
    if (g_audio_dev > 0) {
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
    }
}

void hle_register_audio() {
    populate_vtables();

    recomp_register_fn(0x00265FC4u, ds_DirectSoundCreate);
    recomp_register_fn(0x0026600Bu, ds_DirectSoundCreateBuffer);
    recomp_register_fn(0x00266062u, ds_DirectSoundCreateStream);
    recomp_register_fn(0x0026366Eu, ds_XFileCreateMediaObject);
    recomp_register_fn(0x002632F7u, ds_DirectSoundDoWork);
    recomp_register_fn(0x00263320u, ds_DownloadEffectsImage_4);
    recomp_register_fn(0x00262EE0u, ds_IDirectSound_GetSpeakerConfig);
    recomp_register_fn(0x00262EFCu, ds_IDirectSound_DownloadEffectsImage);
    recomp_register_fn(0x00262F8Cu, ds_IDirectSound_SetMixBinHeadroom);

    recomp_register_fn(0x0026302Eu, ds_Stream_SetVolume);
    recomp_register_fn(0x002630D6u, ds_Stream_SetMixBinVolumes);
    recomp_register_fn(0x0026310Eu, ds_Buffer_Play);
    recomp_register_fn(0x00263156u, ds_Buffer_Stop);
    recomp_register_fn(0x002631F2u, ds_Buffer_GetStatus);
    recomp_register_fn(0x0026322Eu, ds_Buffer_SetCurrentPosition);
    recomp_register_fn(0x0026329Au, ds_Buffer_SetVolume);
    recomp_register_fn(0x002632B3u, ds_Buffer_SetHeadroom);
    recomp_register_fn(0x002632B8u, ds_Buffer_SetMixBinVolumes);
    recomp_register_fn(0x002632BDu, ds_Buffer_SetMixBins);
    recomp_register_fn(0x002632C2u, ds_Stream_Pause);
    recomp_register_fn(0x00264CB3u, ds_Stream_SetMode_8);
    recomp_register_fn(0x00264F2Bu, ds_Buffer_SetMode_8);
    recomp_register_fn(0x00265AD0u, ds_Buffer_SetBufferData);
    recomp_register_fn(0x00269A1Au, ds_InternalStub_4);
    recomp_register_fn(0x002679A4u, ds_StaticInit_2679A4);
    recomp_register_fn(0x002679AFu, ds_StaticInit_2679AF);
    recomp_register_fn(0x0026EDF2u, ds_StaticInit_26EDF2);
    recomp_register_fn(0x0026EDFDu, ds_StaticInit_26EDFD);

    recomp_register_fn(0x00287588u, wma_WmaCreateDecoder);
    recomp_register_fn(0x0028C0D6u, wma_dBToLinear);

    std::printf("[HLE] Registered 24 DSOUND + 2 WMADEC + SDL2 Stereo Audio Mixer hooks.\n");
}
