/*
 * Fault reporting for the loaded module. See crash.h.
 *
 * The module is mapped at a base the loader chooses at runtime and it is not a
 * file the debugger can open anyway - it lives inside the user's APK. So the
 * one thing worth printing is the faulting PC *relative to the module's text*,
 * which is directly usable:
 *
 *     unzip -p <game>.apk lib/armeabi-v7a/lib<game>.so > /tmp/g.so
 *     arm-linux-gnueabihf-objdump -d --start-address=0x<off-0x20> \
 *         --stop-address=0x<off+0x20> /tmp/g.so
 *
 * Everything below runs inside a signal handler on whichever thread faulted,
 * so it uses write(2) and hand-rolled hex only: no printf, no malloc, no
 * locks. After reporting it restores the default action and re-raises, so the
 * process still dies exactly as it would have.
 */
#include "crash.h"
#include "unwind_arm.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

static uintptr_t   g_text_base = 0;
static size_t      g_text_size = 0;
static uintptr_t   g_exidx_base = 0;
static size_t      g_exidx_size = 0;
static const char *g_soname    = "module";

/*
 * Only the first thread to fault gets to report.
 *
 * This engine runs several threads and they do not fault politely one at a
 * time. When two go down together, both handlers run concurrently and both
 * write to fd 2 a few bytes at a time, and the two reports interleave into
 * something like
 *
 *     r0the guest library = +0x57bc27640x003e7198
 *
 * which is not a corrupted register, it is two lines braided together. That
 * cost an evening: it reads as though the crash handler itself had faulted, so
 * the real report - the only evidence of where the engine died - looked
 * untrustworthy when it was simply mixed with another one.
 *
 * A later thread is not silenced entirely, because "a second thread also
 * faulted" is itself worth knowing; it gets one atomic line and then dies.
 */
static volatile int g_reporting = 0;

static void put(const char *s)
{
    ssize_t n = write(2, s, strlen(s));
    (void)n;
}

static void put_hex(uintptr_t v)
{
    char buf[10] = { '0', 'x' };
    for (int i = 0; i < 8; i++)
        buf[9 - i] = "0123456789abcdef"[(v >> (4 * i)) & 0xf];
    ssize_t n = write(2, buf, sizeof(buf));
    (void)n;
}

/* One register, plus where it lands inside the module if it lands there. */
static void put_reg(const char *name, uintptr_t v)
{
    put("       ");
    put(name);
    put(" = ");
    put_hex(v);
    if (g_text_size && v >= g_text_base && v < g_text_base + g_text_size) {
        put("  (");
        put(g_soname);
        put("+");
        put_hex(v - g_text_base);
        put(")");
    }
    put("\n");
}

static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default:      return "signal";
    }
}

/*
 * A return-address scan of the faulting stack.
 *
 * There is no unwinder here: the module was mapped by our own loader, so it is
 * in no dl_iterate_phdr list and .ARM.exidx never gets registered. Frame
 * pointers are out too - the game is built -fomit-frame-pointer like every
 * NDK release.
 *
 * What is left is the crude method, and for this job it is enough: walk the
 * stack word by word and print everything that points into the module's text.
 * It reports call sites that already returned as well as live frames, so it is
 * a list of suspects rather than a backtrace - but every real frame is in it,
 * and that is what turns "SIGSEGV somewhere in the C++ runtime" into a
 * specific engine function to disassemble.
 *
 * Thumb return addresses have bit 0 set; the offsets are printed with it
 * cleared so they can be pasted straight into objdump --start-address.
 */
/*
 * Is this word plausibly a return address, or just data that happens to fall
 * inside the module?
 *
 * The range check alone is not enough, and the reason is a detail of the
 * loader: text_size is the whole PT_LOAD segment, not just .text, so every
 * pointer-shaped constant in .rodata and .data passes it. A report full of
 * those is worse than no report - one run appeared to show an infinite
 * recursion between two addresses that objdump says are not code at all, and
 * that cost time to disbelieve.
 *
 * A real return address always has a call immediately before it. This game is
 * ARM throughout - no Thumb in the disassembly - so the four forms that can
 * precede one are cheap to recognise:
 *
 *     BL / BLcond      cond 101 1 <imm24>
 *     BLX <imm>        1111 101 <H> <imm24>
 *     BLX <reg>        cond 0001 0010 ... 0011 <Rm>
 *     ldr pc, [...]    the `mov lr, pc ; ldr pc, [r3]` virtual-call pair this
 *                      engine uses everywhere, where lr points just past the
 *                      load
 *
 * False positives are still possible - data can encode a valid BL - but they
 * drop from "most lines" to "rare", which is the difference between a list of
 * suspects and noise.
 */
static bool looks_like_return_address(uintptr_t v)
{
    uint32_t prev = *(const uint32_t *)((v & ~(uintptr_t)1) - 4);

    if ((prev & 0x0F000000) == 0x0B000000) return true;  /* BL / BLcond   */
    if ((prev & 0xFE000000) == 0xFA000000) return true;  /* BLX immediate */
    if ((prev & 0x0FFFFFF0) == 0x012FFF30) return true;  /* BLX register  */
    if ((prev & 0x0E10F000) == 0x0410F000) return true;  /* ldr pc, [..]  */

    return false;
}

static void put_stack(uintptr_t sp)
{
    if (!sp || !g_text_size)
        return;

    put("       stack (module return addresses, innermost first):\n");

    /* Bounded so a corrupted sp cannot walk off the mapping and fault us a
     * second time inside the handler, where the report would be lost. */
    const uintptr_t *w   = (const uintptr_t *)(sp & ~(uintptr_t)3);
    const int        max = 1024;   /* 4 KiB of stack */
    int              hits = 0;

    for (int i = 0; i < max && hits < 24; i++) {
        uintptr_t v = w[i];
        if (v < g_text_base + 4 || v >= g_text_base + g_text_size)
            continue;
        if (!looks_like_return_address(v))
            continue;
        put("         ");
        put(g_soname);
        put("+");
        put_hex((v & ~(uintptr_t)1) - g_text_base);
        put("\n");
        hits++;
    }

    if (!hits)
        put("         (none - the fault is not below any module frame)\n");
}

/*
 * A file descriptor on /dev/null, opened once at init, used to ask the kernel
 * whether an address is readable.
 *
 * write() validates its buffer and returns EFAULT rather than delivering a
 * signal, which is the one way to probe memory from inside a signal handler
 * without risking a second fault. Everything else - reading it directly,
 * installing a temporary handler, msync - either faults or is not
 * async-signal-safe.
 */
static int g_null_fd = -1;

static bool readable(const void *p, size_t len)
{
    if (g_null_fd < 0 || !p)
        return false;
    return write(g_null_fd, p, len) == (ssize_t)len;
}

/*
 * Four words at a pointer, when the pointer is readable.
 *
 * The registers alone say where a fault happened; they do not say what the
 * program thought it had. This port spent hours on a fault whose whole
 * explanation was "the std::vector at r12 has begin == 0" - three words that
 * were sitting in memory the entire time and that no report ever printed.
 */
static void put_words(const char *name, uintptr_t v)
{
    if (!readable((const void *)v, 16))
        return;

    const uintptr_t *w = (const uintptr_t *)v;
    put("       [");
    put(name);
    put("] = ");
    for (int i = 0; i < 4; i++) {
        put_hex(w[i]);
        put(i == 3 ? "\n" : " ");
    }
}

/*
 * The real frame chain, walked from .ARM.exidx.
 *
 * Printed before the suspect scan, and it does not replace it: anything this
 * cannot walk - a function the table marks CANTUNWIND, an opcode form the
 * interpreter refuses to guess at - falls through to the scan, so a fault is
 * never reported with less than it was before this existed.
 *
 * The distinction is the whole point. A frame here is one the unwind tables say
 * is live; a line in the scan below is a word on the stack that looks like a
 * return address, which includes calls that already returned.
 */
static void put_unwind(const mcontext_t *m)
{
    if (!g_exidx_size || !g_text_size) {
        put("       (no unwind table registered for the module)\n");
        return;
    }

    put("       backtrace (unwound, innermost first):\n");

    uint32_t regs[16];
    memset(regs, 0, sizeof(regs));
    regs[0]  = m->arm_r0;  regs[1]  = m->arm_r1;
    regs[2]  = m->arm_r2;  regs[3]  = m->arm_r3;
    regs[4]  = m->arm_r4;  regs[5]  = m->arm_r5;
    regs[6]  = m->arm_r6;  regs[7]  = m->arm_r7;
    regs[8]  = m->arm_r8;  regs[9]  = m->arm_r9;
    regs[10] = m->arm_r10; regs[11] = m->arm_fp;
    regs[12] = m->arm_ip;  regs[13] = m->arm_sp;
    regs[14] = m->arm_lr;  regs[15] = m->arm_pc;

    ArmUnwindContext ctx;
    ctx.exidx_base = g_exidx_base;
    ctx.exidx_size = g_exidx_size;
    ctx.readable   = readable;

    int printed = 0;
    for (int depth = 0; depth < 32; depth++) {
        uintptr_t pc = regs[15] & ~(uintptr_t)1;

        put("         ");
        if (pc >= g_text_base && pc < g_text_base + g_text_size) {
            put(g_soname);
            put("+");
            put_hex(pc - g_text_base);
        } else {
            put("pc ");
            put_hex(pc);
            put(" (outside the module)");
        }
        put("\n");
        printed++;

        if (!arm_unwind_step(&ctx, regs)) {
            put("         (chain ends here - the tables cannot go further)\n");
            break;
        }
    }

    if (!printed)
        put("         (nothing to walk)\n");
}

/*
 * SWP/SWPB emulation.
 *
 * SWP is ARM's original atomic swap, deprecated since ARMv6 and disabled in
 * hardware on this SoC (SCTLR.SW=0), so executing one raises SIGILL. Old NDK
 * builds still carry them - their compilers emitted hand-rolled spinlocks -
 * and qemu emulates SWP silently, so the harness can never see this fault;
 * only real hardware does.
 *
 * Rather than dying, decode the instruction at the faulting pc, perform the
 * swap with a real atomic exchange, step the pc over it and resume. A pc that
 * is not a SWP falls through to the fatal report below, so genuine illegal
 * instructions still die loudly.
 *
 * Encoding:  cond 0001 0B00 Rn Rd 0000 1001 Rm   (B=1 for SWPB)
 * Semantics: Rd = [Rn]; [Rn] = Rm, atomically.
 *
 * r15 in any slot is UNPREDICTABLE and an unaligned word SWP likewise - both
 * are left to die rather than guessed at. The condition field is honoured: a
 * conditional undefined instruction may trap even when its condition fails,
 * in which case the right emulation is to do nothing but still skip it.
 */
static_assert(offsetof(mcontext_t, arm_pc) - offsetof(mcontext_t, arm_r0)
                  == 15 * sizeof(unsigned long),
              "arm_r0..arm_pc must be contiguous for the register file below");

static volatile int g_swp_count = 0;

static bool cond_passed(uint32_t instr, uint32_t cpsr)
{
    const bool n = cpsr & (1u << 31), z = cpsr & (1u << 30),
               c = cpsr & (1u << 29), v = cpsr & (1u << 28);
    switch (instr >> 28) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return c;
    case 0x3: return !c;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return c && !z;
    case 0x9: return !c || z;
    case 0xa: return n == v;
    case 0xb: return n != v;
    case 0xc: return !z && n == v;
    case 0xd: return z || n != v;
    default:  return true;   /* AL; 0xF never encodes SWP */
    }
}

static bool swp_try_emulate(mcontext_t *m)
{
    if (m->arm_cpsr & 0x20)   /* Thumb state: not this instruction */
        return false;

    if (!readable((const void *)m->arm_pc, 4))
        return false;

    const uint32_t instr = *(const uint32_t *)m->arm_pc;
    if ((instr & 0x0FB00FF0) != 0x01000090 || (instr >> 28) == 0xF)
        return false;

    const unsigned rn = (instr >> 16) & 0xF;
    const unsigned rd = (instr >> 12) & 0xF;
    const unsigned rm = instr & 0xF;
    if (rn == 15 || rd == 15 || rm == 15)
        return false;

    unsigned long *regs = &m->arm_r0;

    if (cond_passed(instr, m->arm_cpsr)) {
        void      *addr = (void *)regs[rn];
        const bool byte = instr & (1u << 22);

        if (!byte && ((uintptr_t)addr & 3))
            return false;
        if (!readable(addr, byte ? 1 : 4))
            return false;

        if (byte)
            regs[rd] = __atomic_exchange_n((uint8_t *)addr,
                                           (uint8_t)regs[rm],
                                           __ATOMIC_SEQ_CST);
        else
            regs[rd] = __atomic_exchange_n((uint32_t *)addr,
                                           (uint32_t)regs[rm],
                                           __ATOMIC_SEQ_CST);
    }

    m->arm_pc += 4;

    /* Once, because a spinlock that traps on every acquisition would other-
     * wise flood the log; the count is still worth having for a post-mortem. */
    if (__sync_fetch_and_add(&g_swp_count, 1) == 0) {
        put("\nNOTE: SWP at ");
        put(g_soname);
        put("+");
        put_hex((m->arm_pc - 4) - g_text_base);
        put(" - deprecated atomic, disabled on this SoC; emulating this and "
            "all further ones silently.\n");
    }

    return true;
}

static void on_fault(int sig, siginfo_t *si, void *ucontext)
{
    const ucontext_t *uc = (const ucontext_t *)ucontext;

    /* Unlike every other path in this handler, the SWP path RESUMES the
     * program - so it must leave errno exactly as it found it, or the probe
     * write() above corrupts whatever syscall result the game was inspecting
     * when the signal hit. */
    if (sig == SIGILL && ucontext) {
        const int saved_errno = errno;
        if (swp_try_emulate(&((ucontext_t *)ucontext)->uc_mcontext)) {
            errno = saved_errno;
            return;
        }
        errno = saved_errno;
    }

    /*
     * __sync_lock_test_and_set rather than a mutex: this is a signal handler
     * and pthread_mutex_lock is not async-signal-safe. One atomic word is, and
     * it is all the exclusion needed - the loser never writes again.
     */
    if (__sync_lock_test_and_set(&g_reporting, 1)) {
        put("\nFATAL: a second thread faulted while the first was still "
            "reporting; its details are omitted so the report above stays "
            "readable.\n");
        /*
         * Wait, do not die.
         *
         * The obvious thing here is to restore SIG_DFL and re-raise, and it is
         * wrong: this thread dies, the process goes with it, and the winner is
         * killed mid-sentence. The first attempt at this fix produced
         *
         *     pc = 0x4f66f384  (the guest library
         *
         * - the report truncated exactly where the other thread pulled the
         * floor out. So the loser parks instead and lets the winner finish and
         * take the process down. Bounded, because a winner that never finishes
         * must not turn a crash into a hang: after two seconds this thread
         * gives up and dies itself, which at least preserves the exit status.
         */
        for (int i = 0; i < 200; i++) {
            struct timespec ts = { 0, 10 * 1000 * 1000 };  /* 10 ms */
            nanosleep(&ts, NULL);
        }
        struct sigaction dfl2;
        memset(&dfl2, 0, sizeof(dfl2));
        dfl2.sa_handler = SIG_DFL;
        sigaction(sig, &dfl2, NULL);
        raise(sig);
        return;
    }

    /*
     * Which thread. This engine runs six of them and the fault is on a worker,
     * not on the frame path - a fact that took three sessions to establish
     * because nothing in the report distinguished them. syscall(SYS_gettid) is
     * async-signal-safe; pthread_self is not, and gettid() the glibc wrapper
     * did not exist before 2.30.
     */
    put("\nFATAL: [tid ");
    put_hex((uintptr_t)syscall(SYS_gettid));
    put("] ");
    put(signal_name(sig));
    put(" at ");
    put_hex((uintptr_t)(si ? si->si_addr : 0));
    put("\n");

    if (uc) {
        const mcontext_t *m = &uc->uc_mcontext;
        put_reg("pc", m->arm_pc);
        put_reg("lr", m->arm_lr);
        put_reg("sp", m->arm_sp);
        put_reg("r0", m->arm_r0);
        put_reg("r1", m->arm_r1);
        put_reg("r2", m->arm_r2);
        put_reg("r3", m->arm_r3);
        put_reg("ip", m->arm_ip);

        /* What the registers point at, which is usually the actual answer. */
        put_words("r0", m->arm_r0);
        put_words("r1", m->arm_r1);
        put_words("r2", m->arm_r2);
        put_words("r3", m->arm_r3);
        put_words("ip", m->arm_ip);
        put_words("sp", m->arm_sp);
    }

    if (uc)
        put_unwind(&uc->uc_mcontext);

    put("       module text at ");
    put_hex(g_text_base);
    put("\n");

    if (uc)
        put_stack(uc->uc_mcontext.arm_sp);

    /* Die the way we would have died without this handler. */
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigaction(sig, &dfl, NULL);
    raise(sig);
}

/*
 * The same stack scan, on demand and without a fault.
 *
 * A JNI method this port implements knows what it was asked for and nothing
 * about who asked. That is fine while the engine is making progress and useless
 * the moment it parks: "GetPrimaryExternalStorageDirectory is called once per
 * frame" does not say which of the engine's state machines is doing the
 * calling, and that is the only question worth answering.
 *
 * Reuses put_stack() rather than a second implementation, so a frame listed
 * here means what it means in a crash report: a word on this thread's stack
 * that points just past a call instruction in the module.
 */
extern "C" void crash_report_backtrace(const char *tag)
{
    if (!g_text_size)
        return;

    put("\nTRACE: module frames at ");
    put(tag ? tag : "(here)");
    put(":\n");
    put_stack((uintptr_t)__builtin_frame_address(0));
}

extern "C" void crash_report_init(so_module *mod, const char *soname)
{
    if (mod) {
        g_text_base  = mod->text_base;
        g_text_size  = mod->text_size;
        g_exidx_base = mod->arm_exidx;
        g_exidx_size = mod->arm_exidx_size;
    }
    if (soname)
        g_soname = soname;

    /* O_WRONLY so a probe never actually consumes anything. */
    g_null_fd = open("/dev/null", O_WRONLY);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags     = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);

    /*
     * SIGABRT too. glibc's heap checks ("free(): invalid pointer",
     * "malloc(): corrupted top size") abort() instead of faulting, and without
     * this the process just dies with "uncaught target signal 6" and no clue
     * about which module frame handed the bad pointer to free(). The stack
     * scan below is exactly as useful there as it is for a SIGSEGV.
     */
    sigaction(SIGABRT, &sa, NULL);
}
