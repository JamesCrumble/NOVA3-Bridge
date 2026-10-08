/*
 * The expansion files, served as if the installer had unpacked them.
 *
 * This game does not read its data out of the .obb files. Its Java installer
 * activity unpacks them into the app's data directory first, and the engine
 * then opens flat files there - the first frame asks for
 *
 *     <resourcePath>/sprites.gla
 *     <resourcePath>/effects.gla
 *
 * and aborts with `assert false && "could not find sprites pack!" failed 2391`
 * when they are not present. Both are inside patch.1070.obb, which is an
 * ordinary zip:
 *
 *     $ unzip -l patch.1070....obb
 *     15 files, 392 MB   actors.gla effects.gla levels.gla sprites.gla ...
 *     $ unzip -l main.1050....obb
 *     4266 files, 3.6 GB  every level's .bdae and .irr, flat
 *
 * That installer is a licence check against a Play server that has been gone
 * for years, so this port does the unpacking itself - but on demand rather
 * than up front. Unpacking both containers eagerly means 4 GB written before
 * the title screen appears and 4 GB of card used on top of the 2.3 GB the
 * player already has. Extracting each entry the first time the engine asks for
 * it costs only what is actually played, keeps the donor tree read-only, and
 * leaves an install that has already been unpacked (by eapx, or by hand)
 * completely untouched: an entry that is already on disk is never looked up.
 *
 * Both containers are flat - no entry name contains a directory separator - so
 * the basename of what the engine asked for is the entry name, and there is
 * nothing to create alongside it. The patch is searched first: where the two
 * disagree, the patch is the newer file, which is what "patch" means here.
 */

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zip.h>

#include "io_paths.h"
#include "trace.h"

#include "nova3.h"

/* The two containers, by the names the donor tree carries. Version numbers and
 * all: a different build of the game ships different ones, and silently
 * accepting whatever .obb happens to be there would hide a mismatched donor
 * until the engine failed on a file it could not parse. */
static const char *const kContainers[] = {
    "patch.1070.com.gameloft.android.ANMP.GloftN3HM.obb",
    "main.1050.com.gameloft.android.ANMP.GloftN3HM.obb",
};

#define kContainerCount (sizeof(kContainers) / sizeof(kContainers[0]))

static zip_t *g_zip[kContainerCount];
static bool   g_opened;
static long   g_extracted;
static long   g_missing;

/* fix_path can be reached from several of the engine's threads at once - it
 * streams assets from a loader thread while the frame loop opens its own
 * files - and libzip's handles are not safe to share. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void open_containers(void)
{
    if (g_opened)
        return;
    g_opened = true;

    for (unsigned i = 0; i < kContainerCount; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", nova3_obb_dir(), kContainers[i]);

        int err = 0;
        g_zip[i] = zip_open(path, ZIP_RDONLY, &err);
        if (g_zip[i])
            trace("obb: opened %s (%lld entries)", kContainers[i],
                  (long long)zip_get_num_entries(g_zip[i], 0));
        else
            trace("obb: cannot open %s (libzip error %d) - anything only that "
                  "container holds will be missing", kContainers[i], err);
    }
}

/*
 * Write `entry` out of `z` to `dest`.
 *
 * Through a temporary and a rename, so a run interrupted halfway - the harness
 * kills the process on a timeout, and a player can pull the power - cannot
 * leave a half-written pack behind that the next run would find, believe, and
 * fail to parse somewhere far away from here.
 */
static bool extract_entry(zip_t *z, zip_int64_t index, const char *dest)
{
    zip_file_t *f = zip_fopen_index(z, index, 0);
    if (!f)
        return false;

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.part", dest);

    FILE *out = fopen(tmp, "wb");
    if (!out) {
        trace("obb: cannot write %s: %s", tmp, strerror(errno));
        zip_fclose(f);
        return false;
    }

    static char buf[256 * 1024];
    bool ok = true;
    zip_int64_t n;

    while ((n = zip_fread(f, buf, sizeof(buf))) > 0) {
        if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
            ok = false;
            break;
        }
    }
    if (n < 0)
        ok = false;

    fclose(out);
    zip_fclose(f);

    if (!ok || rename(tmp, dest) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

/*
 * Make `path` exist if one of the containers holds it.
 *
 * Returns true when the file is on disk afterwards - including when it already
 * was, which is the common case after the first run of a level.
 */
bool nova3_obb_materialise(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return true;

    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (!*name)
        return false;

    pthread_mutex_lock(&g_lock);

    /* Re-checked under the lock: two threads asking for the same pack at once
     * would otherwise both extract it, and the loser's rename would replace a
     * file the winner may already have open. */
    if (stat(path, &st) == 0) {
        pthread_mutex_unlock(&g_lock);
        return true;
    }

    open_containers();

    bool done = false;
    for (unsigned i = 0; i < kContainerCount && !done; i++) {
        if (!g_zip[i])
            continue;

        zip_int64_t index = zip_name_locate(g_zip[i], name, 0);
        if (index < 0)
            continue;

        done = extract_entry(g_zip[i], index, path);
        if (done) {
            /* The first few by name, then a count. The engine asks for
             * thousands of these and a line each would bury the log; the first
             * handful is what says whether the right container answered. */
            if (g_extracted < 8)
                trace("obb: extracted %s from %s", name, kContainers[i]);
            else if (g_extracted == 8)
                trace("obb: further extractions counted, not listed");
            g_extracted++;
        } else {
            trace("obb: failed to extract %s from %s", name, kContainers[i]);
        }
    }

    if (!done && g_missing < 8) {
        trace("obb: '%s' is in neither container", name);
        g_missing++;
    }

    pthread_mutex_unlock(&g_lock);
    return done;
}

long nova3_obb_extracted(void) { return g_extracted; }
