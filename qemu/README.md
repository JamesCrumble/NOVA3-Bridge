# qemu-arm for the Android wrapper

The APK runs the ARM32 game under our own static aarch64 `qemu-arm` (linux-user), built from
QEMU 11.1.2 with `nova3-qemu-11.1.2.patch`. The patch, all of it Android/Oryon-specific:

- **Fast host FP**: VFP add/sub/mul/div/fma and float<->int conversions as single AArch64
  instructions (same IEEE and NaN rules as ARMv7); only the cumulative FPSCR flags are not produced.
  `QEMU_STRICT_FP=1` turns it off.
- **Neon f32 on the host FPU** (vadd/vsub/vmul/vmla/vmls, also by scalar) under FPCR FZ+DN, the
  AArch32 "standard FPSCR". The mode is entered lazily and left in place: writing FPCR serialises
  Oryon, and switching per helper call made Neon 6.7x slower.
- **Fast A32 indirect-branch lookup** (`helper_arm_lookup_tb_ptr`, `NOVA3_FAST_LOOKUP`): flags built
  inline, generic path only on a jump-cache miss. Calls/returns 1.7x faster on the bench.
- Jump cache 4096 -> 65536 entries (`NOVA3_JMP_BITS=16`).
- `cpsr_write` does not rebuild hflags for NZCV/Q/GE writes.
- `realpath()` in guest `open` only for paths mentioning "proc" (glibc's realpath reaches the
  faccessat2 syscall, which Android's app seccomp filter kills the process for).
- `QEMU_SYSCALL_STATS=1`: guest syscall counts every 10 s and sampled open/access paths on stderr.

`android/patch_syscalls.py` additionally rewrites set_robust_list/rseq/faccessat2 call sites in the
static glibc for the seccomp filter.

## Build (Docker, from the WSL host)

```sh
docker build -t nova3-qemu-build android/qemu
# qemu source: download.qemu.org/qemu-11.1.2.tar.xz unpacked to <work>/qemu-src/qemu-11.1.2,
# patched with: patch -p1 < android/qemu/nova3-qemu-11.1.2.patch
docker run --rm -u 1000:1000 -e HOME=/tmp -v <work>:/work nova3-qemu-build bash /work/qemu-build/build_variant.sh D \
  '-DNOVA3_JMP_BITS=16 -DNOVA3_FAST_LOOKUP -O3 -march=armv8.2-a+lse+rcpc+dotprod -mtune=neoverse-v1'
```

Do not add SVE to `-march`: Oryon has none (SIGILL).

## Tests

- `fptest.c` (armhf static): hash of VFP/Neon results over denormals, NaNs, infinities; must match with
  and without `QEMU_STRICT_FP=1` and Debian's qemu 7.2.
- `armbench.c`: engine-shaped workloads (calls/vtables, VFP, Neon mat*vec, qsort, memcpy) to compare
  builds on the phone from `/data/local/tmp`.
