#ifndef PORTBASE_IO_PATHS_H
#define PORTBASE_IO_PATHS_H

#include <stddef.h>

/*
 * Path translation, split between what every port needs and what only one game
 * knows.
 *
 * The guest opens files by names that made sense inside an Android package:
 * absolute paths into /data, a vendor URI scheme, platform fonts under
 * /system/fonts. None of those exist here, and the engine does not check - it
 * takes the NULL and dereferences it a few frames later, so a path that was
 * never translated shows up as a crash somewhere unrelated.
 *
 * portbase owns the rules that are the same for every game:
 *
 *   - /proc/cpuinfo, when the host kernel's spelling would cost the guest a
 *     capability it actually has (see the note in symtab_io.cpp - this is what
 *     made an emulator unable to produce audio at all);
 *   - a bare relative name, resolved against the game directory, because the
 *     process is launched from the loader tree and not from the game tree;
 *   - "<gamedir>/assets/<x>" falling back to "<gamedir>/<x>", because the
 *     development tree bridges those with a symlink and exFAT cannot store
 *     symlinks, so on a real SD card that bridge silently disappears.
 *
 * Everything else is the port's, through the two hooks below. They are the
 * reason this file exists: the previous scaffold hard-coded one game's package
 * name, URI scheme and asset layout in the middle of the generic open() path,
 * so the next port inherited translation rules for a game it was not.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Translate one guest path. Implemented by the port.
 *
 * Return `buf` after writing the translation into it, or NULL to say "I have
 * no rule for this name" - portbase then applies its generic rules. Returning
 * `orig` unchanged is not the same thing: it claims the name is already right
 * and skips the generic fallbacks.
 *
 * io_game_dir() gives the root of the player's tree.
 */
const char *port_fix_path(const char *orig, char *buf, size_t bufsz);

/*
 * What to serve when the guest opens something under /system/fonts.
 *
 * Engines commonly hard-code an Android platform font ("/system/fonts/
 * DroidSans.ttf"). There is no such file here, FreeType returns a null face,
 * and the crash lands two calls later inside FT_Request_Size - which reads as
 * a FreeType bug rather than as a font that was never opened.
 *
 * Return a path relative to the game directory (a font the player's own tree
 * already contains, so nothing has to be bundled or redistributed), or NULL if
 * this game never asks.
 */
const char *port_system_font(void);

/* The player's game tree, and a directory that is writable on this device. */
const char *io_game_dir(void);
const char *io_writable_dir(void);

#ifdef __cplusplus
}
#endif

#endif
