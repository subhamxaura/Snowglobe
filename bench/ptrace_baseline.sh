#!/bin/bash
# bench/ptrace_baseline.sh — Phase 1A Block 3 "before" numbers for Phase 4.
# Network-free workloads, 5 runs each, traced vs untraced, medians + min/max.
# Numbers only; methodology is the script itself.
# Usage: bench/ptrace_baseline.sh <snowglobe-bin> <out-md> <repo-dir>
set -u

SG="$1"
OUT="$2"
REPO="$3"
mkdir -p "$(dirname "$OUT")"

now_ns() {
  date +%s%N
}

# median of "$@" (odd count): 3rd of 5 sorted.
median5() {
  printf '%s\n' "$1" "$2" "$3" "$4" "$5" | sort -n | sed -n '3p'
}
min5() {
  printf '%s\n' "$1" "$2" "$3" "$4" "$5" | sort -n | sed -n '1p'
}
max5() {
  printf '%s\n' "$1" "$2" "$3" "$4" "$5" | sort -n | sed -n '5p'
}

ms1() {
  awk "BEGIN{printf \"%.1f\", $1/1000000}"
}

# run_timed <mode> <snippet> — mode is "plain" or "traced".
run_timed() {
  local mode="$1" snippet="$2" t0 t1 work out
  if [ "$mode" = "traced" ]; then
    work=$(mktemp -d)
    out="$work/run.sgr"
    t0=$(now_ns)
    "$SG" run --out="$out" -- bash -c "$snippet" > /dev/null 2>&1
    t1=$(now_ns)
    rm -rf "$work"
  else
    t0=$(now_ns)
    bash -c "$snippet" > /dev/null 2>&1
    t1=$(now_ns)
  fi
  echo $((t1 - t0))
}

bench_one() {
  # bench_one <name> <snippet> — prints: name|plain_med|min|max|traced_med|min|max
  local name="$1" snippet="$2" i p t
  p=""
  t=""
  i=1
  while [ "$i" -le 5 ]; do
    p="$p $(run_timed plain "$snippet")"
    # shellcheck disable=SC2086
    t="$t $(run_timed traced "$snippet")"
    i=$((i + 1))
  done
  # shellcheck disable=SC2086
  set -- $p
  local pm mn mx
  pm=$(median5 "$@")
  mn=$(min5 "$@")
  mx=$(max5 "$@")
  # shellcheck disable=SC2086
  set -- $t
  local tm tn tx
  tm=$(median5 "$@")
  tn=$(min5 "$@")
  tx=$(max5 "$@")
  echo "$name|$pm|$mn|$mx|$tm|$tn|$tx"
}

{
  echo "# ptrace baseline (Phase 1A Block 3)"
  echo
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "machine: $(uname -m) nproc=$(nproc) mem_kb=$(awk '/MemTotal/{print $2}' /proc/meminfo)"
  echo "kernel: $(uname -r)"
  echo "uid: $(id -u)"
  echo "snowglobe: $("$SG" version 2>&1 | head -1)"
  echo "repo: $REPO commit=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "repo_dirty_files: $(git -C "$REPO" status --porcelain 2>/dev/null | wc -l)"
  echo "method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through \`snowglobe run\` with identical \`bash -c\` shape; .sgr dirs removed after each run"
  echo
  echo "| workload | untraced med ms (min-max) | traced med ms (min-max) | overhead ms | ratio |"
  echo "|---|---|---|---|---|"
  for workload in forkexec-300 python-import git-status find-usrlib; do
    case "$workload" in
      forkexec-300) snippet='for i in $(seq 1 300); do /bin/cat /etc/hostname > /dev/null; done' ;;
      python-import) snippet='python3 -c "import json,asyncio,http.client"' ;;
      git-status) snippet="git -C \"$REPO\" status --porcelain=v1 > /dev/null" ;;
      find-usrlib) snippet='find /usr/lib -type f | wc -l' ;;
    esac
    line=$(bench_one "$workload" "$snippet")
    name=$(echo "$line" | cut -d'|' -f1)
    pm=$(echo "$line" | cut -d'|' -f2)
    mn=$(echo "$line" | cut -d'|' -f3)
    mx=$(echo "$line" | cut -d'|' -f4)
    tm=$(echo "$line" | cut -d'|' -f5)
    tn=$(echo "$line" | cut -d'|' -f6)
    tx=$(echo "$line" | cut -d'|' -f7)
    ovms=$(ms1 "$((tm - pm))")
    ratio=$(awk "BEGIN{printf \"%.2f\", $tm/$pm}")
    echo "| $name | $(ms1 "$pm") ($(ms1 "$mn")-$(ms1 "$mx")) | $(ms1 "$tm") ($(ms1 "$tn")-$(ms1 "$tx")) | $ovms | ${ratio}x |"
  done
} > "$OUT"
echo "wrote $OUT"
