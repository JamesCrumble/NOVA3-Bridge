# Makefile for a portbase port. Copy this to your port as `Makefile`.
#
# Everything here is the port's identity and its own sources. The build rules,
# the loader, the thunks and the JNI shim come from portbase — do not copy
# them in, or the next portbase fix will not reach this port.

PORT_NAME  := nova3
ENV_PREFIX := NOVA3

# Where portbase lives. A submodule at ./portbase is the arrangement that keeps
# the base pinned to a known commit per port, so a fix landing in portbase is
# something you take deliberately rather than something that happens to you.
PORTBASE ?= portbase

# The port's own code: the entry point, the JNI classes this game asks for, its
# engine patches. Nothing generic goes here — if you find yourself writing
# something a second game would want, it belongs in portbase.
PORT_SRCS := $(wildcard game/*.cpp) $(wildcard game/jni/*.cpp)

CPPFLAGS += -Igame

include $(PORTBASE)/Makefile

# The redistributable dependency closure, with per-library licences and the
# glibc floor gate.
libs: $(TARGET)
	tools/collect_libs.sh $(TARGET) build/libs.armhf
	tools/check_glibc_floor.sh $(TARGET) build/libs.armhf

.PHONY: libs
