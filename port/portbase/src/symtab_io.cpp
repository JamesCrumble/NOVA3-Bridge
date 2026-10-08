#include "port_env.h"
#include "io_paths.h"
/*
 * Path-translating wrappers around the libc entry points the game opens files
 * with.
 *
 * ---------------------------------------------------------------------------
 * What was observed
 *
 * With com/ea/games/Util registered (see jni/classes/ea_games_Util.cpp) the
 * engine finally gets past its content gate and starts loading. Under
 * `qemu-arm -strace` the very first thing it does is:
 *
 *     openat(AT_FDCWD,"appbundle:/EAMCore.ini",O_RDONLY) = -1 ENOENT
 *     openat(AT_FDCWD,"appbundle:/eamcore.ini",O_RDONLY) = -1 ENOENT
 *
 * That is a literal Android URI reaching the host kernel. Two things are worth
 * naming about it:
 *
 *   - it is proof the three binary patches in src/patch.cpp do what they claim.
 *     Before them this path went out through JNI to AssetManager.open(); the
 *     open() syscall above is the fd path they switch the engine onto, and it
 *     is the reason those patches were kept even though they moved no
 *     milestone at the time.
 *   - the engine does NOT strip the scheme itself. On Android it never had to:
 *     "appbundle:" was the flag that sent the name down the AssetManager path,
 *     and the AssetManager was handed the remainder. With that branch patched
 *     out, whoever answers open() has to do the stripping.
 *
 * ---------------------------------------------------------------------------
 * The rules
 *
 * Taken from fix_path() in vita-ref/loader/reimpl/io.c, which runs this exact
 * build, rather than invented here:
 *
 *   appbundle:/X                            -> <gamedir>/assets/X
 *   .../Android/data/com.<vendor>.<game>/files/ -> that prefix deleted
 *   <game>/published                      -> <game>/assets/published
 *
 * The second and third exist because the engine also builds absolute paths out
 * of the app data directory it thinks it has. They are applied in that order
 * and are not mutually exclusive: a path can need the prefix removed and then
 * the published/ rewrite.
 *
 * One deliberate divergence from the reference. After those rules a path can
 * still be rooted somewhere that does not exist here (the Vita port's data
 * directory is a constant; ours is argv[1] and only known at runtime), so a
 * path that still names "<game>/assets/" anywhere is re-rooted onto the
 * game directory. Without that, any name the engine derives from
 * GetAppDataDirectory - a JNI call this port answers with NULL - would land
 * outside the mounted tree and fail the same silent ENOENT way the scheme did.
 *
 * ---------------------------------------------------------------------------
 * Why wrappers and not a chdir or a symlink farm
 *
 * "appbundle:" is not a path at all, so no amount of cwd or mount trickery
 * reaches it; it has to be rewritten by something. And the rewrite has to
 * happen on the libc boundary rather than by patching the engine's string
 * building, because the same names are built in several places (the ini
 * loader, the package reader, the save system) and only one of them was ever
 * disassembled.
 *
 * No allocation happens in here. The engine calls open() from inside its own
 * allocator's core-acquisition path, so a fix_path() that malloc'd - as the
 * reference one does - would re-enter an allocator that is mid-update.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <atomic>
#include <errno.h>

#include "so_util.h"
#include "thunk_gen.h"
#include "trace.h"
#include "fix_path.h"

/* argv[1]. Static storage rather than a pointer into argv because the thunks
 * outlive nothing in particular but the value has to be readable from any
 * thread the engine starts. */
static char g_game_dir[PATH_MAX] = "/game";
static std::atomic<long> g_assets_opened(0);
static int g_seed_fd = -1;

void io_set_game_dir(const char *dir)
{
    if (dir && *dir)
        snprintf(g_game_dir, sizeof(g_game_dir), "%s", dir);
}

const char *io_game_dir(void) { return g_game_dir; }

/*
 * Pick somewhere writable, once, and remember it.
 *
 * The candidates are tried in the order a player would want them: an explicit
 * override, then next to the game so saves live with the game they belong to,
 * then the XDG location, then scratch. Each is *tested* by actually creating a
 * file rather than by inspecting permissions - access(2) answers a different
 * question than "can this process make a file here", and a read-only bind mount
 * is exactly the case where the two disagree.
 */
static char g_writable_dir[PATH_MAX];

static bool try_writable(const char *path)
{
    if (!path || !*path)
        return false;

    mkdir(path, 0777);          /* may already exist; the probe below decides */

    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s/." PORT_NAME "-write-test", path);
    int fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    close(fd);
    unlink(probe);

    snprintf(g_writable_dir, sizeof(g_writable_dir), "%s", path);
    return true;
}

const char *io_writable_dir(void)
{
    if (g_writable_dir[0])
        return g_writable_dir;

    char candidate[PATH_MAX];

    if (try_writable(port_getenv("SAVEDIR"))) {
        trace("writable storage: %s (" PORT_ENV_PREFIX "_SAVEDIR)", g_writable_dir);
        return g_writable_dir;
    }

    snprintf(candidate, sizeof(candidate), "%s/saves", g_game_dir);
    if (try_writable(candidate)) {
        trace("writable storage: %s (beside the game)", g_writable_dir);
        return g_writable_dir;
    }

    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        snprintf(candidate, sizeof(candidate), "%s/" PORT_NAME, xdg);
        if (try_writable(candidate)) {
            trace("writable storage: %s (XDG_DATA_HOME)", g_writable_dir);
            return g_writable_dir;
        }
    }

    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(candidate, sizeof(candidate), "%s/.local", home);
        mkdir(candidate, 0777);
        snprintf(candidate, sizeof(candidate), "%s/.local/share", home);
        mkdir(candidate, 0777);
        snprintf(candidate, sizeof(candidate), "%s/.local/share/" PORT_NAME, home);
        if (try_writable(candidate)) {
            trace("writable storage: %s (HOME)", g_writable_dir);
            return g_writable_dir;
        }
    }

    /* Last resort. Saves do not survive a reboot here, which is worse than the
     * alternatives and better than an engine that cannot start. */
    if (try_writable("/tmp/" PORT_NAME)) {
        warning("no persistent writable directory found; falling back to %s - "
                "saves will not survive a reboot\n", g_writable_dir);
        return g_writable_dir;
    }

    /* Nothing worked. Returning the game directory keeps every caller's
     * contract - a path, never NULL - and the engine fails its mkdir the way it
     * did before, which is at least the honest outcome. */
    snprintf(g_writable_dir, sizeof(g_writable_dir), "%s", g_game_dir);
    warning("no writable directory anywhere; falling back to the game "
            "directory, which is where this failed before\n");
    return g_writable_dir;
}

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
static const char *cpuinfo_override(void)
{
    static char path[PATH_MAX];
    static bool resolved = false;

    if (resolved)
        return path[0] ? path : NULL;
    resolved = true;

    char features[8192] = {0};
    FILE *real = fopen("/proc/cpuinfo", "re");
    if (real) {
        size_t n = fread(features, 1, sizeof(features) - 1, real);
        features[n] = '\0';
        fclose(real);
    }

    /* Word-boundary matching: "vfp" must not be satisfied by "vfpv3" alone is
     * not the concern - FMOD accepts either - but "fp" must not be read as
     * "vfp", and an unrelated line containing "neon" should not count. */
    bool has_vfp  = strstr(features, " vfp") || strstr(features, "\tvfp");
    bool has_neon = strstr(features, " neon") || strstr(features, "\tneon");
    if (has_vfp || has_neon) {
        path[0] = '\0';
        return NULL;
    }

    const char *dir = io_writable_dir();
    if (!dir) {
        path[0] = '\0';
        return NULL;
    }
    snprintf(path, sizeof(path), "%s/cpuinfo", dir);

    FILE *out = fopen(path, "we");
    if (!out) {
        path[0] = '\0';
        return NULL;
    }
    /* The minimum FMOD parses: an architecture above 6, a Processor line
     * without the "(v6l)" marker, and the feature tokens. The rest is written
     * because a half-populated cpuinfo is more confusing to read than a
     * complete one. */
    fprintf(out,
            "Processor\t: ARMv7 Processor rev 4 (v7l)\n"
            "processor\t: 0\n"
            "BogoMIPS\t: 48.00\n"
            "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 "
            "idiva idivt\n"
            "CPU implementer\t: 0x41\n"
            "CPU architecture: 7\n"
            "CPU variant\t: 0x0\n"
            "CPU part\t: 0xd03\n"
            "CPU revision\t: 4\n"
            "\n"
            "Hardware\t: Generic ARMv7\n");
    fclose(out);

    warning("cpuinfo: this kernel reports AArch64 feature names, which FMOD "
            "does not recognise as a CPU that can play audio (it answers 48 "
            "and registers no output at all). Serving an ARMv7 /proc/cpuinfo "
            "describing the same capabilities from %s.\n", path);
    return path;
}

static const char *fix_path_inner(const char *orig, char *buf, size_t bufsz)
{
    if (!orig || !*orig)
        return orig;

    /* Rule -1: /proc/cpuinfo, only when the kernel's spelling would cost the
     * guest a capability it has. Generic: see cpuinfo_override(). */
    if (strcmp(orig, "/proc/cpuinfo") == 0) {
        const char *override = cpuinfo_override();
        if (override) {
            snprintf(buf, bufsz, "%s", override);
            return buf;
        }
        return orig;
    }

    /* Rule 0: Android's platform fonts. First because these paths are absolute
     * and would otherwise fall through every rule below unchanged. The port
     * decides which font answers; portbase only knows that something must. */
    if (strstr(orig, "/system/fonts/")) {
        const char *font = port_system_font();
        if (font) {
            snprintf(buf, bufsz, "%s/%s", g_game_dir, font);
            return buf;
        }
    }

    /* Rule 1: whatever this particular game's engine spells differently - its
     * URI scheme, its package directory, its asset layout. */
    const char *ported = port_fix_path(orig, buf, bufsz);
    if (ported)
        return ported;

    /* Rule 2: a bare relative name. The qemu process is launched from the
     * loader tree, not from the mounted game tree, so leaving these relative
     * makes valid donor files look absent. */
    if (orig[0] != '/') {
        snprintf(buf, bufsz, "%s/%s", g_game_dir, orig);
        return buf;
    }

    return orig;
}

const char *fix_path(const char *orig, char *buf, size_t bufsz)
{
    const char *fixed = fix_path_inner(orig, buf, bufsz);

    /* The donor keeps its whole tree at the game-dir root; the engine reaches
     * it through "<gamedir>/assets/". The development tree bridged that with an
     * "assets -> ." symlink, but FAT/exFAT SD cards cannot store symlinks, so
     * on device that bridge silently disappears. When an assets/ spelling does
     * not exist but the same name at the root does, use the root spelling.
     * Genuinely new files (writes) still land under a real assets/ directory
     * that the package ships empty. */
    if (fixed && g_game_dir[0]) {
        size_t root_len = strlen(g_game_dir);
        if (strncmp(fixed, g_game_dir, root_len) == 0 &&
            strncmp(fixed + root_len, "/assets/", 8) == 0 &&
            access(fixed, F_OK) != 0) {
            char flat[PATH_MAX];
            snprintf(flat, sizeof(flat), "%s/%s", g_game_dir,
                     fixed + root_len + 8);
            if (access(flat, F_OK) == 0) {
                snprintf(buf, bufsz, "%s", flat);
                return buf;
            }
        }
    }
    return fixed;
}

/*
 * Rate-limited so a game that probes for a hundred optional files does not
 * bury the rest of the log, but not silent: the first translations are how the
 * next reader learns which names the engine actually asks for. Everything past
 * the cap is still counted in the summary line.
 */
static void trace_path(const char *what, const char *orig, const char *fixed, int rc)
{
    static int shown = 0;
    if (fixed == orig || shown >= 24)
        return;
    shown++;
    trace("io: %s '%s' -> '%s' (%s)", what, orig, fixed,
          rc >= 0 ? "ok" : "ENOENT");
}

/*
 * Count successful content-file opens, not probes, directories, saves or
 * configuration. The path has already passed through fix_path(), so the
 * extracted asset tree is the stable boundary regardless of which spelling
 * the engine used (appbundle:, Android/data/... or its VFS mount).
 */
static void count_asset_open(const char *fixed, bool succeeded)
{
    if (succeeded && fixed && strstr(fixed, "/assets/published/"))
        g_assets_opened.fetch_add(1, std::memory_order_relaxed);
}

/* Declared in thunks/libc/generated/impl_header.h, which pulls in the whole
 * generated prototype set; only this one is needed, so it is re-declared with
 * matching C++ linkage instead. */
extern ABI_ATTR void *fopen_impl(const char *path, const char *mode);

extern "C" {

/*
 * open() is variadic in the host header but not in what the game emits: it
 * passes two registers for a read and three for a create. Declaring the third
 * unconditionally is safe on AAPCS - r2 is caller-saved scratch either way -
 * and avoids a va_list in a function the allocator can reach.
 */
int bionic_open(const char *path, int flags, mode_t mode)
{
    char buf[PATH_MAX];
    const char *fixed = fix_path(path, buf, sizeof(buf));
    if (port_getenv("DISABLE_PROGRAM_BINARIES") && fixed &&
        strstr(fixed, "/Shaders/bin/") && strstr(fixed, ".bin")) {
        errno = ENOENT;
        trace_path("open(shader-bin-disabled)", path, fixed, -1);
        return -1;
    }
    int fd = open(fixed, flags, mode);
    if (fd >= 0 && strstr(fixed, "/CC_SeedData.bin"))
    {
        g_seed_fd = fd;
        trace("seed open via open: fd=%d", fd);
    }
    count_asset_open(fixed, fd >= 0);
    trace_path("open", path, fixed, fd);
    return fd;
}

int bionic_openat(int dirfd, const char *path, int flags, mode_t mode)
{
    char buf[PATH_MAX];
    const char *fixed = fix_path(path, buf, sizeof(buf));
    int fd = openat(dirfd, fixed, flags, mode);
    if (fd >= 0 && strstr(fixed, "/CC_SeedData.bin"))
    {
        g_seed_fd = fd;
        trace("seed open via openat: fd=%d", fd);
    }
    count_asset_open(fixed, fd >= 0);
    trace_path("openat", path, fixed, fd);
    return fd;
}

int bionic_open64(const char *path, int flags, mode_t mode)
{
    return bionic_open(path, flags, mode);
}

int bionic_openat64(int dirfd, const char *path, int flags, mode_t mode)
{
    return bionic_openat(dirfd, path, flags, mode);
}

int bionic_open_2(const char *path, int flags)
{
    return bionic_open(path, flags, 0);
}

int bionic_openat_2(int dirfd, const char *path, int flags)
{
    return bionic_openat(dirfd, path, flags, 0);
}

/* The generated libc table does not include read(2) on this ABI. Bind it
 * explicitly; without this entry the seed parser can fall through to a bad
 * PLT target and read stdin (fd 0) after successfully opening the seed file. */
ssize_t bionic_read(int fd, void *buf, size_t count)
{
    /* The Android seed reader loses the returned descriptor through its
     * bionic FILE shim and calls read(0, ..., 1023). Recover the descriptor
     * captured by bionic_open; otherwise stdin EOF is reported as corrupt seed
     * data and Cloudcell never initializes its local cache. */
    if (fd == 0 && g_seed_fd >= 0 && count == 1023)
        fd = g_seed_fd;
    ssize_t n = read(fd, buf, count);
    if (fd >= 0 && count > 256)
        trace("io: read fd=%d count=%zu -> %zd", fd, count, n);
    return n;
}

/*
 * Forwarded to the existing thunk rather than calling the host's fopen.
 *
 * fopen_impl() does not return a host FILE*: it wraps one in a BIONIC_FILE, a
 * structure with bionic's field offsets, because the game's inlined getc/putc
 * read _p and _r out of it directly. Re-implementing the open here would hand
 * back a host FILE* that looks fine until the first inlined character read.
 * Only the path is ours to change.
 */
void *bionic_fopen(const char *path, const char *mode)
{
    char buf[PATH_MAX];
    const char *fixed = fix_path(path, buf, sizeof(buf));
    if (port_getenv("DISABLE_PROGRAM_BINARIES") && fixed &&
        strstr(fixed, "/Shaders/bin/") && strstr(fixed, ".bin")) {
        errno = ENOENT;
        trace_path("fopen(shader-bin-disabled)", path, fixed, -1);
        return NULL;
    }
    void *f = fopen_impl(fixed, mode);
    count_asset_open(fixed, f != NULL);
    trace_path("fopen", path, fixed, f ? 0 : -1);
    return f;
}

long android_io_assets_opened(void)
{
    return g_assets_opened.load(std::memory_order_relaxed);
}

void *bionic_opendir(const char *path)
{
    char buf[PATH_MAX];
    const char *fixed = fix_path(path, buf, sizeof(buf));
    DIR *d = opendir(fixed);
    trace_path("opendir", path, fixed, d ? 0 : -1);
    return d;
}

int bionic_mkdir(const char *path, mode_t mode)
{
    char buf[PATH_MAX];
    return mkdir(fix_path(path, buf, sizeof(buf)), mode);
}

int bionic_remove(const char *path)
{
    char buf[PATH_MAX];
    return remove(fix_path(path, buf, sizeof(buf)));
}

int bionic_unlink(const char *path)
{
    char buf[PATH_MAX];
    return unlink(fix_path(path, buf, sizeof(buf)));
}

int bionic_rename(const char *from, const char *to)
{
    char a[PATH_MAX], b[PATH_MAX];
    /* Two buffers: fix_path may return either of them or the original, so they
     * cannot be shared between the two arguments. */
    const char *fa = fix_path(from, a, sizeof(a));
    const char *fb = fix_path(to,   b, sizeof(b));
    return rename(fa, fb);
}

} /* extern "C" */

DynLibFunction symtable_io[] = {
    /* fopen returns a FILE* the game only ever hands back to us, so the
     * bionic/host FILE layout difference does not matter here - unlike the
     * struct stat in symtab_stat.cpp, which the game reads fields out of. */
    THUNK_SPECIFIC("open",    bionic_open),
    THUNK_SPECIFIC("openat",  bionic_openat),
    THUNK_SPECIFIC("open64",  bionic_open64),
    THUNK_SPECIFIC("openat64", bionic_openat64),
    THUNK_SPECIFIC("__open_2", bionic_open_2),
    THUNK_SPECIFIC("__openat_2", bionic_openat_2),
    THUNK_SPECIFIC("read",    bionic_read),
    THUNK_SPECIFIC("fopen",   bionic_fopen),
    THUNK_SPECIFIC("opendir", bionic_opendir),
    THUNK_SPECIFIC("mkdir",   bionic_mkdir),
    THUNK_SPECIFIC("remove",  bionic_remove),
    THUNK_SPECIFIC("unlink",  bionic_unlink),
    THUNK_SPECIFIC("rename",  bionic_rename),
    { NULL, 0 },
};
