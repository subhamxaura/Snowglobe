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
  unit/test_redact.cpp
  unit/test_proxy_util.cpp
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

  # Golden scenario fixtures: run each scenario under snowglobe, compare
  # normalised events to test/fixtures/scenarios/<name>/expected.jsonl.
  # Regenerate with SNOWGLOBE_UPDATE_GOLDENS=1 (plus a CHANGELOG line).
  # threads/unix_sockets use compiled C helpers (no interpreter: python
  # startup file sets differ per release and cannot be normalised).
  add_executable(sg_threads fixtures/scenarios/threads/sg_threads.c)
  add_executable(sg_sockets fixtures/scenarios/unix_sockets/sg_sockets.c)
  add_executable(sg_tcp fixtures/scenarios/tcp_loopback/sg_tcp.c)
  set_target_properties(sg_threads sg_sockets sg_tcp
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/helpers)
  # Helpers are traced workloads, not code under test: build them WITHOUT
  # sanitizers in every preset. An instrumented tracee loads extra runtime
  # libraries (libasan/libubsan/libtsan: golden divergence across presets)
  # and LeakSanitizer fatally fails under ptrace ("does not work under
  # ptrace (strace, gdb, etc)"). The observer (snowglobe) stays instrumented.
  foreach(helper IN ITEMS sg_threads sg_sockets sg_tcp)
    target_compile_options(${helper} PRIVATE -fno-sanitize=address,undefined,thread)
    target_link_options(${helper} PRIVATE -fno-sanitize=address,undefined,thread)
  endforeach()
  foreach(scn IN ITEMS fork_storm threads exec_chain deep_dirs unix_sockets failing_exec
      tcp_loopback)
    add_test(
      NAME golden_${scn}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/golden.py
              $<TARGET_FILE:snowglobe> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scenarios/${scn}
              ${CMAKE_SOURCE_DIR} $<TARGET_FILE_DIR:sg_threads>
    )
  endforeach()

  # Automated kill tests: supervisor SIGKILL, root SIGKILL, supervisor SIGTERM.
  foreach(kill IN ITEMS a b c)
    add_test(
      NAME kill_${kill}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/kill_tests.py
              $<TARGET_FILE:snowglobe> ${CMAKE_SOURCE_DIR} ${kill}
    )
    set_tests_properties(kill_${kill} PROPERTIES TIMEOUT 120)
  endforeach()
endif()
