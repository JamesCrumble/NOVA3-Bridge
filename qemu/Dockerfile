# Cross-build environment for a static aarch64 qemu-arm (linux-user) for the Android wrapper.
# Same Debian release as the qemu-user-static that is known to run on the phone (bookworm, glibc 2.36).
FROM debian:bookworm
RUN dpkg --add-architecture arm64 \
 && apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      crossbuild-essential-arm64 build-essential pkgconf ninja-build python3 python3-venv python3-pip \
      make xz-utils bzip2 ca-certificates file binutils-aarch64-linux-gnu \
      libglib2.0-dev:arm64 libpcre2-dev:arm64 zlib1g-dev:arm64 libffi-dev:arm64 \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /work
