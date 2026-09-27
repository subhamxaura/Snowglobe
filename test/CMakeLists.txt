include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.4.0
)
FetchContent_MakeAvailable(Catch2)

add_executable(snowglobe_tests
  unit/test_json_escape.cpp
  unit/test_sha256.cpp
  unit/test_hash_chain.cpp
  unit/test_open_flags.cpp
)
target_link_libraries(snowglobe_tests PRIVATE snowglobe_core Catch2::Catch2WithMain)
target_include_directories(snowglobe_tests PRIVATE ${CMAKE_SOURCE_DIR}/core)

include(CTest)
include(Catch)
catch_discover_tests(snowglobe_tests)

# Integration: trace a scripted process tree, assert fs.open/fs.rename/fs.unlink sequence.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(
    NAME tracer_basic
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/tracer_basic.py
            $<TARGET_FILE:snowglobe>
  )
endif()
