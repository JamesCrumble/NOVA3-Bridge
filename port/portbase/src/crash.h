/*
 * Fault reporting for the loaded module.
 *
 * A crash inside the guest library is invisible from the outside: the process
 * has no debugger, the .so is not on disk, and qemu only reports the faulting
 * address. Naming the faulting instruction's offset inside the module is what
 * turns "segfault at 0x4" into something objdump can answer.
 */
#ifndef PORTBASE_CRASH_H
#define PORTBASE_CRASH_H

#include "so_util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the handler and remembers where the module was mapped. */
void crash_report_init(so_module *mod, const char *soname);

/*
 * Prints the module return addresses on the current thread's stack, right now,
 * without a fault. For answering "who is calling this every frame?" from inside
 * a fake JNI method. Meant for a one-shot guard, not for the frame path.
 */
void crash_report_backtrace(const char *tag);

#ifdef __cplusplus
}
#endif

#endif /* PORTBASE_CRASH_H */
