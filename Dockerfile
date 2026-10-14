# Builds the Mixxx binary for the deck's Raspberry Pi (arm64).
#
# The Pi runs Mixxx from apt and only /usr/bin/mixxx is swapped out, so this
# binary has to link against exactly the libraries already on the device. Two
# things follow from that:
#
#   - BASE must be the same Debian release the Pi runs, which is trixie. It is
#     pinned here and in the deploy step rather than read off the device, so a
#     build does not need the deck switched on and cannot quietly change
#     release; the deploy step checks the deck against the pin and stops if
#     they disagree. A
#     newer release links against a newer glibc and the binary then refuses to
#     start on the Pi; an older one fails at the link step, described further
#     down beside the build tree's cache key.
#   - Anything this build links dynamically has to already be on the Pi. The
#     dependencies below are Mixxx's own Debian build-deps, so that holds by
#     construction; the deploy step re-checks with ldd before installing anyway.
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
# Built and deployed by the TriMixxx repo's deploy step 004_mixxx
# (pi-qemu/deploy/004_mixxx.sh): pi-qemu deck deploy TARGET mixxx.

ARG BASE=debian:trixie
FROM ${BASE} AS build
# Re-declared so it can be read inside the stage: the cache mounts below are
# keyed on it, because they hold compiler output and two Debian releases must
# not share them. See the RUN that builds mixxx.
ARG BASE
# Off by default: the unstripped binary is 156 MB against a 27 MB one, and a
# routine build should not carry it. Turned on only to read a backtrace; see
# the `debug` stage.
ARG WITH_DEBUG=0
# Capped rather than $(nproc), for the same reason the unittest stage below is:
# several of Mixxx's translation units are large enough that a full-width build
# exhausts the Docker VM's memory and the OOM killer takes cc1plus with it. The
# failure reads "fatal error: Killed signal terminated program cc1plus", which
# does not obviously say "out of memory".
#
# It only bites on a COLD build -- an incremental one compiles a handful of
# files and never gets wide enough to matter -- which is why this went unnoticed
# until the first build after the ccache was pruned.
#
# The build's own sub-builds are capped too, in the environment of the RUN
# that builds mixxx: ninja counts each as one job, and each would otherwise
# start as many processes as the VM has CPUs. The ExternalProject builds of
# libdjinterop and libkeyfinder (`cmake --build .`) read
# CMAKE_BUILD_PARALLEL_LEVEL, held to MIXXX_BUILD_JOBS: uncapped, a cold build
# peaked at 25 cc1plus (4 GB) in those two. cargo (lib/prolink, run by
# cmake/modules/ProlinkRust.cmake) reads CARGO_BUILD_JOBS, held to CARGO_JOBS
# (declared by that RUN): its peak was smaller (9 rustc, 2 GB), and after a Rust
# change cargo is what a deploy waits for, 4 jobs costing it ~2 s against 12.
ARG MIXXX_BUILD_JOBS=4

# Mixxx's build dependencies, from packaging/debian/control.in. Only the mixxx
# target is built here, so libbenchmark-dev and xvfb are left out -- but not
# libgtest-dev, despite control.in filing it under "for running mixxx-test":
# production headers such as src/util/fileinfo.h include <gtest/gtest_prod.h>
# for FRIEND_TEST, so the main build needs it too.
#
# Four are not in control.in: ca-certificates, because cmake fetches
# libkeyfinder and libdjinterop over https while Debian's own mirrors are plain
# http; qt6-shadertools-dev, which the Qt6 find_package needs once QML is on;
# libfftw3-dev, for the statically linked KeyFinder; and mold, the linker
# (MOLD_SUPPORT, below).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build ccache mold pkg-config git \
        ca-certificates curl libgtest-dev \
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

# The Pro DJ Link support lives in lib/prolink, a Rust workspace linked
# statically into the binary. Debian's own rustc is 1.85 and cannot build it --
# it uses let-chains, stable since 1.88 -- so the toolchain comes from rustup,
# pinned by lib/prolink/rust-toolchain.toml. Installed in its own layer so a
# source edit does not re-download it.
#
# Nothing from this reaches the Pi: a Rust staticlib is absorbed into the
# binary, which is the point. The deploy step's ldd check stays as true as it was.
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo PATH=/opt/cargo/bin:$PATH
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
      | sh -s -- -y --no-modify-path --default-toolchain none
# And the toolchain rust-toolchain.toml names, in a layer keyed on that file
# alone. Without it, cargo found no toolchain at build time and rustup fetched
# one into the build's own layer: 1.5 GB downloaded, and left behind in
# Docker's disk, by every build.
COPY lib/prolink/rust-toolchain.toml /opt/rust-toolchain/rust-toolchain.toml
RUN cd /opt/rust-toolchain && rustup toolchain install

COPY . /src

# The build tree and the ccache are cache mounts, so editing a couple of files
# and re-running only recompiles those files instead of all ~1500 of them --
# which is what makes a re-upload quick. Their contents do not survive the RUN,
# so the binary is copied out to /mixxx.
#
# Stripped on the way out: Debian ships a stripped binary too, and it keeps the
# scp to the deck short.
#
# BUILD_TESTING=OFF explicitly. It defaults to whether GTest was found, and
# libgtest-dev is one of Mixxx's build dependencies -- so it turns itself on
# here, configures the mixxx-test target, and then fails to generate because
# GTest::gmock is a separate Debian package that only the unittest stage below
# installs. The failure is a configure error about a target nobody asked for.
#
# -g1 instead of RelWithDebInfo's -g: function names and line tables, which is
# all a backtrace needs (the debug stage below), without the types and local
# variables that made every compile job, object, the ccache and the archive
# several times bigger. BUILD_LOW_MEMORY is Mixxx's own switch: no -pipe, so a
# job's cc1plus and as don't hold their memory at once. Measured with the caps
# and mold above: a cold build 12% faster, cc1plus 281 MB on average against
# 341, the tree 2.0 GB against 5.0, the ccache 85 MB against 409, and a one-file
# build 8 s against 16.
#
# MOLD_SUPPORT=ON explicitly, though Mixxx's CMake picks mold by itself when it
# finds it: the choice is a cached option, so a tree configured before mold was
# installed would keep GNU ld for good. mold linked the unstripped binary (570
# MB with -g) in under a second, where GNU ld took five. The setting stays in
# every tree this configures, and a new worktree's tree is seeded from the main
# checkout's: a commit from before mold built in one (an old branch's new
# worktree, a bisect, a hotfix from an older pi/v tag) stops at "Could NOT find
# mold". Give that commit's configure -DMOLD_SUPPORT=OFF, uncommitted, or merge
# main into its branch; the next build of this Dockerfile turns mold back on.
#
# The build tree is keyed on BASE, and that is not tidiness. It holds
# prolink-rust/release/libprolink_cxx.a -- the C++ shim the cxx crate generates,
# compiled by the *container's* g++ and not by rustc. Shared across releases, a
# copy built by trixie's g++ survives into a bookworm build and the link fails
# with `undefined reference to __cxa_call_terminate`, which reads as a problem
# with the Rust library and is nothing of the sort.
#
# ccache does not need this -- it hashes the compiler into its keys -- and the
# cargo registry is source only. This is the one that matters.
#
# So is which checkout this is (./checkout-id.sh): empty for the main one, its
# own for each git worktree. ccache is shared -- it hashes contents.
#
# The tree builds from its own copy of the sources, /build/src, which
# tree-sync.sh brings level with /src by content: a changed file is copied in
# with a fresh timestamp, an identical one is left alone. ninja and moc go by
# timestamps, and the ones in /src are git's, set when it wrote each file --
# so a fresh worktree looked all new, and a checkout of older code looked
# already built, and shipped whatever the tree had compiled last. By content,
# a tree rebuilds exactly what changed since its own last build.
#
# A worktree's first build starts from a copy of the main checkout's tree
# (SEED_ID: checkout-id.sh), when that one is complete: moc alone is minutes
# in a new tree. The main checkout's own build mounts an empty one there
# ("-none") rather than its tree twice.
#
# Declared here, not with the other ARGs: every RUN after an ARG sees it, and
# the dependency layers above must not rebuild per checkout.
ARG CHECKOUT_ID=
ARG SEED_ID=-none
ARG CARGO_JOBS=8
RUN --mount=type=cache,target=/build,sharing=locked,id=mixxx-build-${BASE}${CHECKOUT_ID} \
    --mount=type=cache,target=/seed,sharing=shared,readonly,id=mixxx-build-${BASE}${SEED_ID} \
    --mount=type=cache,target=/ccache,sharing=locked \
    --mount=type=cache,target=/opt/cargo/registry,sharing=locked \
    export CCACHE_DIR=/ccache CARGO_BUILD_JOBS="${CARGO_JOBS}" \
        CMAKE_BUILD_PARALLEL_LEVEL="${MIXXX_BUILD_JOBS}" \
    && /src/tree-sync.sh /src /build /seed \
    && cmake -S /build/src -B /build/out -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -g1 -DNDEBUG" \
        -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g1 -DNDEBUG" \
        -DBUILD_LOW_MEMORY=ON \
        -DBUILD_TESTING=OFF \
        -DINSTALL_USER_UDEV_RULES=OFF \
        -DMOLD_SUPPORT=ON \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    && cmake --build /build/out --target mixxx --parallel "${MIXXX_BUILD_JOBS}" \
    && touch /build/.complete \
    && strip -o /mixxx /build/out/mixxx \
    && if [ "$WITH_DEBUG" = "1" ]; then cp /build/out/mixxx /mixxx.debug; else : > /mixxx.debug; fi

# Export stage: `--output` copies just the binary out to the host.
FROM scratch AS export
COPY --from=build /mixxx /mixxx

# The same binary with its symbols still on, for reading a backtrace off the
# deck. It is NOT installed there -- its debug info is several times the size of
# the binary and the deck gains nothing from carrying it. It is -g1 (see the
# build's RUN): functions and line numbers, no local variables, so gdb on it
# shows a backtrace but not the locals. The two have identical layout, so an
# address from a stripped run resolves against this one:
#
#   docker build --target debug --build-arg WITH_DEBUG=1 \
#       --output type=local,dest=dist .
#   # on the deck: gdb -p $(pgrep -x mixxx) ... bt ; info proc mappings
#   # here: addr2line -fCe dist/mixxx.debug <addr - loadbase>
FROM scratch AS debug
COPY --from=build /mixxx.debug /mixxx.debug

# Unit tests, for the parts of the tree that can be checked without hardware.
#
#   docker buildx build --target unittest --build-arg GTEST_FILTER='Library*' \
#       $(./checkout-id.sh --build-args) --output type=cacheonly .
#
# (--output type=cacheonly: the tests run, and no ~2 GB image is left behind.)
#
# **The Pro DJ Link tests are not here any more.** The protocol moved to
# lib/prolink and its tests moved with it -- 659 of them, including a replay of
# 37 captures of real CDJ traffic that must re-encode byte for byte. Run them
# with `cargo test --workspace` in lib/prolink, or with the prolink-tests stage
# below, which does it in this same container.
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
ARG GTEST_FILTER=*
# ARG scope is per stage, so this has to be repeated to key the test build tree
# on it below -- an unset BASE would silently collapse every release onto one
# cache again.
ARG BASE
# Parallelism is capped rather than $(nproc): the test tree is ~700 translation
# units and several of Mixxx's are large enough that a full-width build exhausts
# the Docker VM's memory and the OOM killer takes cc1plus with it. The failure
# looks like "fatal error: Killed signal terminated program cc1plus", which does
# not obviously say "out of memory". Raise it if the VM has plenty of RAM.
# It caps the ExternalProject builds too, as MIXXX_BUILD_JOBS does; cargo has
# CARGO_JOBS, as there.
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
# Each checkout's own test tree, kept and seeded as /build is (see there).
ARG CHECKOUT_ID=
ARG SEED_ID=-none
ARG CARGO_JOBS=8
RUN --mount=type=cache,target=/build-test,sharing=locked,id=mixxx-build-test-${BASE}${CHECKOUT_ID} \
    --mount=type=cache,target=/seed,sharing=shared,readonly,id=mixxx-build-test-${BASE}${SEED_ID} \
    --mount=type=cache,target=/ccache,sharing=locked \
    --mount=type=cache,target=/opt/cargo/registry,sharing=locked \
    export CCACHE_DIR=/ccache CARGO_BUILD_JOBS="${CARGO_JOBS}" \
        CMAKE_BUILD_PARALLEL_LEVEL="${BUILD_JOBS}" \
    && /src/tree-sync.sh /src /build-test /seed \
    && cmake -S /build-test/src -B /build-test/out -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -g1 -DNDEBUG" \
        -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g1 -DNDEBUG" \
        -DBUILD_LOW_MEMORY=ON \
        -DBUILD_TESTING=ON \
        -DINSTALL_USER_UDEV_RULES=OFF \
        -DMOLD_SUPPORT=ON \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    && cmake --build /build-test/out --target mixxx-test --parallel "${BUILD_JOBS}" \
    && touch /build-test/.complete \
    && QT_QPA_PLATFORM=offscreen /build-test/out/mixxx-test --gtest_filter="${GTEST_FILTER}"

# The protocol's own tests, in the same container the binary is built in.
#
#   docker build --target prolink-tests .
#
# Separate from `unittest` because they need no Qt, no display and no audio
# devices, and because a failure here means something different: the protocol
# is wrong, rather than Mixxx's use of it.
FROM build AS prolink-tests
RUN --mount=type=cache,target=/opt/cargo/registry,sharing=locked \
    cd /src/lib/prolink \
    && cargo test --workspace --locked \
    && cargo clippy --workspace --all-targets --locked -- -D warnings \
    && cargo fmt --all -- --check
