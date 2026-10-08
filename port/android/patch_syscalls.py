#!/usr/bin/env python3
"""Make a static aarch64 glibc binary survive Android's app seccomp filter.

Android kills (SIGSYS) any app-spawned process that makes a syscall outside bionic's
allowlist. glibc's startup makes two of them: set_robust_list(99) and rseq(293).
Both are optional for glibc, so each `mov x8, #nr; ... svc #0` site is rewritten:
  set_robust_list -> getpid (172): harmless, result unused
  rseq            -> close  (57):  fails with EBADF, glibc then runs without rseq

  patch_syscalls.py IN OUT
"""
import struct
import sys

REWRITE = {99: 172, 293: 57, 439: 48}
# faccessat2 (439) is outside the allowlist too; glibc reaches it from faccessat() with flags
# (realpath's "dir/.." check uses AT_EACCESS). faccessat (48) takes the same first three
# arguments and ignores the flags, which is a fine answer for an existence check.
OPTIONAL = {439}
SVC0 = 0xD4000001
MOVZ_X8 = 0xD2800008  # movz x8, #imm16 (imm in bits 5..20)
LOOKBACK = 8


def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = bytearray(open(src, "rb").read())
    count = len(data) // 4
    words = struct.unpack("<%dI" % count, bytes(data[: count * 4]))
    patched = {}
    for i, w in enumerate(words):
        if w != SVC0:
            continue
        for j in range(i - 1, max(i - LOOKBACK, 0), -1):
            v = words[j]
            if v & 0xFFE0001F == MOVZ_X8:
                nr = (v >> 5) & 0xFFFF
                if nr in REWRITE:
                    struct.pack_into("<I", data, j * 4, MOVZ_X8 | (REWRITE[nr] << 5))
                    patched.setdefault(nr, []).append(j * 4)
                break
    for nr in REWRITE:
        if nr not in patched:
            if nr in OPTIONAL:
                continue
            sys.exit("no syscall %d site found; refusing to write %s" % (nr, dst))
        print("syscall %d -> %d at %s" % (nr, REWRITE[nr], " ".join("0x%x" % o for o in patched[nr])))
    with open(dst, "wb") as f:
        f.write(data)


main()
