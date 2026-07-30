# Builds the Mixxx binary for the deck's Raspberry Pi (arm64).
#
# The Pi runs Mixxx from apt and only /usr/bin/mixxx is swapped out, so this
# binary has to link against exactly the libraries already on the device. Two
# things follow from that:
#
#   - BASE must be the same Debian release the Pi runs. upload.sh reads that off
#     the device and passes it in; a newer release links against a newer glibc
#     and the binary then refuses to start on the Pi.
#   - Anything this build links dynamically has to already be on the Pi. The
#     dependencies below are Mixxx's own Debian build-deps, so that holds by
#     construction; upload.sh re-checks with ldd before installing anyway.
#
# KeyFinder (musical key detection) is left ON, unlike packaging/debian/rules
# which disables it: Debian has no libkeyfinder package at all, so find_package
# fails and Mixxx falls back to fetching libkeyfinder and linking it statically.
# That needs the network and libfftw3-dev at build time, but nothing on the Pi.
#
# INSTALL_USER_UDEV_RULES=OFF mirrors packaging/debian/rules. It only controls
# whether `cmake --install` drops res/linux/mixxx-usb-uaccess.rules (uaccess for
# USB HID/Bulk controllers -- nothing to do with mounting Rekordbox sticks), and
# this build never installs, so it is inert. Kept explicit so a later
# `cmake --install` here cannot quietly start writing udev rules.
#
# Build and deploy with: ./upload.sh

ARG BASE=debian:trixie
FROM ${BASE} AS build

# Mixxx's build dependencies, from packaging/debian/control.in. Only the mixxx
# target is built here, so libbenchmark-dev and xvfb are left out -- but not
# libgtest-dev, despite control.in filing it under "for running mixxx-test":
# production headers such as src/util/fileinfo.h include <gtest/gtest_prod.h>
# for FRIEND_TEST, so the main build needs it too.
#
# Three are not in control.in: ca-certificates, because cmake fetches
# libkeyfinder and libdjinterop over https while Debian's own mirrors are plain
# http; qt6-shadertools-dev, which the Qt6 find_package needs once QML is on;
# and libfftw3-dev, for the statically linked KeyFinder.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build ccache pkg-config git \
        ca-certificates libgtest-dev \
        docbook-to-man markdown \
        libglu1-mesa-dev qt6-shadertools-dev libfftw3-dev \
        qtkeychain-qt6-dev qt6-declarative-private-dev qt6-base-private-dev \
        qml6-module-qt-labs-qmlmodels qml6-module-qtquick-controls \
        qml6-module-qtquick-layouts libqt6core5compat6-dev libqt6opengl6-dev \
        libqt6sql6-sqlite libqt6svg6-dev \
        libjack-dev portaudio19-dev libid3tag0-dev libmad0-dev libogg-dev \
        libsndfile1-dev libasound2-dev libavformat-dev libvorbis-dev \
        libfaad-dev libportmidi-dev libtag1-dev libshout-idjc-dev libssl-dev \
        libprotobuf-dev protobuf-compiler libusb-1.0-0-dev libchromaprint-dev \
        librubberband-dev libopusfile-dev libsqlite3-dev libsoundtouch-dev \
        libhidapi-dev libupower-glib-dev liblilv-dev libmodplug-dev \
        libmp3lame-dev libebur128-dev libwavpack-dev libudev-dev libmsgsl-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . /src

# The build tree and the ccache are cache mounts, so editing a couple of files
# and re-running only recompiles those files instead of all ~1500 of them --
# which is what makes a re-upload quick. Their contents do not survive the RUN,
# so the binary is copied out to /mixxx.
#
# Stripped on the way out: Debian ships a stripped binary too, and it keeps the
# scp to the deck short.
RUN --mount=type=cache,target=/build,sharing=locked \
    --mount=type=cache,target=/ccache,sharing=locked \
    export CCACHE_DIR=/ccache \
    && cmake -S /src -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DINSTALL_USER_UDEV_RULES=OFF \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    && cmake --build /build --target mixxx --parallel "$(nproc)" \
    && strip -o /mixxx /build/mixxx

# Export stage: `--output` copies just the binary out to the host.
FROM scratch AS export
COPY --from=build /mixxx /mixxx

# Unit tests, for the parts of the tree that can be checked without hardware.
#
#   docker build --target unittest --build-arg GTEST_FILTER='ProLinkXdrTest.*' .
#
# Separate stage rather than folded into `build` so a routine binary build for
# the deck does not pay for compiling the test tree, which is large.
#
# QT_QPA_PLATFORM=offscreen because there is no display in the container and
# mixxx-test constructs a QApplication. GTEST_FILTER defaults to the ProLink
# tests: the full suite needs xvfb and audio devices that are not installed
# here, so running all of it would fail for reasons unrelated to the change
# being checked.
FROM build AS unittest
ARG GTEST_FILTER=ProLink*
# Parallelism is capped rather than $(nproc): the test tree is ~700 translation
# units and several of Mixxx's are large enough that a full-width build exhausts
# the Docker VM's memory and the OOM killer takes cc1plus with it. The failure
# looks like "fatal error: Killed signal terminated program cc1plus", which does
# not obviously say "out of memory". Raise it if the VM has plenty of RAM.
ARG BUILD_JOBS=3
# gmock is a separate Debian package from gtest, and mixxx-test links
# GTest::gmock. Installed only in this stage so the image the deck binary is
# built in stays exactly as it was.
RUN apt-get update && apt-get install -y --no-install-recommends libgmock-dev \
    && rm -rf /var/lib/apt/lists/*
# Its own build tree, not /build: the deck build configures with BUILD_TESTING
# off, and flipping that setting back and forth in a shared tree would make
# every alternate build a near-full rebuild. The ccache is shared, so most
# object files are hits anyway.
RUN --mount=type=cache,target=/build-test,sharing=locked \
    --mount=type=cache,target=/ccache,sharing=locked \
    export CCACHE_DIR=/ccache \
    && cmake -S /src -B /build-test -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DBUILD_TESTING=ON \
        -DINSTALL_USER_UDEV_RULES=OFF \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    && cmake --build /build-test --target mixxx-test --parallel "${BUILD_JOBS}" \
    && QT_QPA_PLATFORM=offscreen /build-test/mixxx-test --gtest_filter="${GTEST_FILTER}"
