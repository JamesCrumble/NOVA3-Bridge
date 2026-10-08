#include <limits.h>
#include <zlib.h>

#include "fix_path.h"
#include "so_util.h"
#include "thunk_gen.h"

extern "C" gzFile portbase_gzopen(const char *path, const char *mode)
{
    char resolved[PATH_MAX];
    return gzopen(fix_path(path, resolved, sizeof(resolved)), mode);
}

DynLibFunction symtable_zlib[] = {
    NO_THUNK("gzopen",  (uintptr_t)&portbase_gzopen),
    NO_THUNK("gzgets",  (uintptr_t)&gzgets),
    NO_THUNK("gzclose", (uintptr_t)&gzclose),
    { NULL, 0 },
};
