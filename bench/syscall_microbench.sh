#!/bin/sh
# bench/syscall_microbench.sh — fork+exec loop + python startup under none/ptrace.
# Full benchmark matrix lands in Phase 4; this stub establishes the harness.
set -eu
OUT="${1:-bench/results/ptrace_local.json}"
mkdir -p "$(dirname "$OUT")"
echo "warmup: fork+exec loop (100x /bin/true, no tracer)"
i=0
start=$(date +%s%N)
while [ "$i" -lt 100 ]; do /bin/true; i=$((i + 1)); done
end=$(date +%s%N)
echo "untraced 100x /bin/true: $(( (end - start) / 1000000 )) ms"
echo "{\"mode\":\"none\",\"note\":\"tracer benches land in Phase 4 (see docs/adr/0001)\"}" > "$OUT"
echo "wrote $OUT"
