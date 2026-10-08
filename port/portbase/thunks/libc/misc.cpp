#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdio.h>
#include <stdarg.h>
#include <link.h>
#include <sys/syscall.h>
#include "platform.h"
#include "so_util.h"
#include "crash.h"
#include "trace.h"
#include "port_fmod_pump.h"

#include "bionic_file.h"
#include "../../src/port_env.h"

extern "C" ABI_ATTR int dl_iterate_phdr_impl(
                 int (*callback)(struct dl_phdr_info *info,
                                 size_t size, void *data),
                 void *data)
{
    // TODO:: Implement a reasonable version of this.
    fatal_error("-- dl_iterate_phdr was called! --\n");
    return -1;
}

extern "C" ABI_ATTR int login_tty_impl(int fd)
{
    return -1;
}

extern "C" ABI_ATTR long syscall_impl(long number, ...)
{
#ifdef gettid
    if (number == 0xb2)
        return gettid();
#else
    if (number == 0xb2)
        return syscall(SYS_gettid);
#endif
    return 0;
}

extern "C" ABI_ATTR void abort_impl(void)
{
    /*
     * The guest reaches here through its own std::terminate, so the only thing
     * printed so far is libstdc++'s "terminate called after throwing ...".
     * That names the exception and nothing else; the module offsets are the
     * part objdump can answer. SIGABRT is already wired to the fault reporter
     * in src/crash.cpp, but this path never raises it - it exits directly - so
     * the stack scan has to be asked for here.
     */
    crash_report_backtrace("guest abort()");
    fatal_error("Guest called abort!\n");
    exit(-1);
}

/*
 * The guest's dlopen. It never maps anything: the handle is a token, and
 * dlsym_impl below answers by name out of the loader's symbol tables no matter
 * which handle it is given. So the only decision here is which libraries this
 * port claims to have - and saying "no" is not neutral, because a guest that
 * asks by dlopen is asking whether a capability exists.
 *
 * Real Racing 3 is the case that proved it. libfmodex.so picks its audio
 * backend with a single probe - dlopen("libOpenSLES.so") - and takes the answer
 * as the verdict: present means the native OpenSL output, absent means the
 * AudioTrack output, which is pumped by a Java thread. A loader with no JVM
 * cannot run that thread, so refusing the name sent FMOD down the one road it
 * could not walk and the game ran to the tutorial in complete silence, with
 * nothing in the log to explain it.
 *
 * Anything named here must actually be backed by a symbol table (see
 * android/opensles.cpp for OpenSL ES), or the guest gets a handle and then a
 * NULL from dlsym, which is a worse lie than the refusal.
 */
extern "C" ABI_ATTR void *dlopen_impl(const char *filename, int flags)
{
    (void)flags;

    if (filename == NULL)
        return NULL;

    char *fn = strdup(filename);
    if (fn == NULL)
        return NULL;

    const char *ex = basename(fn);
    int ret = strncmp(ex, "libEGL", 6) == 0 ||
              strncmp(ex, "libGL", 5) == 0;

    /* This one answer is how FMOD picks its output, so it is also how the port
     * picks: present means OpenSL, absent means the AudioTrack output that
     * src/port_fmod_pump.cpp drives. Refusing on purpose is the only way to
     * reach the second path, since the game never calls setOutput itself. */
    if (!ret && strncmp(ex, "libOpenSLES", 11) == 0)
        ret = !port_fmod_prefers_audiotrack();

    /* Loud, because a dlopen is a capability question and the answer decides
     * which code path the guest takes for the rest of the run. There are only a
     * handful of these in a whole session, and not being able to see them is
     * what made the silent-audio bug take a day: the log could not distinguish
     * "FMOD asked for OpenSL and we refused" from "FMOD never asked". */
    trace("dlopen(\"%s\") -> %s", filename, ret ? "handle" : "NULL");

    free(fn);

    return (ret) ? (void*)0xDEAD : NULL;
}

extern "C" ABI_ATTR char *dlerror_impl(void)
{
    WARN_STUB
    return NULL;
}

extern "C" ABI_ATTR int dlclose_impl(void *handle)
{
    /* ... */
    return 0;
}

extern "C" ABI_ATTR void *dlsym_impl(void *handle, const char *name)
{
    (void)handle;

    void *addr = (void *)so_resolve_link(NULL, name);

    /* Only the misses. A hit is the normal case and the GL tables alone would
     * put thousands of lines in the log, but a miss is a capability the guest
     * asked for and did not get - and it will act on that answer silently. */
    if (addr == NULL)
        trace("dlsym(\"%s\") -> NULL (no symbol table answers to that name)",
              name ? name : "(null)");

    return addr;
}

extern "C" ABI_ATTR const void *
memchr_impl (const void *__s, int __c, size_t __n)
{
  return __builtin_memchr (__s, __c, __n);
}

extern "C" ABI_ATTR int sigsetmask_impl(int mask)
{
    WARN_STUB
    return -1;
}

extern "C" ABI_ATTR char *tempnam_impl(const char *dir, const char *pfx)
{
    WARN_STUB
    return NULL;
}

extern "C" ABI_ATTR char *tmpnam_impl(char *s)
{
    WARN_STUB
    return NULL;
}

extern "C" ABI_ATTR char *mktemp_impl(char *_template)
{
    WARN_STUB
    return NULL;
}

extern "C" ABI_ATTR int* __errno_impl(void)
{
    return __errno_location();
}

extern "C" ABI_ATTR int __android_log_write_impl(int prio, const char *tag, const char *text)
{
    char andlog[2048] = {};
    warning("LOG[%s]: %s\n", tag, text);
    return 1;
}

extern "C" ABI_ATTR int __android_log_print_impl(int prio, const char *tag, const char *fmt, ...)
{
    char andlog[2048] = {};
    va_list va;
    va_start(va, fmt);
    warning("LOG[%s]: ", tag);
    int r = vsnprintf(andlog, 2047, fmt, va);
    warning("%s\n", andlog);
    va_end(va);
    return r;
}

extern "C" ABI_ATTR int __android_log_vprint_impl(int prio, const char *tag, const char *fmt, va_list va)
{
    char andlog[2048] = {};
    warning("LOG[%s]: ", tag);
    int r = vsnprintf(andlog, 2047, fmt, va);
    warning("%s\n", andlog);
    return r;
}

extern "C" ABI_ATTR const char* __strchr_chk(const char* __s, int __ch, size_t __n) { return strchr(__s, __ch); }
extern "C" ABI_ATTR const char* __strrchr_chk(const char* __s, int __ch, size_t __n) { return strrchr(__s, __ch); }
extern "C" ABI_ATTR size_t __strlen_chk(const char* __s, size_t __n) { return strnlen(__s, __n); }

extern "C" ABI_ATTR void android_set_abort_message_impl(const char* msg)
{
    fatal_error("%s", msg);
    abort();
}

extern "C" ABI_ATTR int __system_property_get_impl(const char *name, char *value)
{
    /*
     * The game reads its UI language from Android system properties, not from
     * the SystemAndroidDelegate.GetLanguage JNI (which already returned "en"
     * and was being ignored). As an empty stub, persist.sys.language comes
     * back blank and the engine falls to whatever default its build was made
     * with, which on a repacked regional copy is not English - the whole menu
     * can render in another language even though the asset tree ships ENG_US,
     * SPA_ES and seven more string sets.
     *
     * <PREFIX>_LANG picks the two-letter code (default "en"); "es" gives the
     * Spanish set. Country/region answer to match so a locale-aware path does
     * not disagree with the language. Everything else keeps the empty answer
     * the game treats as "unset".
     */
    if (!name || !value)
        return 0;

    const char *lang = port_getenv("LANG");
    if (!lang || !*lang)
        lang = "en";

    const char *out = NULL;
    if (strstr(name, "language"))
        out = lang;
    else if (strstr(name, "locale"))
        out = (strcmp(lang, "es") == 0) ? "es_ES"
            : (strcmp(lang, "en") == 0) ? "en_US" : lang;
    else if (strstr(name, "country") || strstr(name, "region"))
        out = (strcmp(lang, "es") == 0) ? "ES" : "US";

    if (out) {
        size_t n = strlen(out);
        if (n > 31) n = 31;
        memcpy(value, out, n);
        value[n] = 0;
        return (int)n;
    }

    WARN_STUB;
    value[0] = 0;
    return 0;
}

extern "C" ABI_ATTR int __open_2_impl(const char* pathname, int flags) {
  return open(pathname, flags);
}

// Taken from https://github.com/libhybris/libhybris/blob/master/hybris/common/hooks.c
ABI_ATTR int scandirat_impl(int fd, const char *dir,
                      struct bionic_dirent ***namelist,
                      int (*filter) (const struct bionic_dirent *),
                      int (*compar) (const struct bionic_dirent **,
                                     const struct bionic_dirent **))
{
    struct dirent **namelist_r;
    struct bionic_dirent **result;
    struct bionic_dirent *filter_r;

    int i = 0;
    size_t nItems = 0;

    int res = scandirat(fd, dir, &namelist_r, NULL, NULL);

    if (res > 0 && namelist_r != NULL) {
        result = (bionic_dirent**)malloc(res * sizeof(struct bionic_dirent));
        if (!result)
            return -1;

        for (i = 0; i < res; i++) {
            filter_r = (bionic_dirent*)malloc(sizeof(struct bionic_dirent));
            if (!filter_r) {
                while (i-- > 0)
                    free(result[i]);
                free(result);
                return -1;
            }

            filter_r->d_ino = namelist_r[i]->d_ino;
            filter_r->d_off = namelist_r[i]->d_off;
            filter_r->d_reclen = namelist_r[i]->d_reclen;
            filter_r->d_type = namelist_r[i]->d_type;

            strcpy(filter_r->d_name, namelist_r[i]->d_name);
            filter_r->d_name[sizeof(namelist_r[i]->d_name) - 1] = '\0';

            if (filter != NULL && !(*filter)(filter_r)) {//apply filter
                free(filter_r);
                continue;
            }

            result[nItems++] = filter_r;
        }
        
        if (nItems && compar != NULL) // sort
            qsort(result, nItems, sizeof(struct bionic_dirent *), (__compar_fn_t)compar);

        *namelist = result;
    } else {
        return res;
    }

    return nItems;
}

ABI_ATTR int scandir_impl(const char *dir,
                      struct bionic_dirent ***namelist,
                      int (*filter) (const struct bionic_dirent *),
                      int (*compar) (const struct bionic_dirent **,
                                     const struct bionic_dirent **))
{
    return scandirat_impl(AT_FDCWD, dir, namelist, filter, compar);
}
