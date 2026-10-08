# The port contract

portbase compiles on its own and does not link on its own. That is deliberate:
the symbols it leaves undefined are exactly the things only the game knows, and
the linker is what enforces the list. If you can build `portbase` and the only
errors are these ten names, the scaffold is intact.

```
$ make
undefined reference to `main'
undefined reference to `so_after_relocate'
undefined reference to `port_guest_module()'
undefined reference to `port_fix_path'
undefined reference to `port_system_font'
undefined reference to `android_input_cursor_position'
undefined reference to `android_input_cursor_press(bool)'
undefined reference to `android_input_cursor_set(float, float)'
undefined reference to `android_input_inject_control(char const*, bool)'
undefined reference to `android_input_inject_stick(char const*, float, float)'
```

Ten symbols. Everything else — the bionic ELF loader, the libc/libm/zlib/GLES
thunks, the fake JVM, the EGL shim, the asset manager, the OpenSL ES backend,
the crash reporter, the emulator control channel — comes from portbase and
should not be copied into a port. Copying it in is how the previous scaffold
accumulated: a fix landing in the base then had to be applied five times, and
in practice was applied once.

## The ten

### `main` and `so_after_relocate`

Your entry point, and the hook the loader calls once the guest library is
mapped and relocated but before anything of it runs. `so_after_relocate` is
where engine patches go — it is the only moment where the addresses are final
and no guest code has executed yet.

### `port_guest_module()`

Returns the `so_module *` for the loaded guest library. portbase needs it to
walk the guest's exports (the pthread thunks resolve callbacks through it).
Declared with C++ linkage on purpose; see `src/symtab_pthread.cpp`.

### `port_fix_path` and `port_system_font`

Path translation for the names only this engine uses — its URI scheme, its
package directory, its asset layout — and which font answers when the guest
opens something under `/system/fonts`. Full rationale in `src/io_paths.h`;
`template/game/io_paths.cpp` is a worked example carrying every rule shape a
port has needed so far.

Return `NULL` from `port_fix_path` to mean "no rule for this name". Returning
`orig` unchanged is a *different* answer — it claims the path is already
correct and skips portbase's generic fallbacks.

### The five `android_input_*`

The input bridge. portbase declares the interface (`android/input_bridge.h`)
and never implements it, because how SDL events become engine input is the
single most game-specific thing in a port: some engines take
`AInputQueue`/`ALooper`, some export `Java_*` JNI entry points that have to be
called directly, and the button ordinals come out of the game's own DEX.

Two of these are worth knowing about even before you have input working:
`android_input_inject_control` and `android_input_inject_stick` are what
the emulator harness drives the game through. Wire them early — an emulator that can
press buttons is worth several trips to the SD card.

### The exit request the frame loop must consult

Not a symbol you implement — a signal portbase produces that only your loop can
consume. `android_app_request_exit()` (`android/app_exit.h`) is raised whenever
the game ends itself: through `ANativeActivity_finish()`, and in most ports
through the engine's `MainActivity.finish()` as well. It pushes `SDL_QUIT` and
sets a flag; it never tears anything down, because finish() arrives from inside
the engine's own call stack or from its threads.

So the frame loop has to ask, once per iteration, before the draw:

```c
if (android_app_exit_requested())
    goto done;              /* or: running = false; break; */
```

The flag is checked next to the event drain rather than trusted to it: a full
SDL queue drops the `SDL_QUIT`, and a dropped exit request is a freeze — the
engine has released its world and the loader keeps drawing over it, which from
outside is a hang that only PortMaster's kill ends.

## What portbase will not decide for you

The scaffold deliberately has no opinion about:

- **Which JNI classes to fake.** `jni/classes/` here holds only the three that
  are not any game's (`bytebuffer`, `string`, `lang_ClassLoader`). Yours go in
  your own tree. `strings -a lib*.so | grep -E "^(net/|com/|org/)"` tells you
  which, and the count is the best early estimate of how long the port will
  take.
- **Engine patches.** `src/patch.cpp` is the mechanism; the table of what to
  patch is yours.
- **The control scheme.** Read it out of the game's own config, do not invent
  one — every port that invented one got it wrong in a way a player noticed.

## Setting up a port

```
your-port/
├── Makefile           <- copy template/Makefile.port, set PORT_NAME/ENV_PREFIX
├── emulator.json      <- copy template/emulator.json, fill it in
├── portbase/          <- submodule, pinned per port
└── game/
    ├── main.cpp
    ├── io_paths.cpp   <- start from template/game/io_paths.cpp
    ├── input_bridge.cpp
    └── jni/           <- the classes this game asks for
```

`PORT_NAME` and `ENV_PREFIX` in the Makefile must match `binary` and
`env_prefix` in `emulator.json`. If they drift, the emulator harness looks for a binary
that is not there and reports it as a missing game tree — the error names the
donor and the donor is fine.
