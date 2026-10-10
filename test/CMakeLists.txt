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
  unit/test_sockaddr.cpp
  unit/test_link_scan.cpp
  unit/test_link.cpp
  unit/test_link_fixtures.cpp
  unit/test_isolate.cpp
  unit/test_notify.cpp
  unit/test_myers.cpp
  unit/test_baseline.cpp
)
target_link_libraries(snowglobe_tests PRIVATE snowglobe_core Catch2::Catch2WithMain)
target_include_directories(snowglobe_tests PRIVATE ${CMAKE_SOURCE_DIR}/core)
# Ground-truth linker tests read committed fixtures (already normalised).
target_compile_definitions(snowglobe_tests PRIVATE
  SNOWGLOBE_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/test/fixtures/real")

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

  # Phase 4 Block 2: notify-backend parity (ADR-0009). goldens-notify pins
  # the notify event stream per scenario (expected-notify.jsonl:
  # shape-equivalent kinds/paths, result unknowns pinned); parity_*
  # checks the ptrace<->notify relation (subsequence + shape rules incl.
  # the exec_failed->exec mapping). Both skip with a reason where the
  # kernel lacks SECCOMP_RET_USER_NOTIF.
  foreach(scn IN ITEMS fork_storm threads exec_chain deep_dirs unix_sockets failing_exec
      tcp_loopback)
    add_test(
      NAME golden_notify_${scn}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/golden.py
              $<TARGET_FILE:snowglobe> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scenarios/${scn}
              ${CMAKE_SOURCE_DIR} $<TARGET_FILE_DIR:sg_threads> --backend=notify
    )
    add_test(
      NAME parity_${scn}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/parity/check_parity.py
              $<TARGET_FILE:snowglobe> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scenarios/${scn}
              ${CMAKE_SOURCE_DIR} $<TARGET_FILE_DIR:sg_threads>
    )
    set_tests_properties(parity_${scn} PROPERTIES TIMEOUT 300)
  endforeach()

  # Phase 4 Block 2: kill tests under notify. (a) proves supervisor
  # SIGKILL never strands a fifo-blocked tracee; (d) proves a
  # trapped-syscall storm leaves no wedged children (unanswered
  # notifications die with their tracees via EXITKILL).

  # Automated kill tests: supervisor SIGKILL, root SIGKILL, supervisor SIGTERM.
  foreach(kill IN ITEMS a b c)
    add_test(
      NAME kill_${kill}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/kill_tests.py
              $<TARGET_FILE:snowglobe> ${CMAKE_SOURCE_DIR} ${kill}
    )
    set_tests_properties(kill_${kill} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(nkill IN ITEMS a b c d)
    add_test(
      NAME notify_kill_${nkill}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/notify/notify_kill.py
              $<TARGET_FILE:snowglobe> ${CMAKE_SOURCE_DIR} ${nkill}
    )
    set_tests_properties(notify_kill_${nkill} PROPERTIES TIMEOUT 120)
  endforeach()

  # Phase 1B Block 2: mock LLM + toy-agent proxy integration (stdlib only).
  # Uniform argv: <snowglobe> <mockllm-dir> <toyagent-dir>. Each test starts
  # its own mock on a fixed localhost port (no API keys anywhere).
  set(SG_PROXY_TESTS
    toy_agent:180 latency:300 concurrent:180 disconnect:180 errors:120
    large:300 redact_argv:120 anthropic:120 error_turn:120 no_tmp:180
    notify_toy:180 notify_concurrent:180)
  # Latency budget scales with instrumentation: the 5 ms product budget
  # (AGENTS.md §5) holds on plain builds; ASan/TSan slow the proxy ~5x, so
  # those presets bound the delta at 25 ms instead. Still a bound, still
  # green — the product number is the debug one.
  if(CMAKE_CXX_FLAGS MATCHES "sanitize=(address|thread)")
    set(SG_LAT_BUDGET_S 0.025)
  else()
    set(SG_LAT_BUDGET_S 0.005)
  endif()
  foreach(pair IN ITEMS ${SG_PROXY_TESTS})
    string(REPLACE ":" ";" parts "${pair}")
    list(GET parts 0 tname)
    list(GET parts 1 ttimeout)
    add_test(
      NAME proxy_${tname}
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/proxy/proxy_${tname}.py
              $<TARGET_FILE:snowglobe> ${CMAKE_CURRENT_SOURCE_DIR}/mockllm
              ${CMAKE_SOURCE_DIR}/examples/toy-agent
    )
    set_tests_properties(proxy_${tname} PROPERTIES TIMEOUT ${ttimeout})
    # Per-test environment: latency budget (always) + TSan suppressions
    # (thread-sanitizer builds only — see test/tsan.supp).
    set(_sgenv)
    if(tname STREQUAL "latency")
      list(APPEND _sgenv "SG_LAT_BUDGET_S=${SG_LAT_BUDGET_S}")
    endif()
    if(CMAKE_CXX_FLAGS MATCHES "sanitize=thread")
      list(APPEND _sgenv "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp")
    endif()
    if(_sgenv)
      set_tests_properties(proxy_${tname} PROPERTIES ENVIRONMENT "${_sgenv}")
    endif()
  endforeach()

  # Phase 1C Block 1: view server API (stdlib only, fixture copied to temp).
  add_test(
    NAME view_api
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/view/view_api.py
            $<TARGET_FILE:snowglobe> ${CMAKE_SOURCE_DIR}/test/fixtures/real/toy-agent-3turn
  )
  set_tests_properties(view_api PROPERTIES TIMEOUT 180)

  # Phase 1D Block 1: linker determinism (double-run byte-compare on
  # fixture copies — link writes links.json, never into the repo tree).
  add_test(
    NAME link_determinism
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/link/link_determinism.py
            $<TARGET_FILE:snowglobe> ${CMAKE_SOURCE_DIR}/test/fixtures/real
  )
  set_tests_properties(link_determinism PROPERTIES TIMEOUT 180)

  # Phase 2 Block 1: --isolate containment. The basic test skips loudly
  # (exit 69) where the kernel cannot do parent-mapped userns + overlay;
  # the toy test additionally needs the mock + agent dirs.
  add_test(
    NAME isolate_basic
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/isolate/isolate_basic.py
            $<TARGET_FILE:snowglobe>
  )
  set_tests_properties(isolate_basic PROPERTIES TIMEOUT 180)
  add_test(
    NAME isolate_toy
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/isolate/isolate_toy.py
            $<TARGET_FILE:snowglobe> ${CMAKE_CURRENT_SOURCE_DIR}/mockllm
            ${CMAKE_SOURCE_DIR}/examples/toy-agent
  )
  set_tests_properties(isolate_toy PROPERTIES TIMEOUT 300)

  # Phase 3: diff/apply/compare over the overlay upper (ADR-0008). The
  # golden drives a scripted --isolate run (create/modify/delete/rename,
  # unicode, symlink, empty + large files) and pins diff --stat/--patch,
  # apply (clean + conflict-zero-writes + dry-run), and compare output.
  # Like isolate_basic, it skips loudly (exit 69) without userns+overlay.
  add_test(
    NAME diff_apply
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/diff/diff_apply_golden.py
            $<TARGET_FILE:snowglobe>
  )
  set_tests_properties(diff_apply PROPERTIES TIMEOUT 300)
endif()

# Phase 1C: viewer tests. vitest (parser + model units on fixtures) needs
# only node; the Playwright suite (run-e2e.sh) skips cleanly without browsers.
if(SNOWGLOBE_BUILD_VIEWER)
  add_test(
    NAME viewer_unit
    COMMAND "${SNOWGLOBE_NPM}" run test --silent
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/viewer"
  )
  set_tests_properties(viewer_unit PROPERTIES TIMEOUT 300)
  add_test(
    NAME viewer_e2e
    COMMAND bash "${CMAKE_SOURCE_DIR}/viewer/e2e/run-e2e.sh"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/viewer"
  )
  set_tests_properties(viewer_e2e PROPERTIES
    TIMEOUT 600
    ENVIRONMENT "SNOWGLOBE_BIN=$<TARGET_FILE:snowglobe>;FIXTURE_DIR=${CMAKE_SOURCE_DIR}/test/fixtures/real/toy-agent-3turn;ERROR_FIXTURE_DIR=${CMAKE_SOURCE_DIR}/test/fixtures/real/claude-code-1-error")
endif()
