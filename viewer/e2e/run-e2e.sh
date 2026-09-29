#!/bin/bash
# CTest driver for the viewer Playwright suite. Skips with a reason when
# the runtime is absent (same convention as the tracer tests); otherwise
# runs the suite against SNOWGLOBE_BIN (must be set by CTest).
set -u
if ! command -v node > /dev/null 2>&1; then
  echo "SKIP: node not installed (viewer e2e needs Node 18+)"
  exit 0
fi
if ! node -e "process.exit(Number(process.versions.node.split('.')[0]) >= 18 ? 0 : 1)"; then
  echo "SKIP: node < 18 (viewer needs 18.17+)"
  exit 0
fi
if [ -z "${SNOWGLOBE_BIN:-}" ] || [ ! -x "$SNOWGLOBE_BIN" ]; then
  echo "SKIP: SNOWGLOBE_BIN is not set to an executable"
  exit 0
fi
if [ ! -f package.json ] || [ ! -d node_modules/@playwright ]; then
  echo "SKIP: viewer node_modules missing (npm ci in viewer/ first)"
  exit 0
fi
BDIR="${PLAYWRIGHT_BROWSERS_PATH:-$HOME/.cache/ms-playwright}"
if ! ls "$BDIR" 2>/dev/null | grep -q -E 'chromium'; then
  echo "SKIP: playwright chromium not downloaded (npx playwright install chromium)"
  exit 0
fi
if [ -z "${FIXTURE_DIR:-}" ] || [ ! -f "$FIXTURE_DIR/events.jsonl" ]; then
  echo "SKIP: FIXTURE_DIR is not set to a trace dir"
  exit 0
fi
exec npx playwright test --reporter=line
