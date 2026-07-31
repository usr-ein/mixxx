# Builds lib/prolink as a static library and exposes it as `prolink::rust`.
#
# SPDX-License-Identifier: GPL-3.0-only
#
# The Pro DJ Link protocol lives in lib/prolink, a Rust workspace, and reaches
# C++ through a `cxx` bridge that generates the header as well as the archive.
# Nothing here is dynamic: the Mixxx binary is swapped onto a Raspberry Pi that
# runs the rest of Mixxx from apt, so anything linked dynamically would have to
# already be on the device. A Rust library never will be.
#
# Two consequences worth knowing before editing this:
#
#   - **The generated header is a build artefact.** `prolink-cxx/src/lib.rs.h`
#     does not exist until cargo has run, so the include directory is attached
#     to the imported target rather than to the source tree, and anything that
#     includes it must depend on `prolink::rust`.
#
#   - **cargo decides when to rebuild, not CMake.** Listing every .rs file as a
#     dependency would be a second source of truth that goes stale silently;
#     the custom target is always considered out of date and cargo's own
#     fingerprinting makes the no-op case fast.

find_program(CARGO_EXECUTABLE cargo
  HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin" /opt/cargo/bin)
if(NOT CARGO_EXECUTABLE)
  message(FATAL_ERROR
    "cargo was not found, and lib/prolink is not optional: the Pro DJ Link "
    "support is implemented there. Install a Rust toolchain (rustup is what "
    "the Dockerfile uses; Debian's own rustc is 1.85 and too old -- see "
    "lib/prolink/rust-toolchain.toml).")
endif()

set(PROLINK_RUST_DIR "${CMAKE_SOURCE_DIR}/lib/prolink")
if(NOT EXISTS "${PROLINK_RUST_DIR}/Cargo.toml")
  message(FATAL_ERROR
    "lib/prolink is empty. It is a git submodule: run "
    "`git submodule update --init lib/prolink`.")
endif()

# One profile, not a mapping from CMAKE_BUILD_TYPE. A debug Rust build of this
# is several times slower at parsing captures and the difference is audible
# when a deck is waiting for a menu; the debug information that matters for
# Mixxx itself is unaffected.
set(PROLINK_RUST_PROFILE release)
set(PROLINK_RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/prolink-rust")
set(PROLINK_RUST_LIB
  "${PROLINK_RUST_TARGET_DIR}/${PROLINK_RUST_PROFILE}/libprolink_cxx.a")
# Where `cxx` writes the header it generates from the bridge module. The
# include path a translation unit uses is "prolink-cxx/src/lib.rs.h", which
# resolves under this.
set(PROLINK_RUST_INCLUDE_DIR "${PROLINK_RUST_TARGET_DIR}/cxxbridge")

add_custom_command(
  OUTPUT "${PROLINK_RUST_LIB}"
  COMMAND "${CMAKE_COMMAND}" -E env
          "CARGO_TARGET_DIR=${PROLINK_RUST_TARGET_DIR}"
          "${CARGO_EXECUTABLE}" build --locked --${PROLINK_RUST_PROFILE}
          --package prolink-cxx
  WORKING_DIRECTORY "${PROLINK_RUST_DIR}"
  COMMENT "Building lib/prolink (Rust, static)"
  VERBATIM)

add_custom_target(prolink-rust-build DEPENDS "${PROLINK_RUST_LIB}")

add_library(prolink::rust STATIC IMPORTED GLOBAL)
add_dependencies(prolink::rust prolink-rust-build)
# The header is generated, so the directory has to exist at configure time or
# INTERFACE_INCLUDE_DIRECTORIES is rejected before cargo has ever run.
file(MAKE_DIRECTORY "${PROLINK_RUST_INCLUDE_DIR}")
set_target_properties(prolink::rust PROPERTIES
  IMPORTED_LOCATION "${PROLINK_RUST_LIB}"
  INTERFACE_INCLUDE_DIRECTORIES "${PROLINK_RUST_INCLUDE_DIR}")

# What a Rust staticlib needs from the platform, as
# `rustc --print native-static-libs` reports it. libm and libdl are absorbed
# into glibc on Debian trixie but are named for older targets and for musl.
if(UNIX AND NOT APPLE)
  target_link_libraries(prolink::rust INTERFACE pthread dl m)
elseif(APPLE)
  target_link_libraries(prolink::rust INTERFACE
    iconv
    "-framework CoreFoundation"
    "-framework SystemConfiguration"
    "-framework Security")
endif()

message(STATUS "Pro DJ Link: lib/prolink (Rust, static) -> ${PROLINK_RUST_LIB}")
