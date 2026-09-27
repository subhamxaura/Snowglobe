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

if(SNOWGLOBE_STATIC)
  # Pinned musl toolchain container handles this for releases; this flag is a
  # best-effort hint for local static builds.
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
endif()

# OpenSSL for the LLM proxy's upstream TLS (AGENTS.md §1.2 allowed dep).
# Static OpenSSL is a Phase 4 problem: link dynamically now (STATUS.md).
find_package(OpenSSL REQUIRED)

# cpp-httplib for the LLM proxy (AGENTS.md §1.2 allowed dep). SYSTEM include
# so -Werror never fires inside the third-party header.
include(FetchContent)
FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.20.1
  SYSTEM
)
set(HTTPLIB_REQUIRE_OPENSSL ON)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF)
set(HTTPLIB_TEST OFF)
set(HTTPLIB_INSTALL OFF)
set(HTTPLIB_COMPILE OFF)
FetchContent_MakeAvailable(httplib)

add_subdirectory(core)

if(BUILD_TESTING)
  include(CTest)
  add_subdirectory(test)
endif()
