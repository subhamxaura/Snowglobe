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

add_subdirectory(core)

if(BUILD_TESTING)
  include(CTest)
  add_subdirectory(test)
endif()
