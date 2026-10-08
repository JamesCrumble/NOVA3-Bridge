/*
 * Engine settings the device profile would otherwise decide.
 *
 *   <PREFIX>_PHYSICS_THREAD=1   physics on its own thread, whatever the profile
 *                               says. Under qemu-user guest threads run on
 *                               separate host cores, so this takes physics off
 *                               the one core the whole game is otherwise
 *                               waiting on.
 *
 * The setter is forced rather than called once: the profile is applied again
 * whenever a level loads, and a one-off call would be undone by it.
 * DeviceOptions::SetUsePhysicsThread(bool) is a local symbol of the 1.0.7
 * library (.symtab only), found by offset like the XML hooks.
 */
#include <pthread.h>
#include <stdint.h>

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
    if (port_getenv_long("PHYSICS_THREAD", 0)) {
        uintptr_t a = function_at(mod, 0x5b1038, "DeviceOptions::SetUsePhysicsThread");
        if (a) {
            g_set_physics = (set_bool_fn)a;
            rehook_new(mod, &g_physics_hook, a, (uintptr_t)set_physics_hook);
            rehook_hook(&g_physics_hook);
            trace("tweaks: physics thread forced on");
        }
    }
}
