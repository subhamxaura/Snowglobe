add_library(snowglobe_core STATIC
  util/string_util.cpp
  util/sha256.cpp
  trace/jsonl_writer.cpp
  tracer/common/trace_helpers.cpp
  tracer/ptrace/ptrace_tracer.cpp
  tracer/common/backend.cpp
  tracer/notify/notify_filter.cpp
  tracer/notify/notify_tracer.cpp
  doctor/doctor.cpp
  redact/redact.cpp
  proxy/proxy.cpp
  view/view.cpp
  link/linker.cpp
  link/link_cli.cpp
  isolate/isolate.cpp
  isolate/seccomp.cpp
  isolate/landlock.cpp
  diff/myers.cpp
  diff/baseline.cpp
  diff/changeset.cpp
  diff/apply.cpp
  diff/cli.cpp
)
target_include_directories(snowglobe_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(snowglobe_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(SNOWGLOBE_BUILD_VIEWER)
  # Generated header with the packed viewer (built at build time, no reconfigure).
  target_include_directories(snowglobe_core PRIVATE "${CMAKE_BINARY_DIR}/generated")
  target_compile_definitions(snowglobe_core PRIVATE SNOWGLOBE_HAS_VIEWER)
  add_dependencies(snowglobe_core snowglobe_viewer_data)
endif()
if(SNOWGLOBE_WITH_SSL)
  target_link_libraries(snowglobe_core PUBLIC httplib::httplib OpenSSL::SSL OpenSSL::Crypto)
else()
  target_link_libraries(snowglobe_core PUBLIC httplib::httplib)
endif()

add_executable(snowglobe cli/main.cpp)
target_link_libraries(snowglobe PRIVATE snowglobe_core)
