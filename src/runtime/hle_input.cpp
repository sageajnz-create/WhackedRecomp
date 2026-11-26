#include "hle_common.h"
#include "app_config.h"
#include "launcher_ui.h"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifndef ERROR_SUCCESS
#define ERROR_SUCCESS 0u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_DEVICE_NOT_CONNECTED 1167u
#endif

// Xbox XINPUT_GAMEPAD offsets inside XINPUT_STATE:
// +0x04 WORD wButtons, +0x06 BYTE bAnalogButtons[8], +0x0E sThumbLX/LY/RX/RY
namespace {

struct SdlPadSlot {
    SDL_GameController* gc = nullptr;
    SDL_JoystickID jid = -1;
    std::string name;
    bool connected = false;
    uint16_t rumble_left = 0;
    uint16_t rumble_right = 0;
    // Live debug snapshot for Launcher UI
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint8_t lt = 0, rt = 0;
    uint32_t buttons_mask = 0;
};

std::mutex g_inp_mtx;
std::array<SdlPadSlot, 4> g_pads{};
std::array<bool, 4> g_connected{true, false, false, false};
std::array<bool, 4> g_reported{false, false, false, false};
std::atomic<uint32_t> g_packet_num{1};
bool g_sdl_gc_inited = false;

bool g_prev_guide = false;
bool g_prev_l3r3 = false;
bool g_initial_scan_done = false;
std::vector<std::string> g_hotplug_notices;

struct AutoInputEvt {
    uint64_t f_start = 0;
    uint64_t f_end = 0;
    std::string btn;
};
std::vector<AutoInputEvt> g_auto_inputs;
std::atomic<uint64_t> g_auto_frame{0};

void apply_radial_deadzone_2d(int16_t rx_in, int16_t ry_in, float inner_dz, float sensitivity,
                              bool invert_y, int16_t& out_x, int16_t& out_y) {
    hle_input_apply_deadzone_2d(rx_in, ry_in, inner_dz, sensitivity, invert_y, &out_x, &out_y);
}

int detect_sdl_pad_family(SDL_GameController* gc) {
    if (!gc) return 0;
#if SDL_VERSION_ATLEAST(2, 0, 12)
    switch (SDL_GameControllerGetType(gc)) {
        case SDL_CONTROLLER_TYPE_PS3:
        case SDL_CONTROLLER_TYPE_PS4:
        case SDL_CONTROLLER_TYPE_PS5:
            return 1;
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 24, 0)
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
            return 2;
        default:
            break;
    }
#endif
    return 0;
}

void ensure_sdl_controllers_scanned() {
    if (!g_sdl_gc_inited) {
        if ((SDL_WasInit(SDL_INIT_GAMECONTROLLER) & SDL_INIT_GAMECONTROLLER) == 0) {
            SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC);
        }
        SDL_GameControllerEventState(SDL_ENABLE);
        SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");
        if (const char* bp = SDL_GetBasePath()) {
            std::string db = std::string(bp) + "gamecontrollerdb.txt";
            SDL_GameControllerAddMappingsFromFile(db.c_str());
        }
        g_sdl_gc_inited = true;
    }
    // Attach newly discovered controllers into empty slots
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; ++i) {
        if (!SDL_IsGameController(i)) continue;
        // Skip already open joystick IDs
        bool already = false;
        for (int p = 0; p < 4; ++p) {
            if (g_pads[p].gc && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[p].gc)) == SDL_JoystickGetDeviceInstanceID(i)) {
                already = true;
                break;
            }
        }
        if (already) continue;
        for (int p = 0; p < 4; ++p) {
            if (!g_pads[p].gc) {
                SDL_GameController* gc = SDL_GameControllerOpen(i);
                if (gc) {
                    g_pads[p].gc = gc;
                    g_pads[p].jid = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
                    const char* nm = SDL_GameControllerName(gc);
                    g_pads[p].name = nm ? nm : "USB Gamepad";
                    g_pads[p].connected = true;
                    std::printf("[INPUT] Port %d connected: %s\n", p + 1, g_pads[p].name.c_str());
                    if (g_initial_scan_done) {
                        g_hotplug_notices.push_back("Controller connected (Port " + std::to_string(p + 1) + "): " + g_pads[p].name);
                    }
                }
                break;
            }
        }
    }
    g_initial_scan_done = true;
}

void close_disconnected_pads() {
    for (int p = 0; p < 4; ++p) {
        if (g_pads[p].gc && !SDL_GameControllerGetAttached(g_pads[p].gc)) {
            std::printf("[INPUT] Port %d disconnected: %s\n", p + 1, g_pads[p].name.c_str());
            g_hotplug_notices.push_back("Controller disconnected (Port " + std::to_string(p + 1) + "): " + g_pads[p].name);
            SDL_GameControllerClose(g_pads[p].gc);
            g_pads[p].gc = nullptr;
            g_pads[p].jid = -1;
            g_pads[p].name.clear();
            g_pads[p].connected = false;
        }
    }
}

void poll_host_devices(uint32_t port, bool& out_connected, uint16_t& wButtons,
                       uint8_t analog[8], int16_t& lx, int16_t& ly, int16_t& rx, int16_t& ry) {
    out_connected = false;
    wButtons = 0;
    std::memset(analog, 0, 8);
    lx = ly = rx = ry = 0;

    std::lock_guard<std::mutex> lk(g_inp_mtx);
    ensure_sdl_controllers_scanned();
    SDL_GameControllerUpdate();
    close_disconnected_pads();

    const AppConfig& cfg = app_config();

    if (port < 4 && g_pads[port].gc) {
        SDL_GameController* gc = g_pads[port].gc;
        out_connected = true;

        auto btn = [&](SDL_GameControllerButton b) -> bool {
            return SDL_GameControllerGetButton(gc, b) != 0;
        };

        if (btn(SDL_CONTROLLER_BUTTON_DPAD_UP))    wButtons |= 0x0001;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN))  wButtons |= 0x0002;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT))  wButtons |= 0x0004;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) wButtons |= 0x0008;
        if (btn(SDL_CONTROLLER_BUTTON_START))      wButtons |= 0x0010;
        if (btn(SDL_CONTROLLER_BUTTON_BACK))       wButtons |= 0x0020;
        if (btn(SDL_CONTROLLER_BUTTON_LEFTSTICK))  wButtons |= 0x0040;
        if (btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK)) wButtons |= 0x0080;

        bool bA = btn(SDL_CONTROLLER_BUTTON_A);
        bool bB = btn(SDL_CONTROLLER_BUTTON_B);
        bool bX = btn(SDL_CONTROLLER_BUTTON_X);
        bool bY = btn(SDL_CONTROLLER_BUTTON_Y);
        if (cfg.pad_preset == static_cast<int>(PadButtonPreset::NintendoSwap)) {
            std::swap(bA, bB);
            std::swap(bX, bY);
        }

        if (bA) analog[0] = 255;
        if (bB) analog[1] = 255;
        if (bX) analog[2] = 255;
        if (bY) analog[3] = 255;
        if (btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) analog[4] = 255; // Black
        if (btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  analog[5] = 255; // White

        int16_t raw_lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int16_t raw_rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        auto trig_byte = [](int16_t v) -> uint8_t {
            if (v <= 650) return 0;
            return static_cast<uint8_t>(std::clamp((v - 650) * 255 / (32767 - 650), 0, 255));
        };
        analog[6] = trig_byte(raw_lt);
        analog[7] = trig_byte(raw_rt);

        int16_t r_lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        int16_t r_ly = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        int16_t r_rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
        int16_t r_ry = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);

        // SDL Y-axis is negative UP; Xbox sThumbLY is positive UP!
        auto neg_y = [](int16_t v) -> int16_t {
            if (v == -32768) return 32767;
            return static_cast<int16_t>(-v);
        };
        apply_radial_deadzone_2d(r_lx, neg_y(r_ly), cfg.deadzone_left, 1.0f, false, lx, ly);
        apply_radial_deadzone_2d(r_rx, neg_y(r_ry), cfg.deadzone_right, cfg.cam_sensitivity, cfg.invert_cam_y, rx, ry);

        g_pads[port].lx = lx; g_pads[port].ly = ly;
        g_pads[port].rx = rx; g_pads[port].ry = ry;
        g_pads[port].lt = analog[6]; g_pads[port].rt = analog[7];
        g_pads[port].buttons_mask = wButtons |
            ((analog[0] > 32) ? 0x100u : 0u) | ((analog[1] > 32) ? 0x200u : 0u) |
            ((analog[2] > 32) ? 0x400u : 0u) | ((analog[3] > 32) ? 0x800u : 0u) |
            ((analog[4] > 32) ? 0x1000u : 0u) | ((analog[5] > 32) ? 0x2000u : 0u);

        if (port == 0) {
            bool guide = btn(SDL_CONTROLLER_BUTTON_GUIDE);
            bool l3r3 = btn(SDL_CONTROLLER_BUTTON_LEFTSTICK) && btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
            if (!ui_is_launcher_active() && ((guide && !g_prev_guide) || (l3r3 && !g_prev_l3r3))) {
                ui_toggle_overlay();
            }
            g_prev_guide = guide;
            g_prev_l3r3 = l3r3;
        }

        // Prevent physical controller actions from leaking into gameplay while menus/overlay are open
        if (ui_is_launcher_active() || ui_is_overlay_open()) {
            wButtons = 0;
            std::memset(analog, 0, 8);
            lx = ly = rx = ry = 0;
        }
    }

    // Merge configurable SDL Keyboard + AutoInput into Port 0
    if (port == 0) {
        out_connected = true;
        bool block_kb = ui_is_launcher_active() || ui_is_overlay_open() || ui_wants_keyboard();
        if (!block_kb) {
            int numkeys = 0;
            const uint8_t* ks = SDL_GetKeyboardState(&numkeys);
            auto kdown = [&](int sc) -> bool {
                return ks && sc > 0 && sc < numkeys && ks[sc] != 0;
            };
            const KeyBindings& kb = cfg.keys;
            if (kdown(kb.a) || kdown(SDL_SCANCODE_Z))        analog[0] = 255;
            if (kdown(kb.b) || kdown(SDL_SCANCODE_RSHIFT))   analog[1] = 255;
            if (kdown(kb.x))                                 analog[2] = 255;
            if (kdown(kb.y))                                 analog[3] = 255;
            if (kdown(kb.black))                             analog[4] = 255;
            if (kdown(kb.white))                             analog[5] = 255;
            if (kdown(kb.l_trig) || kdown(SDL_SCANCODE_LCTRL)) analog[6] = 255;
            if (kdown(kb.r_trig))                            analog[7] = 255;
            if (kdown(kb.start) || kdown(SDL_SCANCODE_ESCAPE)) wButtons |= 0x0010;
            if (kdown(kb.back) || kdown(SDL_SCANCODE_BACKSPACE)) wButtons |= 0x0020;
            if (kdown(kb.l_thumb))                           wButtons |= 0x0040;
            if (kdown(kb.r_thumb))                           wButtons |= 0x0080;

            if (kdown(kb.up))    wButtons |= 0x0001;
            if (kdown(kb.down))  wButtons |= 0x0002;
            if (kdown(kb.left))  wButtons |= 0x0004;
            if (kdown(kb.right)) wButtons |= 0x0008;

            int klx = (kdown(kb.ls_right) ? 32767 : 0) - (kdown(kb.ls_left) ? 32767 : 0);
            int kly = (kdown(kb.ls_up) ? 32767 : 0)    - (kdown(kb.ls_down) ? 32767 : 0);
            int krx = (kdown(kb.rs_right) ? 32767 : 0) - (kdown(kb.rs_left) ? 32767 : 0);
            int kry = (kdown(kb.rs_up) ? 32767 : 0)    - (kdown(kb.rs_down) ? 32767 : 0);
            if (klx == 0 && kly == 0) {
                klx = (kdown(kb.right) ? 32767 : 0) - (kdown(kb.left) ? 32767 : 0);
                kly = (kdown(kb.up) ? 32767 : 0)    - (kdown(kb.down) ? 32767 : 0);
            }
            if (klx != 0) lx = static_cast<int16_t>(std::clamp(klx, -32767, 32767));
            if (kly != 0) ly = static_cast<int16_t>(std::clamp(kly, -32767, 32767));
            if (krx != 0) rx = static_cast<int16_t>(std::clamp(krx, -32767, 32767));
            if (kry != 0) ry = static_cast<int16_t>(std::clamp(kry, -32767, 32767));
        }

        uint64_t af = g_auto_frame.load(std::memory_order_relaxed);
        for (const auto& ev : g_auto_inputs) {
            if (af >= ev.f_start && af <= ev.f_end) {
                if (ev.btn == "A") analog[0] = 255;
                else if (ev.btn == "B") analog[1] = 255;
                else if (ev.btn == "X") analog[2] = 255;
                else if (ev.btn == "Y") analog[3] = 255;
                else if (ev.btn == "START") wButtons |= 0x0010;
                else if (ev.btn == "BACK")  wButtons |= 0x0020;
                else if (ev.btn == "L3")    wButtons |= 0x0040;
                else if (ev.btn == "R3")    wButtons |= 0x0080;
                else if (ev.btn == "UP" || ev.btn == "D_UP")       { wButtons |= 0x0001; ly = 32767; }
                else if (ev.btn == "DOWN" || ev.btn == "D_DOWN")   { wButtons |= 0x0002; ly = -32767; }
                else if (ev.btn == "LEFT" || ev.btn == "D_LEFT")   { wButtons |= 0x0004; lx = -32767; }
                else if (ev.btn == "RIGHT" || ev.btn == "D_RIGHT") { wButtons |= 0x0008; lx = 32767; }
                else if (ev.btn == "BLACK" || ev.btn == "RB") analog[4] = 255;
                else if (ev.btn == "WHITE" || ev.btn == "LB") analog[5] = 255;
                else if (ev.btn == "LT") analog[6] = 255;
                else if (ev.btn == "RT") analog[7] = 255;
                else if (ev.btn == "LS_UP")    ly = 32767;
                else if (ev.btn == "LS_DOWN")  ly = -32767;
                else if (ev.btn == "LS_LEFT")  lx = -32767;
                else if (ev.btn == "LS_RIGHT") lx = 32767;
                else if (ev.btn == "RS_UP")    ry = 32767;
                else if (ev.btn == "RS_DOWN")  ry = -32767;
                else if (ev.btn == "RS_LEFT")  rx = -32767;
                else if (ev.btn == "RS_RIGHT") rx = 32767;
                else if (ev.btn == "RT")    analog[7] = 255;
                else if (ev.btn == "LT")    analog[6] = 255;
            }
        }
    }
}

void refresh_connections() {
    std::lock_guard<std::mutex> lk(g_inp_mtx);
    ensure_sdl_controllers_scanned();
    close_disconnected_pads();
    for (uint32_t p = 0; p < 4; ++p) {
        g_connected[p] = (p == 0) || (g_pads[p].gc != nullptr);
    }
}

void hle_XInitDevices(CpuContext& ctx) { refresh_connections(); hle_ret(ctx, 8); }

void hle_XGetDevices(CpuContext& ctx) {
    refresh_connections();
    std::lock_guard<std::mutex> lk(g_inp_mtx);
    uint32_t mask = 0;
    for (uint32_t p = 0; p < 4; ++p) {
        if (g_connected[p]) { mask |= (1u << p); g_reported[p] = true; }
    }
    hle_ret_u32(ctx, mask, 4);
}

void hle_XGetDeviceChanges(CpuContext& ctx) {
    uint32_t pins = hle_arg(ctx, 1);
    uint32_t prem = hle_arg(ctx, 2);
    refresh_connections();
    std::lock_guard<std::mutex> lk(g_inp_mtx);
    uint32_t ins = 0, rem = 0;
    for (uint32_t p = 0; p < 4; ++p) {
        if (g_connected[p] && !g_reported[p]) { ins |= (1u << p); g_reported[p] = true; }
        else if (!g_connected[p] && g_reported[p]) { rem |= (1u << p); g_reported[p] = false; }
    }
    if (pins) wr32(pins, ins);
    if (prem) wr32(prem, rem);
    hle_ret_u32(ctx, (ins || rem) ? 1u : 0u, 12);
}

void hle_XInputOpen(CpuContext& ctx) {
    uint32_t port = hle_arg(ctx, 1);
    if (port >= 4) { hle_ret_u32(ctx, 0, 16); return; }
    refresh_connections();
    std::lock_guard<std::mutex> lk(g_inp_mtx);
    if (!g_connected[port]) { hle_ret_u32(ctx, 0, 16); return; }
    hle_ret_u32(ctx, 0x00020000u | port, 16);
}

void hle_XInputClose(CpuContext& ctx) { hle_ret(ctx, 4); }
void hle_XInputPoll(CpuContext& ctx)  { hle_ret_u32(ctx, ERROR_SUCCESS, 4); }

void hle_XInputGetState(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    uint32_t st_va = hle_arg(ctx, 1);
    uint32_t port = h & 3u;
    if ((h & 0xFFFF0000u) != 0x00020000u) {
        hle_ret_u32(ctx, ERROR_INVALID_HANDLE, 8);
        return;
    }
    bool conn = false;
    uint16_t btns = 0;
    uint8_t analog[8]{};
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    poll_host_devices(port, conn, btns, analog, lx, ly, rx, ry);
    if (!conn) {
        hle_ret_u32(ctx, ERROR_DEVICE_NOT_CONNECTED, 8);
        return;
    }
    if (st_va) {
        wr32(st_va + 0, g_packet_num.fetch_add(1u));
        wr16(st_va + 4, btns);
        std::memcpy(mem_ptr(st_va + 6), analog, 8);
        wr16(st_va + 14, static_cast<uint16_t>(lx));
        wr16(st_va + 16, static_cast<uint16_t>(ly));
        wr16(st_va + 18, static_cast<uint16_t>(rx));
        wr16(st_va + 20, static_cast<uint16_t>(ry));
    }
    hle_ret_u32(ctx, ERROR_SUCCESS, 8);
}

void hle_XInputSetState(CpuContext& ctx) {
    uint32_t h = hle_arg(ctx, 0);
    uint32_t fb_va = hle_arg(ctx, 1);
    uint32_t port = h & 3u;
    if (fb_va) {
        wr32(fb_va + 0, ERROR_SUCCESS);
        uint16_t lo = rd16(fb_va + 8);
        uint16_t hi = rd16(fb_va + 10);
        const AppConfig& cfg = app_config();
        std::lock_guard<std::mutex> lk(g_inp_mtx);
        if (port < 4 && g_pads[port].gc) {
            if (!cfg.rumble_enabled) {
                SDL_GameControllerRumble(g_pads[port].gc, 0, 0, 0);
            } else {
                uint16_t sl = static_cast<uint16_t>(std::min<uint32_t>(65535u, static_cast<uint32_t>(lo * cfg.rumble_strength)));
                uint16_t sr = static_cast<uint16_t>(std::min<uint32_t>(65535u, static_cast<uint32_t>(hi * cfg.rumble_strength)));
                SDL_GameControllerRumble(g_pads[port].gc, sl, sr, 500);
            }
        }
    }
    hle_ret_u32(ctx, ERROR_SUCCESS, 8);
}

void hle_XGetSectionHandle_or_MU(CpuContext& ctx) { hle_ret_u32(ctx, 0, 20); }
void hle_XPP_Misc4(CpuContext& ctx) { hle_ret_u32(ctx, 0, 4); }
void hle_XPP_StaticInit(CpuContext& ctx) {
    uint32_t v = rd32(0x002C3074u);
    for (uint32_t va : {0x002B9CFCu, 0x002B9D00u, 0x002B9D0Cu, 0x002B9D10u, 0x002B9D14u,
                        0x002B9D18u, 0x002B9D1Cu, 0x002B9D20u, 0x002B9D2Cu, 0x002B9D30u}) {
        wr32(va, v);
    }
    hle_ret(ctx, 0);
}

} // namespace

void hle_input_set_hwnd(void*) {}

void hle_input_poll_ui_snapshot() {
    bool c = false; uint16_t b = 0; uint8_t a[8]{}; int16_t x0, y0, x1, y1;
    for (uint32_t p = 0; p < 4; ++p) poll_host_devices(p, c, b, a, x0, y0, x1, y1);
}

bool hle_input_get_port_status(int port, std::string& out_name, int16_t* out_lx, int16_t* out_ly,
                               int16_t* out_rx, int16_t* out_ry, uint32_t* out_btn_mask) {
    if (port < 0 || port >= 4) return false;
    std::lock_guard<std::mutex> lk(g_inp_mtx);
    ensure_sdl_controllers_scanned();
    if (!g_pads[port].gc) {
        out_name = (port == 0) ? "Keyboard (Port 1 Ready)" : "Not Connected";
        if (out_lx) *out_lx = 0;
        if (out_ly) *out_ly = 0;
        if (out_rx) *out_rx = 0;
        if (out_ry) *out_ry = 0;
        if (out_btn_mask) *out_btn_mask = 0;
        return port == 0;
    }
    out_name = g_pads[port].name;
    if (out_lx) *out_lx = g_pads[port].lx;
    if (out_ly) *out_ly = g_pads[port].ly;
    if (out_rx) *out_rx = g_pads[port].rx;
    if (out_ry) *out_ry = g_pads[port].ry;
    if (out_btn_mask) *out_btn_mask = g_pads[port].buttons_mask;
    return true;
}

void hle_input_test_rumble(int port) {
    if (port < 0 || port >= 4) return;
    std::lock_guard<std::mutex> lk(g_inp_mtx);
    if (g_pads[port].gc) {
        SDL_GameControllerRumble(g_pads[port].gc, 42000, 52000, 350);
    }
}

bool hle_input_consume_hotplug_notice(std::string& out_msg) {
    std::lock_guard<std::mutex> lk(g_inp_mtx);
    if (g_hotplug_notices.empty()) return false;
    out_msg = std::move(g_hotplug_notices.front());
    g_hotplug_notices.erase(g_hotplug_notices.begin());
    return true;
}

void hle_input_apply_deadzone_2d(int16_t rx_in, int16_t ry_in, float inner_dz, float sensitivity,
                                 bool invert_y, int16_t* out_x, int16_t* out_y) {
    float x = static_cast<float>(rx_in) / 32767.0f;
    float y = static_cast<float>(ry_in) / 32767.0f;
    if (invert_y) y = -y;
    const float mag = std::sqrt(x * x + y * y);
    const float dz = std::clamp(inner_dz, 0.0f, 0.85f);
    constexpr float outer_dz = 0.04f;
    int16_t ox = 0, oy = 0;
    if (mag > dz && mag >= 1e-6f) {
        const float denom = std::max(0.05f, 1.0f - outer_dz - dz);
        float scaled = std::clamp((mag - dz) / denom, 0.0f, 1.0f);
        float ux = (x / mag) * scaled * sensitivity;
        float uy = (y / mag) * scaled * sensitivity;
        const float out_mag = std::sqrt(ux * ux + uy * uy);
        if (out_mag > 1.0f) {
            ux /= out_mag;
            uy /= out_mag;
        }
        ox = static_cast<int16_t>(std::lround(std::clamp(ux, -1.0f, 1.0f) * 32767.0f));
        oy = static_cast<int16_t>(std::lround(std::clamp(uy, -1.0f, 1.0f) * 32767.0f));
    }
    if (out_x) *out_x = ox;
    if (out_y) *out_y = oy;
}

int hle_input_get_primary_pad_type() {
    std::lock_guard<std::mutex> lk(g_inp_mtx);
    const AppConfig& cfg = app_config();
    if (cfg.pad_preset == 1 || cfg.pad_preset == 2) return cfg.pad_preset;
    for (int p = 0; p < 4; ++p) {
        if (g_pads[p].gc) {
            int fam = detect_sdl_pad_family(g_pads[p].gc);
            if (fam != 0) return fam;
            return 0;
        }
    }
    return 0;
}

bool hle_input_get_port_info(int port, bool* out_connected, char* out_name, size_t max_name) {
    std::lock_guard<std::mutex> lk(g_inp_mtx);
    if (port < 0 || port >= 4) return false;
    bool conn = (g_pads[port].gc != nullptr);
    if (out_connected) *out_connected = conn;
    if (out_name && max_name > 0) {
        if (conn && !g_pads[port].name.empty()) {
            std::snprintf(out_name, max_name, "%s", g_pads[port].name.c_str());
        } else {
            out_name[0] = '\0';
        }
    }
    return true;
}

void hle_input_set_script(const std::string& spec) {
    g_auto_inputs.clear();
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t comma = spec.find(',', pos);
        std::string tok = (comma == std::string::npos) ? spec.substr(pos) : spec.substr(pos, comma - pos);
        size_t colon = tok.find(':');
        if (colon != std::string::npos) {
            std::string range = tok.substr(0, colon);
            std::string tail  = tok.substr(colon + 1);
            size_t dash  = range.find('-');
            size_t colon2 = tail.find(':');
            AutoInputEvt e{};
            if (dash != std::string::npos) {
                e.f_start = std::strtoull(range.substr(0, dash).c_str(), nullptr, 10);
                e.f_end   = std::strtoull(range.substr(dash + 1).c_str(), nullptr, 10);
                e.btn     = tail;
                g_auto_inputs.push_back(std::move(e));
            } else if (colon2 != std::string::npos) {
                e.f_start = std::strtoull(range.c_str(), nullptr, 10);
                e.f_end   = e.f_start + std::strtoull(tail.substr(colon2 + 1).c_str(), nullptr, 10);
                e.btn     = tail.substr(0, colon2);
                g_auto_inputs.push_back(std::move(e));
            }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
}

void hle_input_set_frame(uint64_t frame_num) {
    g_auto_frame.store(frame_num, std::memory_order_relaxed);
}

void hle_register_input() {
    recomp_register_fn(0x002BACEDu, hle_XPP_Misc4);
    recomp_register_fn(0x002BB7B6u, hle_XGetSectionHandle_or_MU);
    recomp_register_fn(0x002BC443u, hle_XPP_StaticInit);
    recomp_register_fn(0x002BBB7Fu, hle_XInputOpen);
    recomp_register_fn(0x002BBBD5u, hle_XInputClose);
    recomp_register_fn(0x002BBDBFu, hle_XInputGetState);
    recomp_register_fn(0x002BBE2Bu, hle_XInputSetState);
    recomp_register_fn(0x002BBE5Eu, hle_XInputPoll);
    recomp_register_fn(0x002BBF15u, hle_XInitDevices);
    recomp_register_fn(0x002BBF1Au, hle_XGetDevices);
    recomp_register_fn(0x002BBF3Cu, hle_XGetDeviceChanges);
}
