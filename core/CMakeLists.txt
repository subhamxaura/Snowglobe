add_library(snowglobe_core STATIC
  util/string_util.cpp
  util/sha256.cpp
  trace/jsonl_writer.cpp
  tracer/ptrace/ptrace_tracer.cpp
  doctor/doctor.cpp
  redact/redact.cpp
  proxy/proxy.cpp
)
target_include_directories(snowglobe_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(snowglobe_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(snowglobe_core PUBLIC httplib::httplib OpenSSL::SSL OpenSSL::Crypto)

add_executable(snowglobe cli/main.cpp)
target_link_libraries(snowglobe PRIVATE snowglobe_core)
