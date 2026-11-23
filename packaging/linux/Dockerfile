# UA Reforged - Linux build environment (x86_64). Ubuntu 22.04 = glibc 2.35: the binaries run on that and newer
# (Ubuntu 22.04+, Linux Mint 21+, Debian 12+, Fedora 36+, SteamOS 3.5+). Used by packaging/linux/docker-build.sh.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config nasm curl ca-certificates xz-utils python3 patchelf file \
        squashfs-tools zlib1g-dev \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxinerama-dev \
        libxxf86vm-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl1-mesa-dev \
        libgl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev libdbus-1-dev libudev-dev \
        libasound2-dev libpulse-dev libpipewire-0.3-dev \
    && rm -rf /var/lib/apt/lists/*
# a newer CMake than 22.04's 3.22 (FindLua knows Lua 5.5 from 4.3 on), pinned by SHA-256
RUN curl -fsSL -o /tmp/cmake.tgz https://github.com/Kitware/CMake/releases/download/v4.3.3/cmake-4.3.3-linux-x86_64.tar.gz \
    && echo "927b2368a946c37269c3a66225ab00544e756459cdd0b5d0da438694fb9ff802  /tmp/cmake.tgz" | sha256sum -c - \
    && tar -xzf /tmp/cmake.tgz -C /opt && ln -sf /opt/cmake-4.3.3-linux-x86_64/bin/* /usr/local/bin/ && rm /tmp/cmake.tgz
