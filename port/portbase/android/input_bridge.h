#pragma once

#include <SDL2/SDL.h>

struct _JNIEnv;
struct so_module;

/*
 * Input for this build crosses exported Java_* JNI functions. It does not use
 * AInputQueue/native_app_glue, so SDL events have to be delivered explicitly
 * by the loader's frame loop.
 */
void android_input_init(so_module *mod, _JNIEnv *env, int width, int height);
bool android_input_event(const SDL_Event *event);
void android_input_tick(void);
extern "C" void android_input_cursor_position(float *x, float *y, int *visible);
void android_input_cursor_set(float x, float y);
void android_input_cursor_press(bool down);
bool android_input_inject_control(const char *name, bool down);
bool android_input_inject_stick(const char *name, float x, float y);
/* Engine touchEvent: action 1 down, 2 move, 0 up; view pixels; pointer id. */
bool android_input_inject_touch(int action, int x, int y, int id);
bool android_input_inject_key(int code, bool down);
void android_input_set_pad(bool connected);
/* True while the game shows a menu rather than gameplay. */
bool android_input_in_menu(void);
void android_input_autopilot_tick(long frame);
void android_input_autopilot_sample(long frame);
long android_input_autopilot_keys(void);
long android_input_autopilot_scenes(void);
