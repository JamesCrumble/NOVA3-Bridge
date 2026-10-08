#ifndef PORTBASE_PATCH_H
#define PORTBASE_PATCH_H

#include "so_util.h"

/*
 * The port's engine patches. Implemented by the port, called by the loader.
 *
 * Must run after so_relocate_all() and before so_initialize(): the module is
 * still writable at that point (so_load_module() calls so_flush_caches(mod, 1)
 * just before the so_after_relocate() hook) and none of the game's own code
 * has executed yet. Patching later means racing the engine for the same bytes.
 *
 * template/game/patch.cpp is a worked example. The offsets in it belong to one
 * build of one game and transfer to nothing — only the shape does.
 */
void so_patch_binary(so_module *mod);

#endif
