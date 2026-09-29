cmake_minimum_required(VERSION 3.25)
project(snowglobe VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# Global warnings — non-negotiable per AGENTS.md §5
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# Build type defaults (single-config generators / presets set this explicitly)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(BUILD_TESTING "Build tests" ON)
option(SNOWGLOBE_STATIC "Attempt fully static link (musl release builds)" OFF)
# TLS for the LLM proxy's upstream HTTPS. ON by default (native builds test
# TLS); OFF for aarch64 cross compile checks where only host x86_64 OpenSSL
# exists and the cross compiler cannot use it (opensslconf.h arch mismatch).
# The proxy still compiles (httplib without CPPHTTPLIB_OPENSSL_SUPPORT);
# https upstreams would fail at runtime, but the cross job is build-only.
option(SNOWGLOBE_WITH_SSL "Enable TLS via OpenSSL for LLM proxy" ON)

if(SNOWGLOBE_STATIC)
  # Pinned musl toolchain container handles this for releases; this flag is a
  # best-effort hint for local static builds.
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
endif()

# OpenSSL for the LLM proxy's upstream TLS (AGENTS.md §1.2 allowed dep).
# Static OpenSSL is a Phase 4 problem: link dynamically now (STATUS.md).
# SNOWGLOBE_WITH_SSL=OFF skips OpenSSL entirely (aarch64 cross compile check;
# see option above). httplib then builds without CPPHTTPLIB_OPENSSL_SUPPORT.
if(SNOWGLOBE_WITH_SSL)
  find_package(OpenSSL REQUIRED)
endif()

# cpp-httplib for the LLM proxy (AGENTS.md §1.2 allowed dep). SYSTEM include
# so -Werror never fires inside the third-party header.
include(FetchContent)
FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.20.1
  SYSTEM
)
set(HTTPLIB_REQUIRE_OPENSSL ${SNOWGLOBE_WITH_SSL})
# httplib also auto-enables SSL via USE_IF_AVAILABLE (default ON) whenever it
# finds *any* OpenSSL — including host x86_64 libs under a cross compiler
# (CI has pkg-config, so the host lib is found and CPPHTTPLIB_OPENSSL_SUPPORT
# gets defined, breaking the aarch64 build on opensslconf.h). Tie it to the
# same switch so WITH_SSL=OFF truly means no OpenSSL anywhere.
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE ${SNOWGLOBE_WITH_SSL})
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF)
set(HTTPLIB_TEST OFF)
set(HTTPLIB_INSTALL OFF)
set(HTTPLIB_COMPILE OFF)
FetchContent_MakeAvailable(httplib)

# Viewer (Phase 1C): Next.js static export built with the project's Node,
# then packed into the binary by cmake/embed_viewer.cmake (pure CMake, no
# extra tools). ON by default; the aarch64 cross-check job sets OFF (its
# container has no Node, and the check is compile-only anyway).
option(SNOWGLOBE_VIEWER "Build + embed the Next.js viewer (needs Node 18+)" ON)
if(SNOWGLOBE_VIEWER)
  find_program(SNOWGLOBE_NPM npm REQUIRED)
  find_program(SNOWGLOBE_NODE node REQUIRED)
  set(SNOWGLOBE_VIEWER_OUT "${CMAKE_SOURCE_DIR}/viewer/out")
  set(SNOWGLOBE_VIEWER_HPP "${CMAKE_BINARY_DIR}/generated/snowglobe_viewer_data.hpp")
  file(GLOB_RECURSE SNOWGLOBE_VIEWER_SRCS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/viewer/pages/*"
    "${CMAKE_SOURCE_DIR}/viewer/lib/*"
    "${CMAKE_SOURCE_DIR}/viewer/components/*"
    "${CMAKE_SOURCE_DIR}/viewer/test/*"
    "${CMAKE_SOURCE_DIR}/viewer/styles.css"
    "${CMAKE_SOURCE_DIR}/viewer/package.json"
    "${CMAKE_SOURCE_DIR}/viewer/package-lock.json"
    "${CMAKE_SOURCE_DIR}/viewer/next.config.mjs"
    "${CMAKE_SOURCE_DIR}/viewer/tsconfig.json")
  add_custom_command(
    OUTPUT "${SNOWGLOBE_VIEWER_HPP}"
    COMMAND ${CMAKE_COMMAND} -E env "NEXT_TELEMETRY_DISABLED=1" "CI=true"
      "${SNOWGLOBE_NPM}" ci --no-audit --no-fund
    COMMAND ${CMAKE_COMMAND} -E env "NEXT_TELEMETRY_DISABLED=1" "CI=true"
      "${SNOWGLOBE_NPM}" run build
    COMMAND "${CMAKE_COMMAND}" "-DVIEWER_OUT_DIR=${SNOWGLOBE_VIEWER_OUT}"
      "-DOUTPUT_HPP=${SNOWGLOBE_VIEWER_HPP}"
      -P "${CMAKE_SOURCE_DIR}/cmake/embed_viewer.cmake"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/viewer"
    DEPENDS ${SNOWGLOBE_VIEWER_SRCS}
    COMMENT "Building + embedding snowglobe viewer")
  add_custom_target(snowglobe_viewer_data DEPENDS "${SNOWGLOBE_VIEWER_HPP}")
endif()

add_subdirectory(core)

if(BUILD_TESTING)
  include(CTest)
  add_subdirectory(test)
endif()
