#!/bin/bash
# Format gate, single source: C/C++ headers and sources ONLY.
# Never pass other languages to clang-format — it mangles them instead
# of refusing (a *.py file was destroyed this way in 1D Block 1).
# Tracked AND untracked files (new work is untracked until committed —
# the gate missed whole new files twice before this line existed).
# CI and developers both call this; no hand-typed globs.
set -euo pipefail
cd "$(dirname "$0")/.."
{
  git ls-files '*.cpp' '*.hpp' '*.h' '*.c'
  git ls-files --others --exclude-standard '*.cpp' '*.hpp' '*.h' '*.c'
} | sort -u | xargs -r clang-format --dry-run --Werror
