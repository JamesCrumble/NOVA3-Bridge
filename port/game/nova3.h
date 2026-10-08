#pragma once

/*
 * What the parts of this port need from each other.
 *
 * Nothing generic belongs here - portbase owns the loader, the thunks, the
 * fake JVM and the input interface. This is only the handful of facts that are
 * about N.O.V.A. 3 specifically and are needed in more than one file.
 */

struct so_module;
struct _JNIEnv;

/* The loaded guest library. Same pointer port_guest_module() returns; declared
 * here so the JNI classes can forward to the game's own native methods without
 * threading it through the class registry. */
so_module *nova3_module(void);

/*
 * Every entry point this port drives lives on one Java class, and the mangled
 * name is long enough that spelling it out at each call site is how a typo
 * becomes a null function pointer. so_symbol() answers NULL for a name that
 * does not exist and says nothing about it.
 */
#define NOVA3_GL2JNILIB(m) "Java_com_gameloft_android_ANMP_GloftN3HM_GL2JNILib_" m

/*
 * Make a file the engine asked for exist, by pulling it out of one of the two
 * expansion containers if it is not on disk yet. True when the path is
 * readable afterwards. See game/obb_cache.cpp for why the unpacking is lazy.
 */
bool nova3_obb_materialise(const char *path);
long nova3_obb_extracted(void);

/*
 * Point the four GDRM lock words at real storage.
 *
 * Called from main() after JNI_OnLoad and before GL2JNILib.init(), which is
 * the first thing that writes through them. See game/patch.cpp for the fault
 * that names them.
 */
void nova3_unlock_drm(so_module *mod);
/* Dump / edit the engine's XML (quality profiles) as it is parsed. xml_hook.cpp */
void nova3_xml_hook_install(so_module *mod);
/* Engine settings forced over the device profile. tweaks.cpp */
void nova3_tweaks_install(so_module *mod);

/*
 * Hand the engine its four directories.
 *
 * On Android this is Java: GL2JNILib.setupPaths() works out the obb, data,
 * home and temp directories and calls the native setPaths with them. The
 * engine calls setupPaths back through JNI from inside GL2JNILib.init(), so
 * the normal path through this function is the fake class in
 * jni/gl2jni_activity.cpp. main() calls it too, after init(), for the case
 * where the callback never arrives - it runs once and traces which of the two
 * got there first.
 */
void nova3_set_paths(_JNIEnv *env);

/*
 * Create the directories under the writable storage that the engine writes
 * into without ever creating them. Called from main() before the boot
 * sequence; implemented next to the path rules in game/io_paths.cpp.
 */
void nova3_prepare_writable_storage(void);

/* The obb directory inside the player's tree: <game>/<package>, which is where
 * Android's /sdcard/Android/obb/<package> maps to. Both io_paths.cpp (for the
 * rewrite rule) and the setPaths call need it. */
const char *nova3_obb_dir(void);

/*
 * Whether the engine says it is sitting in the main menu or the in-game menu.
 *
 * The game exports this as GL2JNILib.nativeIsMainMenuOrIGM and its own Java
 * layer polls it to decide what a button means. Here it is the cheap way to
 * tell "the port drew 600 frames of the title screen" apart from "the port is
 * playing" - note the polarity, true means *in a menu*. Implemented in
 * game/input_bridge.cpp, next to the symbol it resolves.
 */
bool nova3_is_main_menu(void);

/*
 * Tell the fake activity what GetWindowWidth/Height should answer.
 *
 * The engine lays its interface out against those two numbers and the port
 * resizes the renderer with its own; a disagreement between them puts the
 * touch targets somewhere the pixels are not. One source, set from main().
 * Implemented in jni/gl2jni_activity.cpp.
 */
void nova3_set_view_size(int w, int h);

/*
 * Put the engine into its gamepad input path.
 *
 * Called from main() after GL2JNILib.init() has returned, because the flags it
 * sets live in state that init() creates. Implemented in game/input_bridge.cpp
 * next to the rest of the input wiring.
 */
void nova3_input_select_gamepad_mode(void);
