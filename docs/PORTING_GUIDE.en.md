# How NOVA3 Bridge was built: a step-by-step account

Русская версия: [PORTING_GUIDE.md](PORTING_GUIDE.md).

N.O.V.A. 3 is the first project of this family. The method itself was worked out here: a 32-bit game runs
under `qemu-user` inside an APK, and its OpenGL ES calls go to the phone's real GPU. So this guide is mostly
not about the game but about three things that the later projects took over unchanged: how to run
`qemu-user` inside an Android app, how the GLES bridge works, and how to find where the time goes.

Every step has the same shape: what was seen, how the cause was found, what was changed and where. The
current state is in [PROJECT_STATE.md](../PROJECT_STATE.md) (Russian), the summary in
[README.md](../README.md), the versions in [CHANGELOG.md](../CHANGELOG.md).

## Starting point

- Game: N.O.V.A. 3 1.0.7, Gameloft's Glitch engine, library `libNOVA3_neon.so`, ARM32 softfp.
- Phone: OnePlus 13s, Snapdragon 8 Elite (Oryon cores). The CPU executes AArch64 only and the firmware has
  no system ARM32 translator (checked: `1554_Tango` in the firmware is a carrier name, not a translator).
- Base: EapRules' existing N.O.V.A. 3 port for PortMaster and its `portbase` — a bionic ELF loader for
  glibc, libc thunks and a fake JNI. That port targets ARM handhelds with 32-bit Linux; here it runs under
  qemu, and everything Android-specific was written on top.

## Part 1. qemu-user inside an Android app

Four Android restrictions, each of which took its own round:

### 1.1. An executable cannot be started from just anywhere

- **Seen:** the system refuses to start qemu from the app's data folder.
- **Cause:** Android allows code to be executed only from the app's native library directory.
- **Fix:** a static AArch64 `qemu-arm` ships in the APK as `libqemu.so` and is started from
  `nativeLibraryDir` with `ProcessBuilder`
  ([GameProcess.java](../port/android/src/com/jamescrumble/nova3/GameProcess.java)).

### 1.2. Seccomp kills the process for "extra" system calls

- **Seen:** the process dies with SIGSYS at start-up or on the first file open.
- **Cause:** the app seccomp filter passes only the system calls on bionic's allow list. The static glibc
  inside qemu calls `set_robust_list`, `rseq` and `faccessat2`. The last one arrives through `realpath()`,
  which qemu calls on every guest `open`.
- **Fix:** [patch_syscalls.py](../port/android/patch_syscalls.py) rewrites the call sites in the built
  binary; the qemu patch keeps `realpath()` only for paths that contain `proc`.

### 1.3. qemu silently refuses to start the guest

- **Seen:** qemu exits without a message.
- **Causes:** the guest program and its `ld.so` were unpacked without the execute bit; qemu prints loader
  errors with `printf` and leaves through `_exit`, so with redirected output they are lost.
- **Fix:** the wrapper sets the execute bit after unpacking the sysroot. For cases like this there is
  `mstrace` (`port/android/tools/mstrace`), which pulls the error text out of the process memory.

### 1.4. A debuggable build slows the GL thread down

- **Seen:** the thread executing GL commands is busy checking rather than working.
- **Cause:** CheckJNI is on in a debuggable APK.
- **Fix:** the manifest has `debuggable=false`, with `profileable shell=true` kept for profiling. The same
  manifest declares `isGame` and a `game_mode_config` that supports performance mode.

## Part 2. The GLES bridge

The game calls GLES 2.0, but there is no driver inside the guest. The calls land in the stubs of
[gl_bridge.cpp](../port/portbase/src/gl_bridge.cpp) (ARM32), are encoded as messages and written to a pipe.
The Java thread [GlBridge.java](../port/android/src/com/jamescrumble/nova3/GlBridge.java) reads them and
executes them through `GLES30` on a `SurfaceView`. Calls that need an answer (`glGet*`, shader status,
`glReadPixels`) use a second pipe.

Decisions made along the way:

- **The guest hands out object names**, the host maps them to real ones. `glGenTextures` then needs no reply.
- **Client vertex arrays are copied into the draw message.** This is the bulk of the data: 2–7 MB per frame.
- **One copy of the game per process**: `singleTask`, full `configChanges`, a static `GameProcess`.
  Otherwise a screen rotation recreated the activity and started a second game.
- **The simple functions are generated**: [gen_glbridge.py](../port/android/tools/gen_glbridge.py) writes
  both sides of the bridge from one table so the op codes cannot drift apart.

## Part 3. The game layer

This part was inherited from the PortMaster port; listed here is what in it is specific to the game.

### 3.1. GDRM protection

- **Seen:** `GL2JNILib.init()` faults on a store through a null pointer (`+0x008d3264`).
- **How it was found:** the pointer is read through the GOT; the slot resolves to the symbol
  `lockPointer4`, with three more next to it.
- **Cause:** on Android the installer activity fills them in after a licence check against a server that
  has been gone for years. The port does not run the installer.
- **Fix:** the four pointers are given storage — [patch.cpp](../port/game/patch.cpp). The game's code is
  not patched.

### 3.2. Data inside OBB files

- **Seen:** `assert false && "could not find sprites pack!"` on the first frame.
- **Cause:** the engine does not read the OBB files. On Android the installer unpacks them into the data
  folder and the game opens ordinary files.
- **Fix:** [obb_cache.cpp](../port/game/obb_cache.cpp) extracts a file from the OBB the first time it is
  asked for. Unpacking everything up front would mean 4 GB written before the title screen.

### 3.3. Call order

`setPaths` must not be called by the port after `init()`: the filesystem object does not exist yet and the
call faults inside `glf::Fs::SetObbDir`. The engine calls `setupPaths` in Java itself when it is ready, and
the fake class answers ([main.cpp](../port/game/main.cpp),
[gl2jni_activity.cpp](../port/game/jni/gl2jni_activity.cpp)).

### 3.4. Quality profile

- **Seen:** the game picks its heaviest graphics profile.
- **Cause:** the profile is chosen by the `GL_RENDERER` string, and an unknown GPU gets the maximum.
- **Fix:** `gpu_name.txt` replaces the GPU name. The profiles live inside encrypted archives, so
  [xml_hook.cpp](../port/game/xml_hook.cpp) hooks XML parsing: it can dump the documents and edit them from
  a list of replacements.
- **What did not work:** the engine's `HardwareSkinning` flag is dead — nothing reads the field. Skinning
  is chosen by material techniques, and overriding it through XML had no effect.

## Part 4. Speed

Starting point: combat ran at 12–16 fps. Result: up to 55, with dips caused by the frequency limiter.

### 4.1. Measure first

The main finding: the bottleneck is **one thread**, the game's main thread inside qemu (85–100 % of a
core). The GPU thread is 15–45 % busy and the GPU is idle. What needs speeding up is emulation and anything
that makes the main thread wait.

Tools built for this:

| What | With |
|---|---|
| fps, GL command count, bridge load, core and frequency | the `[gl] frames` log line every 5 s |
| qemu profile on the phone | `tools/profile.sh` (simpleperf), `tools/qemu_hot.py` |
| profile of the game's code | `QEMU_GUEST_PROF`, `tools/guest_prof.py`, `tools/prof_by_fps.py` |
| guest system calls | `QEMU_SYSCALL_STATS=1` |
| comparing qemu builds | `qemu/armbench.c`, `tools/bench_phone.sh` |
| long frames | the `slow frame` line with counters (`frame_stats.h`) |
| touch latency | the `input:` line every 600 frames |

### 4.2. qemu

The qemu profile showed ~27 % of the time in block lookup on indirect branches, ~25 % in translated code
and ~8 % in NEON. The changes (details in [qemu/README.md](../qemu/README.md)):

| Change | Result |
|---|---|
| VFP and float↔int conversions as one CPU instruction instead of softfloat | the base of the speed-up |
| NEON f32 on the phone's FPU | after the next change |
| lazy FPCR switching | NEON 6.7 times faster: writing FPCR is very expensive on Oryon |
| fast block lookup for calls and returns | 1.7 times on the bench |
| jump cache 4096 → 65536 entries | fewer misses |
| variant F: scalar VFP emitted inline, NEON on a whole register | VFP 200 → 160 ms, NEON 48 → 28 ms |

Accuracy is not judged by eye: the tests `fptest.c`, `cmptest.c`, `neontest.c` and `neoncmptest.c` must
give the same hashes as pure softfloat (`QEMU_STRICT_FP=1`).

Do not add SVE to `-march`: Oryon has none, and the result is SIGILL.

### 4.3. The game's library

- **Seen:** ~13 % of the main thread's time in the `__sync_*` functions.
- **Cause:** the library was built for ARMv5 and calls the kernel's helper at `0xffff0fc0` for its atomic
  operations. qemu-user emulates it by leaving translated code on every call.
- **Fix:** the literal holding that address is pointed at a small `ldrex/strex` routine —
  [tweaks.cpp](../port/game/tweaks.cpp). `NOVA3_FAST_CAS=0` turns it off.

The same file holds physics on its own thread (under qemu every guest thread is a separate host core) and a
custom field of view.

### 4.4. The bridge

| Before | After |
|---|---|
| a mutex on every call | removed: the game's GL is single-threaded |
| repeated `glUseProgram`, `glBindTexture`, identical uniforms | dropped before sending; uniforms per frame 1720 → ~290 |
| dozens of queries about a shader program | one query at link time |
| the vertex array was copied once per attribute | an interleaved array goes as one block |
| indexed draws sent vertices `[0, max]` | the range `[min, max]` is sent, and the bounds are cached |
| `GlBridge.java` read the pipe with two `read()` calls per command | reads in large chunks into a 2 MB buffer |

The last row gave the most. The guest profile showed 43–63 % of the main thread's samples in a heavy scene
at a single address, `svc 0` in libpthread: the game was sitting in `write()` on the pipe. After buffered
reading the share fell to 2–4 %.

Two more sources of needless system calls, found with `QEMU_SYSCALL_STATS`: SDL looked for `libudev.so`
every frame to poll joysticks (~730 `openat` per second) — the joystick subsystem is not initialised in
bridge mode; and the game's log cost three system calls per line — only warnings and errors are kept.

### 4.5. Frames in flight

The guest may run ahead of the host by a set number of frames. A value of 2 with a 1 MB pipe gave a
noticeably steadier frame rate. The price is that the picture lags input by two frames.

### 4.6. CPU frequency — the part that was not won

- **Seen:** with the same work per frame, frame time grows over time from 61 to 84 ms.
- **How it was found:** the core, its current frequency and its ceiling were added to the `[gl] frames`
  line. Temperature ~53 °C, thermal status 0 — this is not overheating.
- **Causes, two of them:** the system cuts the frequency ceiling of the fast cores to 1.2–1.4 GHz out of
  4.32; and on qemu's main thread, and only on it, holds `uclamp.max` at 171–319 out of 1024, so the
  governor treats the thread as lightly loaded.
- **What was tried:** game mode `performance` and allowing background activity help partly. Resetting
  `uclamp` from the guest (`sched_setattr`) returns EPERM. ADPF (`PerfHint.java`) does not affect the clamp
  and is off by default.
- **What helps:** removing the main thread's waits (4.4) and pinning qemu to cores 6–7.

## Part 5. Audio

- **Seen:** audio latency grows to ~0.75 s; a click roughly every 2 seconds.
- **How it was found:** from the log and the disassembly of `vox::DriverAndroid::CallbackOSL`.
- **Causes:** output through SDL slept 23 ms per 23.2 ms period and ran ~1 % faster than real time — the
  surplus piled up in the pipe and the buffer. The engine keeps two 4056-byte blocks queued, while SDL took
  4096 at a time and padded with silence.
- **Fix:** [opensles.cpp](../port/portbase/android/opensles.cpp) writes the game's blocks straight into the
  FIFO, and the phone's speaker sets the pace. The pipe is 4096 bytes and the track buffer is minimal.
  During loading and in the background the track is fed silence rather than dropping data.
- **Check in the log:** `direct out - 100% of real time`, no `speaker ran dry` lines.

## Part 6. Rules that proved useful

1. **A counter first, then the change.** Every speed-up started with a log line that shows the problem as
   a number.
2. **The guest profile matters more than the qemu profile.** "The main thread is sitting in `write()`" is
   invisible in the emulator's profile.
3. **Float accuracy is checked with a bit-exact test**, not with the picture.
4. **Waiting looks like idling to the system.** The more the main thread waits, the lower the core's clock.
5. **Moving an install to another package name** makes it a different app to Android: saves need carrying
   over ([SaveMigration.java](../port/android/src/com/jamescrumble/nova3/SaveMigration.java)).

## What is left

- The splash on the title screen is drawn in the top-left corner; the cause has not been found.
- Above 720p the game's interface breaks.
- A rare hang on the loading screen, seen once and not reproduced.
- Dips when enemies spawn: the game reopens its sound archive for every sound (120–220 opens per second).
  The idea is to keep the `.gla` open in `symtab_io.cpp`; not done.
- The system's frequency limiting (4.6).
