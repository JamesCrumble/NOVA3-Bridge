/*
 * Path translation for THIS game. See portbase/src/io_paths.h for the split.
 *
 * The compiling part of this file is the minimal contract: identity paths
 * and a font served from the player's own tree. Below it, inside #if 0,
 * sits the worked example lifted from the Real Racing 3 port - every rule
 * shape a port tends to need (a vendor URI scheme, a package directory,
 * packed-resource prefixes, an asset-pack downscale). It references RR3's
 * own globals, so it is reference material, not code: read it for the
 * shape, then write your game's rules in the live section.
 */

#include "io_paths.h"

#include <atomic>

#include "trace.h"

#include <stdio.h>

/* ------------------------------------------------------------------ live --
 * Rules earn their place here by appearing in a trace, not by being copied
 * from a sibling. Until a milestone shows a path the engine invents on its
 * own, identity is correct.
 */

const char *port_fix_path(const char *orig, char *buf, size_t bufsz)
{
    (void)buf;
    (void)bufsz;
    return orig;
}

/*
 * Gameloft-era engines hard-code /system/fonts/DroidSans.ttf; answering NULL
 * crashes two calls later inside FreeType (see the worked example for the
 * full story). Serve a file from the player's own tree - which one is the
 * port's choice.
 */
const char *port_system_font(void)
{
    static char path[512];
    const char *dir = io_game_dir();
    if (!dir)
        return NULL;
    snprintf(path, sizeof(path), "%s/DroidSans.ttf", dir);
    return path;
}

/* -------------------------------------------- worked example (RR3) --------
 * Reference only: references RR3's globals and does not compile as-is.
 */
#if 0

static const char kScheme[]   = "appbundle:/";
static const char kAndroid[]  = "Android/data/com.ea.realracing3/files/";
static const char kSysFonts[] = "/system/fonts/";

/*
 * The font the engine expects the operating system to provide.
 *
 * im::IFont::CreateDefaultFont() hard-codes "/system/fonts/DroidSans.ttf" - an
 * Android platform font: absolute, no scheme, and nothing about it that any of
 * the rules below would touch. There is no such file here, FreeType hands back
 * a null face, and the crash lands two calls later inside FT_Request_Size as a
 * null dereference at +0x14 - which reads as a bug in FreeType rather than as a
 * font that was never opened.
 *
 * The Vita port hits this too and redirects the same names to a Roboto it ships
 * (vita-ref/loader/reimpl/io.c). This port answers with the game's own font
 * instead: it is already in the tree the player supplied, so nothing has to be
 * bundled or redistributed, and it is the typeface the game's UI uses anyway.
 * Any /system/fonts name resolves to it - the engine also asks for
 * DroidSansFallback.ttf, and a fallback that fails is the same crash.
 */
static const char kGameFont[] = "assets/published/fonts/EurostileLTStd.ttf";

/*
 * /proc/cpuinfo, when the kernel underneath is not the one the game expects.
 *
 * FMOD decides what this CPU can do by reading /proc/cpuinfo and looking for
 * the literal tokens "vfp" and "neon" in the Features line. If it finds
 * neither, it refuses to register ANY audio output - not OpenSL, not
 * AudioTrack - and every later call fails with FMOD_RESULT 48. The game prints
 * that as one line and carries on, so the whole audio stack is gone and the
 * only trace is a number.
 *
 * A 32-bit ARM kernel prints those tokens, which is why this never came up on
 * the console. A 64-bit kernel running a 32-bit process prints the AArch64
 * spelling instead - "fp asimd ..." - for the same silicon with the same
 * capabilities. That is the whole bug: the hardware is fine and the words
 * changed. It bites under qemu-arm on an arm64 host, which is exactly where
 * this port is developed, and it made the emulator unable to reproduce - or
 * disprove - any audio problem at all.
 *
 * So the substitution is only made when the real file is missing the tokens,
 * and it describes the machine the guest is actually running on. Passing the
 * kernel's own file through whenever it already says vfp/neon keeps the
 * console on the truth.
 */

static const char *fix_path_inner(const char *orig, char *buf, size_t bufsz)
{
    if (!orig || !*orig)
        return orig;

    /* Rule -1: /proc/cpuinfo, only when the kernel's spelling would cost the
     * guest its audio entirely. See cpuinfo_override(). */
    if (strcmp(orig, "/proc/cpuinfo") == 0) {
        const char *override = cpuinfo_override();
        if (override) {
            snprintf(buf, bufsz, "%s", override);
            return buf;
        }
        return orig;
    }

    /* Rule 0: Android's platform fonts. First because these paths are absolute
     * and would otherwise fall through every rule below unchanged. */
    if (strstr(orig, kSysFonts)) {
        snprintf(buf, bufsz, "%s/%s", g_game_dir, kGameFont);
        return buf;
    }

    /* Rule 1: the scheme. Matched with strstr, not a prefix test, because the
     * engine sometimes concatenates it onto a directory it already built. */
    const char *scheme = strstr(orig, kScheme);
    if (scheme) {
        snprintf(buf, bufsz, "%s/assets/%s", g_game_dir,
                 scheme + sizeof(kScheme) - 1);
        return buf;
    }

    /* Rules 2 and 3 both operate on the same working copy. */
    char work[PATH_MAX];
    snprintf(work, sizeof(work), "%s", orig);
    bool changed = false;

    /* Android's packaged resource candidates are absolute in the APK
     * process, but the port mounts the extracted .depot tree at game_dir. */
    const char *packed[] = { "/.depot/", "/apk/res/", "/doc/" };
    for (const char *prefix : packed) {
        if (strncmp(work, prefix, strlen(prefix)) == 0) {
            snprintf(buf, bufsz, "%s/%s", g_game_dir, work + strlen(prefix));
            return buf;
        }
    }

    char *android = strstr(work, kAndroid);
    if (android) {
        memmove(android, android + sizeof(kAndroid) - 1,
                strlen(android + sizeof(kAndroid) - 1) + 1);
        changed = true;
    }

    /*
     * Rule 3, expressed against the game directory rather than as a literal.
     *
     * The reference spells it "realracing3/published" -> "realracing3/assets/
     * published" because its data root is a compile-time constant ending in
     * "realracing3". Ours is argv[1], handed to the engine by
     * com/ea/blast/GetAppDataDirectoryDelegate, so the same rewrite has to key
     * off that: the engine believes published/ hangs off the root, and in the
     * extracted tree it hangs off assets/.
     */
    size_t root_len = strlen(g_game_dir);
    if (strncmp(work, g_game_dir, root_len) == 0 &&
        strncmp(work + root_len, "/published", 10) == 0) {
        char tail[PATH_MAX];
        snprintf(tail, sizeof(tail), "%s", work + root_len);
        snprintf(work, sizeof(work), "%s/assets%s", g_game_dir, tail);
        changed = true;
    }

    /* The literal form as well, for any path the engine built before it had a
     * root from us - it still names the vendor directory. */
    char *published = strstr(work, "realracing3/published");
    if (published) {
        char tail[PATH_MAX];
        snprintf(tail, sizeof(tail), "%s", published + strlen("realracing3/"));
        snprintf(published, sizeof(work) - (size_t)(published - work),
                 "realracing3/assets/%s", tail);
        changed = true;
    }

    /* The divergence described above: re-root anything still pointing at an
     * asset tree we are not mounted at. */
    char *assets = strstr(work, "realracing3/assets/");
    if (assets) {
        snprintf(buf, bufsz, "%s/%s", g_game_dir,
                 assets + strlen("realracing3/"));
        return buf;
    }

    /* Cloudcell's extracted data store is addressed by bare numeric object
     * names ("1", "3", ...). The donor keeps those objects under CC_Data,
     * while ordinary bare names remain rooted at the game directory. Only
     * redirect when the corresponding donor file actually exists. */
    bool numeric = true;
    for (const char *p = orig; *p; ++p) {
        if (*p < '0' || *p > '9') {
            numeric = false;
            break;
        }
    }
    if (numeric && *orig) {
        char cc_data[PATH_MAX];
        snprintf(cc_data, sizeof(cc_data), "%s/CC_Data/%s", g_game_dir, orig);
        if (access(cc_data, F_OK) == 0) {
            snprintf(buf, bufsz, "%s", cc_data);
            return buf;
        }
    }

    if (!changed) {
        /* RR3's EAIO passes bare content names (for example gametext.txt)
         * through the libc boundary.  The qemu process is launched from the
         * loader tree, not from the mounted game tree, so leaving these
         * relative would make valid donor files look absent. */
        if (orig[0] != '/') {
            snprintf(buf, bufsz, "%s/%s", g_game_dir, orig);
            return buf;
        }
        return orig;
    }

    snprintf(buf, bufsz, "%s", work);
    return buf;
}

/*
 * Serve every texture from the smallest art pack.
 *
 * The game ships three of them - assets_480x320 (17 MB), assets_960x640 (54 MB)
 * and assets_2048x1536 (191 MB) - and picks per asset, not once at startup: a
 * single emulator run opened 1157 files from the small pack, 830 from the
 * medium one and 896 from the largest. On a 640x480 panel every byte of the
 * 2048x1536 art is waste, and it is not cheap waste: a texture from that pack
 * costs roughly sixteen times the memory of its 480x320 twin, decoded or not.
 * The first hardware run died to the OOM killer at frame 239 with 685 MB
 * available, so this is the largest single lever the port has.
 *
 * Redirecting is safe because the three packs mirror each other: same 40
 * directories, same names, only the pixel dimensions differ. Even so the
 * substitution is conditional - if the small pack happens not to carry that
 * file, the original path is returned untouched, so a partial donor degrades
 * to the old behaviour instead of failing to open.
 *
 * <PREFIX>_ASSET_PACK overrides the target for anyone with memory to spare
 * and a taste for sharper art; <PREFIX>_ASSET_PACK=off disables the
 * redirect entirely.
 */
static const char *downscale_asset_pack(const char *path, char *buf, size_t bufsz)
{
    static const char *target = NULL;
    if (!target) {
        const char *env = port_getenv("ASSET_PACK");
        target = (env && *env) ? env : "assets_480x320";
    }
    if (!path || strcmp(target, "off") == 0)
        return path;

    const char *found = NULL;
    static const char *const packs[] = { "assets_2048x1536", "assets_960x640" };
    for (const char *pack : packs) {
        found = strstr(path, pack);
        if (found) {
            char candidate[PATH_MAX];
            size_t prefix = (size_t)(found - path);
            if (prefix >= sizeof(candidate))
                return path;
            memcpy(candidate, path, prefix);
            snprintf(candidate + prefix, sizeof(candidate) - prefix, "%s%s",
                     target, found + strlen(pack));
            if (access(candidate, F_OK) != 0)
                return path;

            static std::atomic<long> redirected(0);
            long n = ++redirected;
            if (n == 1 || n % 500 == 0)
                trace("asset pack: serving %s from %s (%ld redirected)",
                      pack, target, n);
            snprintf(buf, bufsz, "%s", candidate);
            return buf;
        }
    }
    return path;
}


#endif /* worked example */
