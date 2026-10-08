#include "port_env.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

namespace {

/*
 * Four slots so a single printf with two or three port_env_name() arguments
 * still reads correctly. The alternative - returning malloc'd strings - leaks
 * on every log line, and these are only ever used inside a format call.
 */
constexpr int kSlots = 4;
constexpr int kSlotSize = 96;

const char *build_name(const char *name)
{
    static char slots[kSlots][kSlotSize];
    static int next = 0;

    char *out = slots[next];
    next = (next + 1) % kSlots;

    snprintf(out, kSlotSize, "%s_%s", PORT_ENV_PREFIX, name ? name : "");
    return out;
}

} // namespace

extern "C" const char *port_env_name(const char *name)
{
    return build_name(name);
}

extern "C" const char *port_getenv(const char *name)
{
    if (name == NULL)
        return NULL;
    return getenv(build_name(name));
}

extern "C" int port_getenv_bool(const char *name, int fallback)
{
    const char *v = port_getenv(name);
    if (v == NULL || *v == '\0')
        return fallback;
    /* "0" and "false" are off; anything else the user bothered to type is on.
     * Accepting only "1" caused a real afternoon of confusion when someone set
     * a switch to "true" and it silently stayed off. */
    if (strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 ||
        strcasecmp(v, "off") == 0 || strcasecmp(v, "no") == 0)
        return 0;
    return 1;
}

extern "C" long port_getenv_long(const char *name, long fallback)
{
    const char *v = port_getenv(name);
    if (v == NULL || *v == '\0')
        return fallback;

    char *end = NULL;
    long parsed = strtol(v, &end, 0);
    if (end == v || (end && *end != '\0'))
        return fallback;
    return parsed;
}
