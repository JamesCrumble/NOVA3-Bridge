/*
 * N.O.V.A. 3: Near Orbit Vanguard Alliance - entry point.
 *
 * This game is driven, not hosted. It declares no libandroid.so, exports no
 * ANativeActivity_onCreate, and has no frame loop of its own: what it exports
 * is JNI_OnLoad plus a wall of Java_* entry points, and on Android a Java layer
 * calls them one by one. Here that Java layer is this file.
 *
 *   $ readelf -d libNOVA3_neon.so | grep NEEDED
 *   libstdc++, libGLESv2, libOpenSLES, liblog, libc, libm, libdl
 *
 * No libandroid and no libz; GLESv2 rather than GLESv1_CM, so this is a
 * shader-based renderer with no fixed-function path at all, and audio is
 * OpenSL ES rather than a Java AudioTrack.
 *
 * The call order below is the one the game's own Java performs, read out of
 * the donor APK rather than guessed, and three of its choices are worth naming
 * because the obvious alternative is wrong:
 *
 *   - GL2JNILib.init() comes from the activity's onCreate. initGL() and
 *     InitViewSettings() come later and from a different thread: they are
 *     called by the EGL config chooser, before the context exists on Android.
 *     Grouping them with init() is what the names suggest and it is not the
 *     order the engine was built for.
 *   - setPaths is not called from here. init() reaches back into Java for
 *     setupPaths(), which is what computes the four directories and calls it;
 *     the fake activity answers that callback. main() only checks afterwards
 *     that it happened.
 *   - The gamepad flags go after init(), because the state they are written
 *     into is what init() builds.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "port_version.h"
#include "so_util.h"
#include "khronos/gles2.h"

#include "jni.h"

#include "app_exit.h"
#include "crash.h"
#include "cursor_draw.h"
#include "emulator_control.h"
#include "fb_probe.h"
#include "fix_path.h"
#include "input_bridge.h"
/* io_set_game_dir/io_game_dir come from fix_path.h and NOT from io_paths.h.
 * The two headers declare the same three functions with different linkage -
 * io_paths.h wraps them in extern "C" - so including both is a compile error.
 * Noted for portbase; here the loader-side header is the right one. */
#include "patch.h"
#include "port_env.h"
#include "trace.h"
#include "gl_bridge.h"

#include "nova3.h"
#include "jni/nova3_classes.h"

/* portbase's activity counters, declared where they are used rather than in a
 * header: three longs whose only consumer is the summary line at the end. */
extern "C" long android_io_assets_opened(void);
extern "C" long android_gl_textures_uploaded(void);
extern "C" long android_gl_draw_calls(void);

/*
 * The library, and where it sits in the player's tree.
 *
 * The donor also ships libNOVA3_8_neon.so - a second variant of the same 2013
 * build. They are not interchangeable: the exports sit at different addresses,
 * so anything this port resolves by name still works and anything it patches
 * by offset would not. The name is pinned here and the harness pins the sha1.
 */
/* portbase's viewport scaler (src/symtab_glprobe.cpp); it exports no header.
 * MC3 shipped with this declared and never called - every scale mode was
 * disarmed on non-640x480 panels until a user root-caused it (mc3 issue #2,
 * fix by Codebr0ken). Declared AND called here. */
extern "C" void viewport_scale_init(int phys_w, int phys_h);

static const char *kNativeLib    = "libNOVA3_neon.so";
static const char *kNativeLibDir = "lib/armeabi-v7a";

/* The R36S panel. The engine takes its resolution from the resize call below
 * and this port never changes it afterwards. The Android wrapper overrides it
 * (<PREFIX>_WIDTH / _HEIGHT) to match the phone's aspect ratio. */
static int kWidth  = 640;
static int kHeight = 480;

/* ---------------------------------------------------------- the module */

static so_module *g_module;

so_module *nova3_module(void) { return g_module; }

/*
 * port_guest_module - one of the ten. Deliberately C++ linkage; portbase's
 * pthread thunks resolve the game's thread callbacks through it.
 */
so_module *port_guest_module(void) { return g_module; }

/*
 * Every import nothing answers, named up front.
 *
 * The loader points unresolved jump slots at a stub that aborts on first use,
 * which surfaces one missing symbol per run, from inside a crash, with no
 * stack. Walking the table here lists all of them at once - and it is the fact
 * the harness reads to decide M2.
 *
 * Undefined *weak* symbols are not failures: resolving them to zero is what a
 * real dynamic linker does and the game tests them before use.
 */
static int report_unresolved_symbols(so_module *mod)
{
    int missing = 0;

    for (int i = 0; i < mod->num_dynsym; i++) {
        Elf_Sym *sym = &mod->dynsym[i];
        if (sym->st_shndx != SHN_UNDEF)
            continue;

        const char *name = mod->dynstr + sym->st_name;
        if (!name || !*name)
            continue;

        if (so_resolve_link(mod, name))
            continue;

        if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) {
            trace("weak import left null: %s", name);
            continue;
        }

        fprintf(stderr, "unresolved symbol: %s\n", name);
        missing++;
    }

    fflush(stderr);
    return missing;
}

extern "C" int so_after_relocate(so_module *mod)
{
    g_module = mod;
    trace("module loaded");

    /*
     * Arm the fault handler here rather than after so_load_module() returns.
     * The 740 bytes of INIT_ARRAY this module declares run immediately after
     * this hook; a fault in one of them without a handler installed is
     * reported by qemu as a bare "uncaught target signal 11" with no pc and no
     * stack - the whole first half of the boot as a blind spot, exactly where
     * the engine's own code starts running.
     */
    crash_report_init(mod, kNativeLib);

    int missing = report_unresolved_symbols(mod);
    if (missing == 0)
        return 0;

    fatal("%d import(s) of %s have no implementation (listed above).\n"
          "       Running the game now would fault on the first call to any of\n"
          "       them, from inside a static constructor, with nothing but an\n"
          "       address to go on.",
          missing, mod->soname ? mod->soname : "the module");
    return 1;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    /* The launcher asks `nova3 --version` to name the build in the log; the
     * field reports on v1.0.0/v1.0.1 all say "vunknown" because this answer
     * was never wired. Before anything else: the loader must not start over
     * a version query. */
    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("%s\n", NOVA3_PORT_VERSION);
        return 0;
    }

    /* Unbuffered from the first line: the log is the only diagnostic that
     * leaves the console, and a crash must not take it with it. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    trace("N.O.V.A. 3 port v%s", NOVA3_PORT_VERSION);

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <nova-3-directory>\n"
                "\n"
                "The directory is your own copy of the game: the extracted tree\n"
                "with lib/armeabi-v7a/ in it and the package directory holding\n"
                "the two .obb expansion files. It is never bundled with this port.\n",
                argv[0]);
        return 2;
    }

    const char *game_dir = argv[1];

    /* Before anything of the game's runs: the libc path thunks translate
     * against this, and the engine opens its first file from inside a static
     * initialiser. */
    io_set_game_dir(game_dir);
    nova3_prepare_writable_storage();
    {
        long w = port_getenv_long("WIDTH", kWidth), h = port_getenv_long("HEIGHT", kHeight);
        if (w >= 320 && w <= 4096 && h >= 240 && h <= 4096) {
            kWidth = (int)w;
            kHeight = (int)h;
        }
        /* The viewport scaler compares the drawable with this logical size;
         * keeping them equal makes it the identity. */
        char dim[16];
        snprintf(dim, sizeof(dim), "%d", kWidth);
        setenv(PORT_ENV_PREFIX "_SCREEN_W", dim, 0);
        snprintf(dim, sizeof(dim), "%d", kHeight);
        setenv(PORT_ENV_PREFIX "_SCREEN_H", dim, 0);
    }
    nova3_set_view_size(kWidth, kHeight);

    char lib_dir[PATH_MAX];
    char lib_path[PATH_MAX];
    snprintf(lib_dir,  sizeof(lib_dir),  "%s/%s", game_dir, kNativeLibDir);
    snprintf(lib_path, sizeof(lib_path), "%s/%s", lib_dir, kNativeLib);

    struct stat st;
    if (stat(lib_path, &st) != 0) {
        fatal("'%s' does not exist.\n"
              "       This does not look like an extracted N.O.V.A. 3 tree.",
              lib_path);
        return 1;
    }
    trace("native library found: %s (%lld bytes)", lib_path, (long long)st.st_size);
    trace("expansion directory: %s", nova3_obb_dir());

    /*
     * GL first, before the module is linked.
     *
     * The game's 100 gl* imports are bound by asking the driver for each entry
     * point, and there is no driver to ask until a context is current.
     * Relocating first would bind every one of them to null.
     *
     * A failure here is deliberately not fatal: "does every import resolve?"
     * and "is there a usable GLES context?" are different questions, and
     * aborting on the second makes the first unanswerable from the log.
     */
    SDL_Window   *window = NULL;
    SDL_GLContext gl     = NULL;

    /* Under the Android wrapper the pads are read by the Java side and arrive as commands; SDL's own
     * joystick subsystem would only keep probing for libudev (hundreds of failed opens a second,
     * each a guest syscall out of translated code). */
    const Uint32 sdl_flags = SDL_INIT_VIDEO | (gl_bridge_enabled() ? 0 : SDL_INIT_GAMECONTROLLER);
    if (SDL_Init(sdl_flags) != 0) {
        trace("SDL_Init(0x%x) failed: %s", sdl_flags, SDL_GetError());
    } else {
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

        window = SDL_CreateWindow("N.O.V.A. 3",
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  kWidth, kHeight,
                                  gl_bridge_enabled() ? 0 : SDL_WINDOW_OPENGL);
        if (!window)
            trace("SDL_CreateWindow failed: %s", SDL_GetError());
        else if (gl_bridge_enabled()) {
            /* The GL context lives in the host app; here it is only a token. */
            if (!gl_bridge_init()) {
                fatal("could not connect to the GL host.");
                return 1;
            }
            gl = (SDL_GLContext)(uintptr_t)1;
        }
        else if (!(gl = SDL_GL_CreateContext(window)))
            trace("no GLES 2.0 context: %s", SDL_GetError());
    }

    if (gl) {
        load_gles2_funcs();

        /*
         * The GLES1 table has to be filled too, and not because this game
         * imports GLES1. It imports none - all 100 of its gl* imports are
         * shader-pipeline calls.
         *
         * portbase resolves several of its own diagnostics through
         * find_gles1_function() and nothing else: the framebuffer probe is one
         * of them, and with the table empty it prints
         *
         *     framebuffer probe disabled: GL entry points unresolved
         *
         * and the run reports a black screen while the engine is uploading 755
         * textures and issuing 4819 draws. That is the probe being blind, not
         * the game being black, and the two are indistinguishable from a log
         * that only shows the counters.
         *
         * GL_SINGLE_DISPATCH is what makes filling it safe. It is portbase's
         * own switch, and it forces every GLES1-table name through
         * SDL_GL_GetProcAddress instead of dlsym on the GLESv1_CM provider, so
         * everything lands in the same dispatch as the GLES2 context SDL gave
         * us. Without it the emulator can bind them in desktop GL while SDL
         * uses GLES2, two ABIs that share no state; on the device libmali
         * exports EGL, GLESv1_CM and GLESv2 from one blob and both routes are
         * the same pointer. Set as a default rather than an override, so an
         * explicit NOVA3_GL_SINGLE_DISPATCH still wins for A/B runs.
         */
        setenv("NOVA3_GL_SINGLE_DISPATCH", "1", 0);
        load_gles1_funcs();

        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(window, &w, &h);
        trace("drawable %dx%d", w, h);
        viewport_scale_init(w, h);

        const GLubyte *(*get_string)(GLenum) =
            (const GLubyte *(*)(GLenum))SDL_GL_GetProcAddress("glGetString");
        if (get_string) {
            const char *ver = (const char *)get_string(0x1F02 /* GL_VERSION  */);
            const char *rnd = (const char *)get_string(0x1F01 /* GL_RENDERER */);
            trace("GL_VERSION=%s | GL_RENDERER=%s", ver ? ver : "?", rnd ? rnd : "?");
        }
    }

    /* The fake JavaVM has to exist before the module is linked: the engine's
     * static initialisers run during so_load_module() and reach for it. */
    JavaVM *vm  = NULL;
    JNIEnv *env = NULL;
    if (JNI_CreateJavaVM(&vm, &env, NULL) != JNI_OK || !vm || !env) {
        fatal("could not create the JNI environment.");
        return 1;
    }

    /*
     * Map, relocate and link.
     *
     * The VM argument is NULL on purpose and it is not an oversight:
     * so_load_module() calls JNI_OnLoad itself when a module exports one, and
     * for this game that would run it in the wrong place. The working boot
     * sequence puts JNI_OnLoad first on the game thread, ahead of everything
     * below - passing NULL here leaves that call to us.
     */
    so_set_options(NULL, lib_dir);

    so_module *mod = so_load_module(kNativeLib, NULL, NULL);
    if (!mod) {
        fatal("could not load '%s' from '%s'.", kNativeLib, lib_dir);
        fflush(NULL);
        _exit(1);
    }

    trace("so_load_module returned (text_base=%p size=%zu)",
          (void *)mod->text_base, (size_t)mod->text_size);
    /* Lets a guest-PC profile (QEMU_GUEST_PROF) be mapped back to this executable's symbols. */
    trace("port main() is at %p", (void *)&main);
    trace("libc addrs: memcpy=%p memset=%p strlen=%p sinf=%p mutex_lock=%p",
          (void *)&memcpy, (void *)&memset, (void *)&strlen, (void *)(float (*)(float))&sinf,
          (void *)&pthread_mutex_lock);

    /* ---------------------------------------------------------------- *
     * The boot sequence, in the game's own order.
     * ---------------------------------------------------------------- */

    auto JNI_OnLoad = (int (*)(JavaVM *, void *))so_symbol(mod, "JNI_OnLoad");
    auto GL2JNILib_init = (void (*)(JNIEnv *, jclass))
        so_symbol(mod, NOVA3_GL2JNILIB("init"));
    auto GL2JNILib_initGL = (void (*)(JNIEnv *, jclass))
        so_symbol(mod, NOVA3_GL2JNILIB("initGL"));
    auto GL2JNILib_InitViewSettings = (void (*)(JNIEnv *, jclass))
        so_symbol(mod, NOVA3_GL2JNILIB("InitViewSettings"));
    auto GL2JNILib_SetDepthValue = (void (*)(JNIEnv *, jclass, jint))
        so_symbol(mod, NOVA3_GL2JNILIB("SetDepthValue"));
    auto GL2JNILib_resize = (void (*)(JNIEnv *, jclass, jint, jint))
        so_symbol(mod, NOVA3_GL2JNILIB("resize"));
    auto GL2JNILib_step = (void (*)(JNIEnv *, jclass))
        so_symbol(mod, NOVA3_GL2JNILIB("step"));

    if (!JNI_OnLoad || !GL2JNILib_init || !GL2JNILib_initGL || !GL2JNILib_step) {
        fatal("%s is missing one of the entry points the port drives:\n"
              "       JNI_OnLoad=%p init=%p initGL=%p step=%p",
              kNativeLib, (void *)JNI_OnLoad, (void *)GL2JNILib_init,
              (void *)GL2JNILib_initGL, (void *)GL2JNILib_step);
        fflush(NULL);
        _exit(1);
    }

    /*
     * A real class object, not a sentinel.
     *
     * The reference ports for this engine family hand 0x42424242 to every
     * entry point and get away with it because their fake JNI dispatches on
     * the *value* of a jclass. portbase resolves methods through a registry
     * and the engine's own code reads the object it is handed, so what an
     * entry point is given has to be the kind of thing it is about to be used
     * as. GL2JNIActivity is the class these methods belong to on Android, so
     * it is also what a real JVM would pass.
     */
    jclass game_class = (jclass)&GloftGL2JNIActivity::clazz;

    trace("-> JNI_OnLoad at +0x%08lx",
          (unsigned long)((uintptr_t)JNI_OnLoad - mod->text_base));
    JNI_OnLoad(vm, NULL);
    trace("JNI_OnLoad returned");

    /* The licence bookkeeping, before the first thing that writes through it.
     * See game/patch.cpp. */
    nova3_unlock_drm(mod);
    nova3_xml_hook_install(mod);
    nova3_tweaks_install(mod);

    /* Input before the engine's own init: init() is where the engine builds
     * the input state the entry points write into, and the resolution above
     * has to have happened before anything can be sent. */
    android_input_init(mod, env, kWidth, kHeight);

    GL2JNILib_init(env, game_class);
    trace("GL2JNILib.init returned");

    /*
     * setPaths is NOT called from here, and the temptation to add it as a
     * safety net is what this comment exists to stop.
     *
     * Calling it right after init() returns faults inside glf::Fs::SetObbDir,
     * three frames down from the strcpy that lands the SIGSEGV:
     *
     *     8d0860  bl glf::Fs::SetObbDir      <- the frame the fault is under
     *     8e8730  bl strcpy@plt              <- dest = 0x000100d0
     *
     * The destination is a field of the Fs the App owns, and at that point the
     * App pointer glf::AndroidSetPaths reads is still a small uninitialised
     * value - init() has printed "AndroidInit" and "AndroidCreateView" but has
     * not built the filesystem object yet. The engine calls setupPaths back
     * through JNI itself once it has, and the fake activity answers; that is
     * the only moment at which the four strings have somewhere to go.
     */

    nova3_input_select_gamepad_mode();

    /*
     * initGL and InitViewSettings, in the order the EGL config chooser calls
     * them on Android. Both run after the context exists here; on the device
     * they run just before it does, and nothing in either reads GL state -
     * they set up the engine's own renderer bookkeeping.
     */
    GL2JNILib_initGL(env, game_class);
    trace("GL2JNILib.initGL returned");

    if (GL2JNILib_InitViewSettings) {
        GL2JNILib_InitViewSettings(env, game_class);
        trace("GL2JNILib.InitViewSettings returned");
    }

    if (GL2JNILib_SetDepthValue) {
        /* 16, matching the depth buffer SDL was asked for above. The engine
         * uses this to pick its depth range, so the two have to agree. */
        GL2JNILib_SetDepthValue(env, game_class, 16);
        trace("GL2JNILib.SetDepthValue(16) returned");
    }

    if (GL2JNILib_resize) {
        GL2JNILib_resize(env, game_class, kWidth, kHeight);
        trace("GL2JNILib.resize(%d, %d) returned", kWidth, kHeight);
    }

    emulator_control_init();

    /*
     * A bounded run. <PREFIX>_FRAME_LIMIT stops the process after that many
     * frames so an automated run terminates on a fact rather than on a
     * stopwatch; unset - the normal case for a player - means run forever.
     */
    const long frame_limit = port_getenv_long("FRAME_LIMIT", 0);

    long frames = 0;
    while (frame_limit == 0 || frames < frame_limit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (!android_input_event(&ev))
                goto done;
        }

        /* The game's own exit request, read next to the event drain rather
         * than trusted to it: a full SDL queue drops the SDL_QUIT that
         * android_app_request_exit() pushes, and a dropped exit is a freeze. */
        if (android_app_exit_requested())
            goto done;

        if (!emulator_control_tick(frames))
            goto done;
        android_input_tick();
        android_input_autopilot_tick(frames);

        /* Entering and returning, for the first few. "frames=1" as the last
         * line cannot distinguish "the second frame never started" from "the
         * second frame never came back", and those are different bugs. */
        if (frames < 5)
            trace("-> GL2JNILib.step #%ld", frames + 1);

        gl_bridge_profile_step(1);
        GL2JNILib_step(env, game_class);
        gl_bridge_profile_step(0);
        frames++;

        if (frames <= 5)
            trace("<- GL2JNILib.step #%ld returned", frames);

        if (window) {
            int w = 0, h = 0;
            SDL_GL_GetDrawableSize(window, &w, &h);
            android_fb_probe(frames, w, h);
            android_input_autopilot_sample(frames);
            android_cursor_draw(w, h);
            emulator_control_after_draw(frames, w, h);
            if (gl_bridge_enabled())
                gl_bridge_swap();
            else
                SDL_GL_SwapWindow(window);
        }

        if (frames <= 5 || frames % 10 == 0)
            trace("frames=%ld", frames);

        /* Whether the engine is in a menu or in a mission. "It froze" and "it
         * is drawing a screen that does not change" look the same from
         * outside, and the frame counter cannot tell them apart. */
        if (frames % 100 == 0)
            trace("activity f=%ld mainmenu=%d", frames, (int)nova3_is_main_menu());
    }

done:
    emulator_control_shutdown(frames);
    trace("frames=%ld", frames);
    /* The three counters the harness reads to tell "the engine is rendering
     * its own content" apart from "the loader cleared the screen 600 times".
     * A solid-colour clear passes a pixel test on its own; assets opened and
     * textures uploaded are what a game that found its data looks like. */
    trace("summary assets=%ld textures=%ld draws=%ld",
          android_io_assets_opened(), android_gl_textures_uploaded(),
          android_gl_draw_calls());
    trace("autopilot keys=%ld scenes=%ld",
          android_input_autopilot_keys(), android_input_autopilot_scenes());
    trace("run finished: %ld frames (%s)", frames,
          android_app_exit_requested() ? "the game asked to exit"
                                       : "the loader stopped driving it");

    /*
     * The engine started threads of its own and they are still running.
     * Tearing the window and the GL context down from here would pull them out
     * from under those threads and produce a segfault that has nothing to do
     * with why the loader stopped. Leave it to the kernel.
     */
    fflush(NULL);
    _exit(0);
}
