/*
 * Engine settings the device profile would otherwise decide.
 *
 *   <PREFIX>_PHYSICS_THREAD=1   physics on its own thread, whatever the profile
 *                               says. Under qemu-user guest threads run on
 *                               separate host cores, so this takes physics off
 *                               the one core the whole game is otherwise
 *                               waiting on.
 *
 *   <PREFIX>_FOV_PERCENT=145    gameplay camera view widened to 145 % (tan of the half angle).
 *                               CCameraSceneNode::setFOV is a leaf of six instructions (store the
 *                               float at +0x128, set a dirty bit at +0x300); it is replaced
 *                               outright, so every camera (weapon and HUD ones too) goes through it.
 *
 *   <PREFIX>_FAST_CAS=0         turn off the atomics fix below (on by default).
 *                               The library was built for ARMv5: its __sync_* functions (refcounts,
 *                               locks - about 13 % of the main thread's time) call the kernel's
 *                               cmpxchg helper at 0xffff0fc0, which qemu-user emulates by trapping
 *                               out of translated code on every call. The literal holding that
 *                               address is pointed at a small ldrex/strex routine instead.
 *
 *   <PREFIX>_UCLAMP=0           leave the scheduler's clamp on the main thread alone (see
 *                               nova3_sched_tick; on by default).
 *   <PREFIX>_UCLAMP_MIN=0..1024 also ask for at least this much (0 by default).
 *
 * The setter is forced rather than called once: the profile is applied again
 * whenever a level loads, and a one-off call would be undone by it.
 * DeviceOptions::SetUsePhysicsThread(bool) is a local symbol of the 1.0.7
 * library (.symtab only), found by offset like the XML hooks.
 */
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "so_util.h"
#include "trace.h"
#include "port_env.h"

#include "nova3.h"

typedef void (*set_bool_fn)(void *self, int on);

static ReentrantHook   g_physics_hook;
static set_bool_fn     g_set_physics;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void set_physics_hook(void *self, int on)
{
    static int logged;
    if (logged++ < 8)
        trace("tweaks: SetUsePhysicsThread(%d) -> 1", on);
    pthread_mutex_lock(&g_lock);
    rehook_unhook(&g_physics_hook);
    g_set_physics(self, 1);
    rehook_hook(&g_physics_hook);
    pthread_mutex_unlock(&g_lock);
}

static ReentrantHook g_fov_hook;
static float         g_fov_scale = 1.0f;

/* The view widens by scaling tan(fov/2), what the projection matrix is built from, so
 * FOV_PERCENT=145 shows 1.45x as much width at the screen edge. The game changes the angle by
 * itself while running or aiming (animation tracks write the member, not only setFOV), so the
 * scaling sits in CCameraSceneNode::recalculateProjectionMatrix: the stored angle is replaced
 * for the length of the call and put back, the game never sees the widened value.
 * Only gameplay cameras (fovy < 1 rad) are touched: the 3D HUD cameras (1.26-1.29 rad) keep
 * theirs, otherwise the interface would drift. */
typedef void (*recalc_fn)(void *self);
static recalc_fn g_recalc;

static void recalc_hook(void *self)
{
    static float seen[32];
    static int   nseen;
    float *fovy = (float *)((char *)self + 0x128);
    float orig = *fovy, f = orig;
    if (orig > 0.05f && orig < 1.0f)
        f = 2.0f * atanf(g_fov_scale * tanf(orig * 0.5f));
    if (f > 2.4f)
        f = 2.4f;
    pthread_mutex_lock(&g_lock);
    int i;
    for (i = 0; i < nseen && seen[i] != orig; i++) {}
    if (i == nseen && nseen < 32) {
        seen[nseen++] = orig;
        trace("tweaks: projection fovy %.3f -> %.3f", orig, f);
    }
    *fovy = f;
    rehook_unhook(&g_fov_hook);
    g_recalc(self);
    rehook_hook(&g_fov_hook);
    *fovy = orig;
    pthread_mutex_unlock(&g_lock);
}

static void install_fast_cas(so_module *mod)
{
    /* kuser cmpxchg: r0 = old, r1 = new, r2 = ptr; returns r0 = 0 when it stored. */
    static const uint32_t code[] = {
        0xf57ff05b, /* dmb ish            */
        0xe1923f9f, /* 1: ldrex r3,[r2]   */
        0xe0533000, /* subs r3,r3,r0      */
        0x01823f91, /* strexeq r3,r1,[r2] */
        0x03330001, /* teqeq r3,#1        */
        0x0afffffa, /* beq 1b             */
        0xf57ff05b, /* dmb ish            */
        0xe1a00003, /* mov r0,r3          */
        0xe12fff1e, /* bx lr              */
    };
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        trace("tweaks: fast cas: mmap failed");
        return;
    }
    memcpy(page, code, sizeof(code));
    uint32_t *w = (uint32_t *)mod->text_base;
    size_t n = mod->text_size / 4, hits = 0;
    for (size_t i = 0; i < n; i++) {
        if (w[i] == 0xffff0fc0u) {
            w[i] = (uint32_t)(uintptr_t)page;
            hits++;
        }
    }
    trace("tweaks: kuser cmpxchg literal replaced at %zu places (routine %p)", hits, page);
}

static uintptr_t function_at(so_module *mod, uintptr_t offset, const char *what)
{
    /* push within the first two instructions (a pc-relative load may come
     * first; the hook restores the original words in place for each call, so
     * pc-relative code is safe). */
    uintptr_t addr = mod->text_base + offset;
    const uint32_t *w = (const uint32_t *)addr;
    if ((w[0] & 0xFFFF0000u) != 0xE92D0000u && (w[1] & 0xFFFF0000u) != 0xE92D0000u) {
        trace("tweaks: no push at %s (+0x%lx), not the 1.0.7 library - skipped", what, (unsigned long)offset);
        return 0;
    }
    return addr;
}

void nova3_tweaks_install(so_module *mod)
{
    if (port_getenv_long("FAST_CAS", 1))
        install_fast_cas(mod);

    if (port_getenv_long("PHYSICS_THREAD", 0)) {
        uintptr_t a = function_at(mod, 0x5b1038, "DeviceOptions::SetUsePhysicsThread");
        if (a) {
            g_set_physics = (set_bool_fn)a;
            rehook_new(mod, &g_physics_hook, a, (uintptr_t)set_physics_hook);
            rehook_hook(&g_physics_hook);
            trace("tweaks: physics thread forced on");
        }
    }

    long fov = port_getenv_long("FOV_PERCENT", 100);
    if (fov != 100 && fov > 20 && fov < 300) {
        uintptr_t a = function_at(mod, 0x82e80c, "CCameraSceneNode::recalculateProjectionMatrix");
        if (a) {
            g_fov_scale = fov / 100.0f;
            g_recalc = (recalc_fn)a;
            rehook_new(mod, &g_fov_hook, a, (uintptr_t)recalc_hook);
            rehook_hook(&g_fov_hook);
            trace("tweaks: field of view x%.2f", g_fov_scale);
        }
    }
}

/*
 * The phone's scheduler puts a utilisation clamp on the game's main thread a few seconds into a
 * mission: uclamp.max 171-199 of 1024, on that one thread and no other. To the frequency governor the
 * thread then never looks more than a fifth loaded, and the core stays at its lowest clock while the
 * thread is in fact saturated (util 1023, 1.0 GHz of 4.3). The clamp is a per-task attribute its owner
 * may set, so the main thread sets it back - again and again, because whatever lowers it does so
 * repeatedly.
 *
 * sched_setattr is not something Android apps normally call, and a system call the seccomp filter
 * does not know kills the process, so the first call is made in a forked child.
 */
struct nova3_sched_attr {
    uint32_t size, sched_policy;
    uint64_t sched_flags;
    int32_t  sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime, sched_deadline, sched_period;
    uint32_t sched_util_min, sched_util_max;
};

static int set_uclamp(uint32_t lo)
{
    nova3_sched_attr a;
    memset(&a, 0, sizeof(a));
    a.size = sizeof(a);
    a.sched_flags = 0x08 /* KEEP_POLICY */ | 0x10 /* KEEP_PARAMS */ | 0x20 /* UTIL_CLAMP_MIN */ | 0x40 /* UTIL_CLAMP_MAX */;
    a.sched_util_min = lo;
    a.sched_util_max = 1024;
    return syscall(SYS_sched_setattr, 0, &a, 0) == 0 ? 0 : errno;
}

void nova3_sched_tick(long frames)
{
    static int state = -1;      /* -1 not tried, 0 off, 1 working */
    static uint32_t lo;

    if (state == 0 || (state == 1 && frames % 8 != 0))
        return;
    if (state < 0) {
        state = 0;
        if (!port_getenv_long("UCLAMP", 1))
            return;
        long v = port_getenv_long("UCLAMP_MIN", 0);
        lo = v < 0 ? 0 : v > 1024 ? 1024 : (uint32_t)v;
        pid_t child = fork();
        if (child < 0)
            return;
        if (child == 0)
            _exit(set_uclamp(lo) & 0x7f);
        int st = 0;
        if (waitpid(child, &st, 0) != child || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            trace("tweaks: uclamp: sched_setattr unusable here (%s %d) - left alone",
                  WIFEXITED(st) ? "errno" : "signal", WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
            return;
        }
        state = 1;
        trace("tweaks: uclamp: main thread kept at min %u, max 1024", lo);
    }
    int e = set_uclamp(lo);
    if (e) {
        trace("tweaks: uclamp: sched_setattr failed (errno %d) - stopped", e);
        state = 0;
    }
}
