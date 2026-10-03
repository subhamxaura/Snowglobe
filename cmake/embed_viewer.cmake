# cmake/embed_viewer.cmake — pack viewer/out/ into a C++ header.
# Invoked at build time via add_custom_command (no reconfigure needed):
#   cmake -DVIEWER_OUT_DIR=<viewer/out> -DOUTPUT_HPP=<generated header>
#     -P cmake/embed_viewer.cmake
# Delegates to embed_viewer.py (stdlib only): gzip + sha256 etag + MIME +
# immutable bit per file. Python3 is already a test dependency.
find_program(_sg_embed_py python3 NAMES python3 python REQUIRED)
execute_process(
  COMMAND "${_sg_embed_py}" "${CMAKE_CURRENT_LIST_DIR}/embed_viewer.py"
    --out-dir "${VIEWER_OUT_DIR}" --output "${OUTPUT_HPP}"
  RESULT_VARIABLE _sg_embed_rc
  OUTPUT_VARIABLE _sg_embed_out
  ERROR_VARIABLE _sg_embed_out)
message(STATUS "${_sg_embed_out}")
if(NOT _sg_embed_rc EQUAL 0)
  message(FATAL_ERROR "embed_viewer.py failed: ${_sg_embed_rc}")
endif()
