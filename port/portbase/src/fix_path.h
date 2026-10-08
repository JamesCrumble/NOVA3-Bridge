#pragma once

#include <stddef.h>

/*
 * Asset path translation.
 *
 * The engine addresses its data with Android-side names that mean nothing on a
 * Linux filesystem. Every libc entry point that takes a path has to run them
 * through fix_path() first; see src/symtab_io.cpp for the rules and for the
 * evidence they were derived from.
 */

/* Called once from main() with argv[1]. */
void io_set_game_dir(const char *dir);

const char *io_game_dir(void);

/*
 * A directory this process can actually write to, created on first use.
 *
 * The game directory is the player's own copy and is frequently read-only - the
 * harness mounts it that way on purpose, and a PortMaster install can sit on a
 * card mounted the same. The engine does not treat that as a soft failure:
 * EASP's SP::Tracking::TrackingImpl builds its module data directory under the
 * storage root, and when the mkdir fails it takes an error path whose own
 * message is malformed ("%s module data directory %s failed to create", second
 * argument 0x1), so the run dies inside the complaint rather than on the thing
 * being complained about.
 *
 * Everything EAMIO's StorageDirectory reports as storage points here. Assets are
 * unaffected: those resolve through appbundle:/ and through
 * com/ea/blast/GetAppDataDirectoryDelegate, which still name the game tree.
 */
const char *io_writable_dir(void);

/*
 * Translates 'orig' into 'buf' and returns 'buf', or returns 'orig' untouched
 * when no rule applies. The caller owns the buffer, which keeps this usable
 * from inside a libc thunk without allocating - the game calls open() from
 * inside its own allocator's core path, where a malloc would recurse.
 */
const char *fix_path(const char *orig, char *buf, size_t bufsz);
