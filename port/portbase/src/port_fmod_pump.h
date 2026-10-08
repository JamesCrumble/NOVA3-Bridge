#ifndef PORTBASE_FMOD_PUMP_H
#define PORTBASE_FMOD_PUMP_H

#include "jni.h"

/*
 * Drives libfmodex.so's AudioTrack output, which on Android is driven from a
 * Java thread this loader has no way to run. See port_fmod_pump.cpp for what
 * that thread does and why nothing else fills in for it.
 *
 * Safe to call before the game has initialised FMOD: the pump waits.
 */
void port_fmod_pump_start(JNIEnv *env);
void port_fmod_pump_stop(void);

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Which of FMOD's two Android outputs this run should end up on.
 *
 * They are mutually exclusive and the choice is made for us: FMOD autodetects
 * by asking dlopen for libOpenSLES.so, takes OpenSL if it is there and
 * AudioTrack if it is not, and only initialises the one it picked. So the
 * decision is expressed in the one place FMOD asks - dlopen_impl - and the
 * pump reads the same answer to know whether it has anything to do. Running
 * both would not be belt and braces; the pump would spin forever against an
 * output that was never created.
 *
 * <PREFIX>_FMOD_OUTPUT=audiotrack switches to the pump. Default is OpenSL,
 * which needs no thread of ours and reuses the shim in android/opensles.cpp.
 */
int port_fmod_prefers_audiotrack(void);

#ifdef __cplusplus
}
#endif

#endif /* PORTBASE_FMOD_PUMP_H */
