/*
 * 32-bit time_t, because the game has one and the host does not.
 *
 * Debian trixie moved armhf to 64-bit time_t. bionic's armeabi-v7a time_t
 * stayed 32-bit, and Real Racing 3 was compiled against it. Binding the game's
 * clock_gettime straight to the host's - which is what the generated bionic
 * table does, and what works on distros that have not made the switch - hands
 * the kernel a 16-byte struct timespec to fill in where the game reserved 8:
 *
 *     xt::Time::getSeconds():
 *         sub  sp, #16
 *         str  <canary>, [sp, #12]
 *         add  r1, sp, #4        ; struct timespec
 *         blx  clock_gettime
 *
 * The extra eight bytes land on the stack canary and the process dies in
 * __stack_chk_fail before the game has drawn anything. It is not a subtle
 * failure, but it is a silent one: nothing in the log points at time_t.
 *
 * gettimeofday is the same bug with a nastier landing. The caller is a
 * microsecond-clock helper at module+0x237740, and it does not have a canary:
 *
 *     push {r4, lr}          ; [sp+8] = r4, [sp+12] = return address
 *     sub  sp, sp, #8        ; [sp+0..7] = struct timeval, all of it
 *     mov  r0, sp
 *     bl   gettimeofday      ; host writes 16 bytes, not 8
 *     ldm  sp, {r0, r2}      ; tv_sec, tv_usec
 *     ...
 *     pop  {r4, pc}          ; <- pc
 *
 * The host's 64-bit tv_usec is written at [sp+8..15]: its low word lands on
 * the saved r4 and its high word - zero, because microseconds never fill 32
 * bits - lands on the saved return address. So the function computes the right
 * timestamp, stores it correctly, and then returns to address zero.
 *
 * That is what the run log showed before this entry existed:
 *
 *     FATAL: SIGSEGV at 0x00000000
 *            pc = 0x00000000  lr = <libc>  r0 = 0x6a6bab71  r1 = 0x000f4240
 *
 * r0 is a live Unix timestamp and r1 is 1000000, which is the sec-to-usec
 * multiply about to happen two instructions later - the smoking gun, and the
 * reason this is not the missing-JNI-class crash it was mistaken for. The
 * fake-class warnings printed just above it are real but unrelated: the engine
 * survives every one of them, because a NULL jmethodID goes through the JNI
 * shim's null checks and comes back as a zero return value, not a jump.
 *
 * Every function here therefore takes the game's layout, calls the host with
 * the host's, and narrows across the boundary. These entries come before the
 * generated libc table in so_dynamic_libraries so they win the lookup.
 *
 * Narrowing is lossless until 2038; past that the game sees a wrapped clock,
 * exactly as it would on a real armeabi-v7a device.
 */
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/time.h>

#include "port_env.h"
#include "so_util.h"
#include "thunk_gen.h"
#include "time_scale.h"
#include "trace.h"

/*
 * Development-only clock acceleration. The contract, the reasoning and the list
 * of what is deliberately left unscaled are in time_scale.h; this is only where
 * it happens to live, because these are the functions it acts on.
 */

/*
 * The multiplier, parsed once. Capped at 64 because past that the offset grows
 * further between two frames than any engine's interpolation expects, and
 * physics stepped off the clock starts tunnelling through geometry - which
 * reads as a port bug and is not one.
 */
double port_time_scale(void)
{
    static const double scale = [] {
        const char *value = port_getenv("TIME_SCALE");
        if (!value || !*value)
            return 1.0;

        char *end = NULL;
        const double parsed = strtod(value, &end);
        if (end == value || (end && *end != '\0') || !(parsed >= 1.0) ||
            parsed > 64.0) {
            warning("%s=%s is not a scale between 1 and 64; ignoring it.\n",
                    port_env_name("TIME_SCALE"), value);
            return 1.0;
        }
        if (parsed != 1.0)
            warning("time scale %.2fx - development emulator only. The guest's "
                    "monotonic clock and gettimeofday run fast; CLOCK_REALTIME, "
                    "time() and localtime() do not.\n", parsed);
        return parsed;
    }();
    return scale;
}

int64_t port_time_scale_offset_ns(void)
{
    const double scale = port_time_scale();
    if (scale == 1.0)
        return 0;   /* the whole feature, off, before any clock is read */

    /* Anchored on the first call, which is the earliest moment the offset is
     * observable by anyone. Function-local static init is thread-safe, and it
     * needs to be: the render thread and the audio thread do arrive together. */
    static const struct timespec origin = [] {
        struct timespec value = {0, 0};
        clock_gettime(CLOCK_MONOTONIC, &value);
        return value;
    }();

    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t elapsed = (int64_t)(now.tv_sec - origin.tv_sec) * 1000000000LL
                          + (int64_t)(now.tv_nsec - origin.tv_nsec);
    if (elapsed <= 0)
        return 0;
    return (int64_t)((double)elapsed * (scale - 1.0));
}

static void shift_timespec(struct timespec *value, int64_t delta_ns)
{
    if (!value || delta_ns == 0)
        return;

    int64_t total = (int64_t)value->tv_nsec + delta_ns;
    value->tv_sec += (time_t)(total / 1000000000LL);
    total %= 1000000000LL;
    if (total < 0) {
        total += 1000000000LL;
        value->tv_sec -= 1;
    }
    value->tv_nsec = (long)total;
}

void port_time_scale_forward(struct timespec *value)
{
    shift_timespec(value, port_time_scale_offset_ns());
}

/*
 * The offset read here is the one at the moment of the call, not the one in
 * force when the guest computed the deadline. It is very slightly smaller, so
 * the wait can end a hair early - which every caller of a timed wait already
 * handles, because spurious wakeups exist.
 */
void port_time_scale_reverse(struct timespec *value)
{
    shift_timespec(value, -port_time_scale_offset_ns());
}

/*
 * Which clock ids measure elapsed time. CLOCK_REALTIME is a date, and the two
 * CPU-time clocks are not wall time at all - scaling either would be a lie
 * about something nobody asked to go faster.
 */
static bool clock_measures_elapsed_time(int clk_id)
{
    switch (clk_id) {
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
        return true;
    default:
        return false;
    }
}

extern "C" {

typedef int32_t bionic_time_t;

struct bionic_timespec {
    int32_t tv_sec;
    int32_t tv_nsec;
};

/* bionic's armeabi struct timeval: two longs, 8 bytes total. The host's is
 * 16 - __time64_t plus __suseconds64_t - and that difference is not visible
 * at any call site, only in how many bytes the callee writes. */
struct bionic_timeval {
    int32_t tv_sec;
    int32_t tv_usec;
};

struct bionic_timeb {
    int32_t time;
    uint16_t millitm;
    int16_t timezone;
    int16_t dstflag;
    int16_t padding;
};

static_assert(sizeof(bionic_timeb) == 12,
              "armeabi bionic timeb must remain 12 bytes");

/* struct tm is identical on both sides (nine ints, then long + char*), so it
 * crosses unchanged; only the time_t on either end of it needs narrowing. */

int bionic_clock_gettime(int clk_id, struct bionic_timespec *ts)
{
    struct timespec host;
    int rc = clock_gettime(clk_id, &host);
    if (rc == 0 && ts) {
        if (clock_measures_elapsed_time(clk_id))
            port_time_scale_forward(&host);
        ts->tv_sec  = (int32_t)host.tv_sec;
        ts->tv_nsec = (int32_t)host.tv_nsec;
    }
    return rc;
}

/*
 * struct timezone is two ints on both sides, so it is forwarded untouched.
 * The game only ever passes NULL for it (the caller zeroes r1 before the
 * branch that leads here), but forwarding is free and a stub that ignored the
 * argument would be a landmine for whichever call site does use it.
 */
int bionic_gettimeofday(struct bionic_timeval *tv, struct timezone *tz)
{
    struct timeval host;
    int rc = gettimeofday(tv ? &host : NULL, tz);
    if (rc == 0 && tv) {
        /* Scaled, unlike the other realtime paths here: see time_scale.h. */
        struct timespec paced;
        paced.tv_sec  = host.tv_sec;
        paced.tv_nsec = (long)host.tv_usec * 1000;
        port_time_scale_forward(&paced);
        tv->tv_sec  = (int32_t)paced.tv_sec;
        tv->tv_usec = (int32_t)(paced.tv_nsec / 1000);
    }
    return rc;
}

int bionic_ftime(struct bionic_timeb *out)
{
    if (!out)
        return -1;

    struct timeval now;
    if (gettimeofday(&now, NULL) != 0)
        return -1;

    tzset();
    out->time = (int32_t)now.tv_sec;
    out->millitm = (uint16_t)(now.tv_usec / 1000);
    out->timezone = (int16_t)(timezone / 60);
    out->dstflag = (int16_t)(daylight ? 1 : 0);
    out->padding = 0;
    return 0;
}

int bionic_nanosleep(const struct bionic_timespec *req, struct bionic_timespec *rem)
{
    struct timespec host_req, host_rem;
    if (!req)
        return clock_gettime(CLOCK_MONOTONIC, &host_rem); /* propagate EFAULT */

    host_req.tv_sec  = req->tv_sec;
    host_req.tv_nsec = req->tv_nsec;

    /* A sleep is a duration, not an instant, so it divides rather than shifts:
     * the guest asked to be woken after N of *its* nanoseconds. */
    const double scale = port_time_scale();
    if (scale > 1.0) {
        const int64_t total = (int64_t)((double)((int64_t)host_req.tv_sec * 1000000000LL
                                                 + host_req.tv_nsec) / scale);
        host_req.tv_sec  = (time_t)(total / 1000000000LL);
        host_req.tv_nsec = (long)(total % 1000000000LL);
    }

    int rc = nanosleep(&host_req, &host_rem);
    if (rem) {
        rem->tv_sec  = (int32_t)host_rem.tv_sec;
        rem->tv_nsec = (int32_t)host_rem.tv_nsec;
    }
    return rc;
}

bionic_time_t bionic_time(bionic_time_t *out)
{
    bionic_time_t now = (bionic_time_t)time(NULL);
    if (out)
        *out = now;
    return now;
}

/*
 * localtime(), which must never hand the engine a NULL.
 *
 * EA::StdC::DateTime::Set does this, with nothing in between:
 *
 *     bl  localtime@plt
 *     mov ip, r0
 *     ldr r0, [ip, #8]        <- SIGSEGV at 0x8, because r0 was NULL
 *
 * and it is reached from Origin::ChainedRequestManager::createSendRequestDelay
 * Timer during EASP's real init. That fault was read as a networking problem
 * for five iterations, because the crash reporter's stack scan kept showing a
 * stale ProtoHttpCreate return address as if it were a live frame. The
 * .ARM.exidx unwinder is what finally named this one.
 *
 * glibc returns NULL only for a value it cannot break down - out of range or
 * overflowing. The environment is not the problem: the build image has
 * /etc/localtime and a full zoneinfo, and native localtime() answers for every
 * ordinary value. So the engine is asking about a time it computed itself, and
 * a delay-timer expiry built from an uninitialised field is the candidate.
 *
 * Whatever the cause upstream, the rule applies: never return NULL
 * to this engine, because it does not check. Three answers, in descending order
 * of honesty, all of them a valid struct tm:
 *
 *   1. local time, which is what was asked for
 *   2. UTC, when the value has no local representation
 *   3. the epoch, when the value is not a time at all
 *
 * The third is a deliberate lie: a wrong timestamp on a delay timer for a
 * server that no longer exists costs nothing, and the alternative is the
 * process dying on an unchecked dereference.
 */
struct tm *bionic_localtime(const bionic_time_t *t)
{
    time_t host = t ? (time_t)*t : 0;

    struct tm *result = localtime(&host);
    if (result)
        return result;

    result = gmtime(&host);
    if (result) {
        trace("localtime(%lld): no local representation, answered in UTC",
              (long long)host);
        return result;
    }

    /* Not a time at all. Static because there is nowhere to allocate from and
     * the caller only reads it. */
    static struct tm epoch;
    static bool      announced = false;
    if (!announced) {
        announced = true;
        warning("localtime(%lld) is not a representable time; answering with the "
                "epoch so the engine's unchecked dereference survives. The value "
                "is the real bug and it is upstream of this shim.\n",
                (long long)host);
    }

    memset(&epoch, 0, sizeof(epoch));
    epoch.tm_mday = 1;   /* 1-based; 0 is not a valid day of month */
    epoch.tm_year = 70;  /* 1970 */
    return &epoch;
}

/*
 * gmtime(), and the reason this file's whole premise nearly shipped with a hole
 * in it.
 *
 * DateTime::Set picks between the two by its TimeFrame argument, and both
 * branches land on the same unchecked dereference:
 *
 *     377b84: beq 377bd4          ; TimeFrame == 1
 *     377b8c: bl  localtime@plt   ; ...else local
 *     377b90: mov ip, r0
 *     377b94: ldr r0, [ip, #8]    <- both paths arrive here
 *     ...
 *     377bd4: bl  gmtime@plt      ; the TimeFrame == 1 branch
 *     377bdc: mov ip, r0
 *     377be0: b   377b94
 *
 * localtime was in the table below. gmtime was not, so it bound straight to the
 * host's - which on Debian trixie armhf takes a 64-bit time_t and reads eight
 * bytes from the four the game wrote at sp+20. The upper word is whatever was
 * on the stack, the value becomes astronomically large, and gmtime answers NULL
 * for a time it cannot represent.
 *
 * So this is not a new class of bug: it is exactly the 32-vs-64-bit time_t
 * problem this file was written for, in the one function that was missed.
 * localtime was narrowed correctly and never faulted; gmtime was not and did.
 * The defensive fallback matters much less than the narrowing.
 */
struct tm *bionic_gmtime(const bionic_time_t *t)
{
    time_t host = t ? (time_t)*t : 0;

    struct tm *result = gmtime(&host);
    if (result)
        return result;

    static struct tm epoch;
    static bool      announced = false;
    if (!announced) {
        announced = true;
        warning("gmtime(%lld) is not a representable time; answering with the "
                "epoch so the engine's unchecked dereference survives.\n",
                (long long)host);
    }

    memset(&epoch, 0, sizeof(epoch));
    epoch.tm_mday = 1;
    epoch.tm_year = 70;
    return &epoch;
}

bionic_time_t bionic_mktime(struct tm *tm)
{
    return (bionic_time_t)mktime(tm);
}

double bionic_difftime(bionic_time_t end, bionic_time_t start)
{
    return difftime((time_t)end, (time_t)start);
}

} /* extern "C" */

DynLibFunction symtable_time[] = {
    THUNK_SPECIFIC("clock_gettime", bionic_clock_gettime),
    THUNK_SPECIFIC("gettimeofday",  bionic_gettimeofday),
    THUNK_SPECIFIC("ftime",         bionic_ftime),
    THUNK_SPECIFIC("nanosleep",     bionic_nanosleep),
    THUNK_SPECIFIC("time",          bionic_time),
    THUNK_SPECIFIC("localtime",     bionic_localtime),
    THUNK_SPECIFIC("gmtime",        bionic_gmtime),
    THUNK_SPECIFIC("mktime",        bionic_mktime),
    /* returns a double: the only one here that needs the softfp bridge. */
    THUNK_SPECIFIC("difftime",      bionic_difftime),
    { NULL, 0 },
};
