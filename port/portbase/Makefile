# portbase — native Android-game loader for Linux/ARM handhelds.
#
# Always cross-compiles to arm-linux-gnueabihf. These devices run an aarch64
# kernel, but the native library the games ship is armeabi-v7a, and the bionic
# ELF loader has to map it into a process of the same word size.
#
# ---------------------------------------------------------------------------
# ONE RULE ABOUT SOURCES: if a .cpp is in the tree, it is compiled.
#
# Not a style preference — a bug this project shipped. The scaffold used to
# carry an explicit source list next to a header comment claiming sources were
# picked up by wildcard. Both statements lived in the same Makefile and only
# one was true, so files inherited from a sibling port sat in the tree looking
# active while the binary never contained them. One of them was the audio pump:
# present, complete, commented, and absent from the build. The game ran silent
# for a day with nothing in the log, because a file that is not compiled cannot
# report that it is not compiled.
#
# So: wildcards below, no exceptions. A file you do not want in the binary does
# not belong in the tree — delete it. `git` remembers it; the build must not.
# ---------------------------------------------------------------------------

# The port's identity. Override these in the including Makefile (see
# template/Makefile.port) — nothing else in this file names a game.
PORT_NAME   ?= portbase
ENV_PREFIX  ?= PORT

# Where this file's own sources are, relative to the directory make runs in.
#
# Every path below used to be bare ("src/*.cpp", "-Iandroid"), which is right
# for exactly one caller: make invoked from inside portbase. The arrangement
# docs/PORT-CONTRACT.md prescribes is the other one — the port on top, portbase
# as a submodule underneath — and there those wildcards match nothing and the
# includes name directories the port does not have.
#
# It does not fail in a way that says so. The wildcards expand to empty, the
# build compiles the port's own handful of files, and the link then reports the
# entire loader as undefined symbols — which reads as a port that implements
# nothing rather than as a base that was never compiled. The first port to use
# the submodule layout hit exactly that.
#
# The including Makefile sets this (template/Makefile.port: PORTBASE ?=
# portbase); the ?= keeps a standalone `make` in this directory working, which
# is the scaffold's own smoke test.
PORTBASE ?= .

ARCH       ?= arm-linux-gnueabihf
CROSS      ?= $(ARCH)-
CXX        := $(CROSS)g++
PKG_CONFIG ?= $(CROSS)pkg-config

TARGET  := build/$(PORT_NAME)
OBJDIR  := build/obj

PKGS := sdl2 zlib libzip

OPT  ?= -O2 -g
WARN := -Wall -Wextra -Wno-unused-parameter -Werror=return-type \
        -Winit-self -Werror=init-self -Werror=uninitialized

# thunks/libc/generated holds the bionic export tables produced by
# tools/generate_libc.sh. They are checked in because the build image has no
# clang; see that script for why.
INCLUDES := -I$(PORTBASE) -I$(PORTBASE)/src -I$(PORTBASE)/android \
            -I$(PORTBASE)/loader -I$(PORTBASE)/thunks -I$(PORTBASE)/thunks/libc \
            -I$(PORTBASE)/thunks/libc/generated -I$(PORTBASE)/jni
# Appended, not assigned. template/Makefile.port adds "-Igame" before including
# this file, which a := here silently discarded - the port's own headers then
# stopped being findable from its own sources, and the error named the header
# rather than the flag that was dropped.
CPPFLAGS += $(INCLUDES) $(shell $(PKG_CONFIG) --cflags $(PKGS)) -MMD -MP \
            -DPORT_ENV_PREFIX=\"$(ENV_PREFIX)\" -DPORT_NAME=\"$(PORT_NAME)\"
# gnu++20: the vendored ELF loader uses std::string::starts_with.
CXXFLAGS := -std=gnu++20 $(OPT) $(WARN) -fno-strict-aliasing -fuse-cxa-atexit
LDFLAGS  := $(OPT)
LDLIBS   := $(shell $(PKG_CONFIG) --libs $(PKGS)) -pthread -lm -ldl -lrt -lbsd

# Every directory that holds loader sources, swept whole. A port adds its own
# game-specific directory by appending to PORT_SRCS in template/Makefile.port.
BASE_SRCS := $(wildcard $(PORTBASE)/src/*.cpp) \
             $(wildcard $(PORTBASE)/android/*.cpp) \
             $(wildcard $(PORTBASE)/loader/*.cpp) \
             $(wildcard $(PORTBASE)/thunks/libc/*.cpp) \
             $(wildcard $(PORTBASE)/thunks/libm/*.cpp) \
             $(wildcard $(PORTBASE)/thunks/khronos/*.cpp) \
             $(wildcard $(PORTBASE)/jni/*.cpp) \
             $(wildcard $(PORTBASE)/jni/classes/*.cpp) \
             $(wildcard $(PORTBASE)/third_party/*/*.cpp)

SRCS := $(BASE_SRCS) $(PORT_SRCS)
OBJS := $(patsubst %.cpp,$(OBJDIR)/%.cpp.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all clean manifest
all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

# What the build actually picked up. The audio bug that ran a game silent for a
# day would have been a one-line diff against this file.
#
# It deliberately does NOT depend on $(TARGET). portbase compiles and does not
# link — that is its smoke test — so hanging the manifest off the binary makes
# it unavailable in exactly the tree where "is this file compiled?" is hardest
# to answer by eye. This target only needs the source list, which make already
# has before a single object is built.
manifest:
	@mkdir -p build
	@printf '%s\n' $(SRCS) | sort > build/sources.txt
	@echo "$(words $(SRCS)) sources -> build/sources.txt"

clean:
	rm -rf build

-include $(DEPS)
