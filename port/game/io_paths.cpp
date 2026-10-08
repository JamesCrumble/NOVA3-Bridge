/*
 * Path translation for N.O.V.A. 3 (1.0.7).
 *
 * See portbase/src/io_paths.h for the split between what portbase answers and
 * what only this game knows. Everything below was read out of the donor's own
 * .rodata rather than guessed - the offsets are from
 * `strings -a -t x libNOVA3_neon.so`, so they can be checked:
 *
 *   c7ce14  /data/data/com.gameloft.android.ANMP.GloftN3HM
 *   c7c55c  obbPath: %s / resourcePath: %s / homePath: %s / tempPath: %s
 *
 * The four directories are not discovered by the engine: Java hands them over
 * through setPaths, and this port is that Java. So the rewrite rules here are
 * a safety net for the names the engine builds on its own - the /data/data
 * spelling it uses for its nine a1..a9.dat lock files, and the two /sdcard
 * roots its Java layer would otherwise have computed.
 *
 * The game splits its filesystem in two and this port has to keep the split,
 * because the two halves have different permissions on a PortMaster install:
 *
 *   /data/data/...  and  /sdcard/Android/data/...  are where it WRITES - its
 *   trophies, its preferences, its lock files. The game tree is the player's
 *   own copy and can sit on a read-only mount, so these go to
 *   io_writable_dir().
 *
 *   /sdcard/Android/obb/...  is where it READS the two expansion files from.
 *   Those are the game data; they go to the package directory inside
 *   io_game_dir(), which is exactly what that Android path maps to.
 *
 * Collapsing both onto the game directory is the mistake that is easy to make
 * here and hard to see: it works on a writable card and fails on a read-only
 * one, at the first save, long after the port looks healthy.
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "io_paths.h"
#include "so_util.h"
#include "trace.h"

#include "jni.h"
#include "jni_internals.h"

#include "nova3.h"

/* The package, in the spellings the binary uses. */
static const char kPackage[]  = "com.gameloft.android.ANMP.GloftN3HM";
static const char kDataData[] = "/data/data/com.gameloft.android.ANMP.GloftN3HM";
static const char kExtFiles[] = "/sdcard/Android/data/com.gameloft.android.ANMP.GloftN3HM";
static const char kObbDir[]   = "/sdcard/Android/obb/com.gameloft.android.ANMP.GloftN3HM";

const char *nova3_obb_dir(void)
{
    static char dir[PATH_MAX];
    if (!dir[0])
        snprintf(dir, sizeof(dir), "%s/%s", io_game_dir(), kPackage);
    return dir;
}

/*
 * Re-root `path` at `root`, keeping whatever followed `prefix`.
 *
 * The tail can be empty (the game asks for the bare directory when it builds
 * its own paths by concatenation) and it can start with or without a slash, so
 * the separator is normalised here rather than at each call site.
 */
static const char *reroot(const char *path, const char *prefix, const char *root,
                          char *buf, size_t bufsz)
{
    const char *tail = path + strlen(prefix);
    while (*tail == '/')
        tail++;

    if (*tail)
        snprintf(buf, bufsz, "%s/%s", root, tail);
    else
        snprintf(buf, bufsz, "%s", root);

    return buf;
}

const char *port_fix_path(const char *orig, char *buf, size_t bufsz)
{
    if (!orig || !*orig)
        return NULL;

    /*
     * The engine dots its own absolute paths, so undot them before matching.
     *
     * Several of glf's file helpers build the name they open as
     * `sprintf(".%s", path)`. Whatever they were given comes back with a "."
     * glued to the front, so a path this port handed the engine through
     * setPaths -
     *
     *     resourcePath -> /root/.local/share/nova3
     *
     * - reaches the libc thunks as "./root/.local/share/nova3/sprites.gla",
     * which is a *relative* name. portbase's generic fallback then resolves it
     * against the game directory and looks for
     * '/game/./root/.local/share/nova3/sprites.gla', which is what the first
     * run's log shows twice, once per pack, right before
     *
     *     assert false && "could not find sprites pack!" failed 2391
     *
     * A leading "./" in front of what is otherwise an absolute path can only
     * have come from that concatenation: a genuine relative name does not
     * begin with a slash after the dot. Dropping the dot is therefore safe,
     * and only when the tail really is one of the roots this port knows -
     * "./data/foo" is a relative name the engine means literally, and
     * undotting it would send the open to the host's own /data.
     */
    bool undotted_here = false;

    if (orig[0] == '.' && orig[1] == '/') {
        const char *undotted = orig + 1;
        const char *roots[] = {
            io_game_dir(), io_writable_dir(), nova3_obb_dir(),
            kObbDir, kExtFiles, kDataData,
        };

        for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
            size_t n = roots[i] ? strlen(roots[i]) : 0;

            if (n && strncmp(undotted, roots[i], n) == 0 &&
                (undotted[n] == '/' || undotted[n] == '\0')) {
                orig = undotted;
                undotted_here = true;
                break;
            }
        }
    }

    /*
     * Order matters: the obb directory and the external files directory both
     * begin "/sdcard/Android/", and only the longer match is the right one.
     * Testing the obb path first is what keeps an expansion file from being
     * looked for in the save directory.
     */
    if (strncmp(orig, kObbDir, sizeof(kObbDir) - 1) == 0)
        return reroot(orig, kObbDir, nova3_obb_dir(), buf, bufsz);

    if (strncmp(orig, kExtFiles, sizeof(kExtFiles) - 1) == 0)
        return reroot(orig, kExtFiles, io_writable_dir(), buf, bufsz);

    if (strncmp(orig, kDataData, sizeof(kDataData) - 1) == 0)
        return reroot(orig, kDataData, io_writable_dir(), buf, bufsz);

    /*
     * An undotted host path is already the answer.
     *
     * The Android prefixes above did not match because there is nothing left
     * to translate: the name came out of a path this port itself handed the
     * engine. Returning NULL here would give portbase the *dotted* original -
     * the buffer is the only way back - and it would resolve it against the
     * game directory again.
     */
    if (undotted_here) {
        snprintf(buf, bufsz, "%s", orig);
        /* The engine reads its data as flat files under resourcePath, which
         * the Android installer would have unpacked out of the two .obb
         * containers. Nothing has unpacked them here, so a miss is answered by
         * extracting that one entry. See game/obb_cache.cpp. */
        nova3_obb_materialise(buf);
        return buf;
    }

    /*
     * No rule. NULL, not `orig`: returning the original claims the name is
     * already correct and skips portbase's generic fallbacks - the bare
     * relative names this engine also uses would then be resolved against the
     * loader's working directory instead of the game tree.
     */
    return NULL;
}

/*
 * This build never opens an Android platform font.
 *
 * There is no "/system/fonts" anywhere in the donor and no DroidSans; the five
 * .ttf names it does carry (Days, NewCezannePro-DB, WenQuanYiZenHei,
 * NanumGothic, neuropol) are the game's own, and they live inside the
 * expansion files. Answering with something anyway would redirect a name the
 * engine never asks for.
 */
const char *port_system_font(void)
{
    return NULL;
}

/*
 * The directories the engine writes into without ever creating them.
 *
 * On Android the framework has already made all of them by the time any native
 * code runs - getFilesDir() and getCacheDir() create on demand, and
 * GL2JNILib.setupPaths() calls ensurePathExists() on the other two. Here
 * nothing has, and the engine does not check: it takes the null stream from a
 * failed fopen and seeks on it, which lands inside portbase's FILE* thunk with
 * nothing in the frame naming a directory.
 *
 * portbase creates the writable root and stops there, which is right - it
 * cannot know which subdirectories an engine expects to find already made.
 * These three are ours, and they are the same three setupPaths() ensures.
 */
void nova3_prepare_writable_storage(void)
{
    static const char *const kSubdirs[] = { "home", "cache" };
    char path[PATH_MAX];

    for (unsigned i = 0; i < sizeof(kSubdirs) / sizeof(kSubdirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", io_writable_dir(), kSubdirs[i]);
        if (mkdir(path, 0777) == 0)
            trace("writable storage: created %s", path);
    }
}

/*
 * setPaths, called once.
 *
 * The engine reaches back into Java for setupPaths() from inside
 * GL2JNILib.init(), and the fake activity answers by coming here. main() calls
 * this again after init() returns in case the callback never arrived; the
 * guard makes the second call free and the trace says which route got there,
 * because "the engine asked" and "the loader had to" are different facts about
 * the port.
 */
void nova3_set_paths(_JNIEnv *env)
{
    static bool done = false;
    if (done)
        return;
    done = true;

    so_module *mod = nova3_module();
    if (!mod) {
        trace("setPaths skipped: the module is not loaded yet");
        done = false;
        return;
    }

    auto setPaths = (void (*)(JNIEnv *, jclass, jstring, jstring, jstring, jstring))
        so_symbol(mod, NOVA3_GL2JNILIB("setPaths"));
    if (!setPaths) {
        trace("setPaths: the module exports no GL2JNILib.setPaths");
        return;
    }

    char home[PATH_MAX], temp[PATH_MAX];
    snprintf(home, sizeof(home), "%s/home",  io_writable_dir());
    snprintf(temp, sizeof(temp), "%s/cache", io_writable_dir());

    JNIEnv *e = (JNIEnv *)env;
    jstring obb  = e->NewStringUTF(nova3_obb_dir());
    jstring data = e->NewStringUTF(io_writable_dir());
    jstring hom  = e->NewStringUTF(home);
    jstring tmp  = e->NewStringUTF(temp);

    trace("setPaths obb='%s' data='%s' home='%s' temp='%s'",
          nova3_obb_dir(), io_writable_dir(), home, temp);

    setPaths(e, NULL, obb, data, hom, tmp);
    trace("GL2JNILib.setPaths returned");
}
