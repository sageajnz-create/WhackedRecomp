#pragma once

#include <cstdint>
#include <string>

void ui_set_userdata_root(const std::string& root);
void ui_set_iso_status(const std::string& iso_path, bool ok, const std::string& detail = {});
void ui_notify_dropped_path(const std::string& path);
void ui_show_toast(const std::string& msg, double seconds = 3.5);
void ui_set_active_backend_name(const char* name);

void ui_init_dx11(void* sdl_win, void* dx_dev, void* dx_ctx);
void ui_init_gl3(void* sdl_win, void* gl_ctx);
void ui_shutdown();
void ui_finish_network_setup();

void ui_process_sdl_event(const void* sdl_event);
bool ui_wants_keyboard();
bool ui_wants_mouse();

void ui_set_launcher_active(bool active);
bool ui_is_launcher_active();
bool ui_consume_launch_requested();
bool ui_consume_quit_requested();

void ui_set_overlay_open(bool open);
void ui_toggle_overlay();
bool ui_is_overlay_open();

void ui_set_start_screen(bool active);
bool ui_handle_quit_shortcut(const void* sdl_event);
bool ui_quit_prompt_captures_input();

struct SDL_Window;
void fit_window_to_display(SDL_Window* window, int width, int height);
void ui_render_frame(bool is_opengl);
