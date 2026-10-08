#pragma once

/*
 * GLES bridge: the game's OpenGL ES calls are serialised into a pipe and
 * executed by the host (the Android app) on the phone's GPU, instead of being
 * rendered by Mesa/llvmpipe inside qemu. See gl_bridge.cpp for the protocol.
 *
 * Active only when <PREFIX>_GL_BRIDGE=1; otherwise port_gl_getproc() is just
 * SDL_GL_GetProcAddress.
 */

#ifdef __cplusplus
extern "C" {
#endif

int gl_bridge_enabled(void);

/* Opens the command and reply pipes named by <PREFIX>_GL_CMD / _GL_REPLY. */
int gl_bridge_init(void);

/* End of frame: tells the host to present what it has drawn. */
void gl_bridge_swap(void);

/* Around the engine's step(): 1 before, 0 after. Timings with <PREFIX>_GL_PROFILE=1. */
void gl_bridge_profile_step(int begin);

/* Where every GL entry point comes from: the bridge, or SDL's driver. */
void *port_gl_getproc(const char *name);

#ifdef __cplusplus
}
#endif

/* Everything that asks SDL for a GL function goes through the bridge instead. */
#ifndef PORT_GL_NO_REDIRECT
#define SDL_GL_GetProcAddress port_gl_getproc
#endif
