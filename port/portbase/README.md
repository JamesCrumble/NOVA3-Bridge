# portbase

Scaffold for running a native Android game on a Linux/ARM handheld: a bionic
ELF loader plus the pieces of Android the guest library expects to find. No
emulator and no Android runtime.

It is the shared base extracted from five ports (Minigore 2, Ice Rage, Dead
Space, Mass Effect Infiltrator, Real Racing 3) after each one had been built by
copying the previous one. That method worked and does not scale: by the fifth
port the tree carried eighteen `.cpp` files inherited from games it had nothing
to do with, a comment claiming sources were collected by wildcard next to an
explicit source list that contradicted it, and one game's package name wired
into the generic `open()` path.

## What it gives you

```
loader/     vendored bionic ELF loader (ARM32)
thunks/     libc / libm / zlib / GLES tables
android/    EGL shim, asset manager, OpenSL ES, logging, emulator channel
jni/        fake JVM and the three classes that belong to no game
src/        symbol tables, path translation, crash reporter, GL census
tools/      library collection, glibc floor gate, packaging, eapx
template/   what to copy into a new port
docs/       the port contract
```

## What it asks of you

Ten symbols. See [docs/PORT-CONTRACT.md](docs/PORT-CONTRACT.md) — the linker
enforces the list, so there is no way to half-implement it and find out later.

If you are an agent working on a port, start with [AGENTS.md](AGENTS.md): what
you may touch, how to tell whether you broke something, and the mistakes this
project has already paid for.

## Two rules that are load-bearing

**If a `.cpp` is in the tree, it is compiled.** Wildcards, no exceptions. This
is not style: the previous scaffold shipped an audio pump that was present,
complete, commented and absent from the build. The game ran silent for a day
with nothing in the log, because a file that is not compiled cannot report that
it is not compiled. A file you do not want in the binary does not belong in the
tree — delete it, git remembers.

**Every runtime switch goes through `port_getenv()`.** The prefix lives in one
place (`ENV_PREFIX` in the Makefile). Each port used to name its ~40 variables
after its own game, so adapting the scaffold meant renaming strings by hand
across seventy files; the ones that got missed kept the old prefix and became
switches that silently never fire.

## Build

```bash
docker build -f Dockerfile.build -t portbase-build .
docker run --rm -v "$PWD":/src -w /src portbase-build make -j8
```

Building portbase alone compiles every object and fails at the link with
exactly the ten contract symbols. That is the expected result, and it is the
scaffold's own smoke test.

The base image is deliberately old (`debian:bullseye`, glibc 2.31). glibc is
forward-compatible and these devices run firmwares as old as 2.28–2.31, so a
binary built against a modern toolchain dies on arrival with
`glibc_2.34 not found`. `tools/check_glibc_floor.sh` fails the build if the
loader or any bundled library asks for more than the floor.
