/*
 * bionic's libstdc++.so - the four allocation operators, and nothing else.
 *
 * Android's libstdc++.so is not GNU's. It is a stub the NDK ships whose entire
 * public surface is operator new, operator new[], operator delete and
 * operator delete[]; everything else a C++ program needs (the STL, RTTI, the
 * exception runtime) is statically linked into the game. So a game either has
 * no mangled C++ import at all - which is what every earlier port here looked
 * like, and why symtab.cpp used to say libstdc++ was "listed but unused" - or
 * it imports exactly these four.
 *
 * The failure without them is not subtle but it is misattributed: the loader
 * reports four unresolved symbols with names like _Znwj, which read as
 * something exotic, and the game cannot allocate a single object.
 *
 * Names, from the Itanium C++ ABI mangling:
 *
 *   _Znwj   operator new(unsigned int)
 *   _Znaj   operator new[](unsigned int)
 *   _ZdlPv  operator delete(void *)
 *   _ZdaPv  operator delete[](void *)
 *
 * The 'j' is `unsigned int`, which is what size_t mangles to on a 32-bit ARM
 * target. A 64-bit build would spell the same operators _Znwm/_Znam, so a
 * table entry copied from an x86-64 reference matches nothing here.
 *
 * ---------------------------------------------------------------------------
 * Why these forward to the host's operators rather than to malloc/free
 *
 * Both would work for a program that only ever pairs new with delete. They
 * stop being interchangeable the moment memory crosses the boundary in the
 * other direction: this loader's own C++ code allocates with the host's
 * operator new, and anything the game frees on our behalf - or we free on the
 * game's - has to reach the same allocator. Going through the operators keeps
 * one allocator on both sides of the boundary, and it keeps whatever
 * new_handler the host runtime has installed.
 *
 * No float crosses any of these boundaries, so no ABI bridge is generated:
 * they resolve to direct pointers.
 */

#include <new>
#include <stddef.h>
#include <stdint.h>

#include "so_util.h"
#include "thunk_gen.h"

/*
 * bionic's operators are the non-throwing kind in practice: the NDK stub
 * returns NULL rather than raising std::bad_alloc, and 2011-era engines test
 * the result. Requesting nothrow here keeps that contract - a game that checks
 * for NULL gets its NULL instead of an exception unwinding through frames the
 * loader compiled with a different runtime.
 */
extern "C" void *bionic_operator_new(size_t size)
{
    return ::operator new(size, std::nothrow);
}

extern "C" void *bionic_operator_new_array(size_t size)
{
    return ::operator new[](size, std::nothrow);
}

extern "C" void bionic_operator_delete(void *p)
{
    ::operator delete(p);
}

extern "C" void bionic_operator_delete_array(void *p)
{
    ::operator delete[](p);
}

DynLibFunction symtable_cxx[] = {
    NO_THUNK("_Znwj",  (uintptr_t)&bionic_operator_new),
    NO_THUNK("_Znaj",  (uintptr_t)&bionic_operator_new_array),
    NO_THUNK("_ZdlPv", (uintptr_t)&bionic_operator_delete),
    NO_THUNK("_ZdaPv", (uintptr_t)&bionic_operator_delete_array),

    { NULL, 0 },
};
