#ifndef PORTBASE_PORT_ENV_H
#define PORTBASE_PORT_ENV_H

/*
 * Every runtime switch in this scaffold is read through port_getenv(), never
 * through getenv() directly.
 *
 * The reason is a bug this codebase kept re-creating. Each port named its
 * variables after its own game - one port's GL_STATS switch was spelled with
 * the game's name baked in, the next port's with a different one - so
 * copying the scaffold to the next game meant renaming ~40 strings by hand
 * across ~70 files. What actually happened is that some got renamed and some
 * did not, and a variable left with the old prefix is invisible: it compiles,
 * it runs, and the switch simply never fires. Nothing reports it.
 *
 * So the prefix lives in exactly one place. Set it in the port's Makefile:
 *
 *     CPPFLAGS += -DPORT_ENV_PREFIX=\"GUNMANCLIVE\"
 *
 * and every switch below becomes GUNMANCLIVE_GL_STATS with no edits. The
 * prefix also keeps the port's variables from colliding with the CFW's and
 * SDL's, which is why they were prefixed in the first place.
 */

#ifndef PORT_ENV_PREFIX
#define PORT_ENV_PREFIX "PORT"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Reads "<PORT_ENV_PREFIX>_<name>". Returns NULL when unset, like getenv. */
const char *port_getenv(const char *name);

/* True when the variable is set to anything other than "0" or the empty
 * string. This is the shape almost every switch wants, and writing it out at
 * each call site is how the port ended up with three different notions of
 * "enabled". */
int port_getenv_bool(const char *name, int fallback);

/* Parsed integer, or the fallback when unset or unparseable. */
long port_getenv_long(const char *name, long fallback);

/* The full name of a variable ("GUNMANCLIVE_GL_STATS"), for log lines that
 * tell the user what to set. Points into a rotating static buffer: print it,
 * do not keep it. */
const char *port_env_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif
