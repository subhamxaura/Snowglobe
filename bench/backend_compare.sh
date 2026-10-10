#!/bin/bash
# bench/backend_compare.sh — Phase 4 Block 2: ptrace vs notify overhead.
# Same 4 network-free workloads as bench/ptrace_baseline.sh, 5 runs each:
# plain vs --backend=ptrace vs --backend=notify. Medians + min/max +
# median per-backend event counts. Numbers only; methodology is the script.
# Usage: bench/backend_compare.sh <snowglobe-bin> <out-md> <repo-dir>
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

# run_timed <mode> <snippet> — mode is "plain", "ptrace" or "notify".
# Prints elapsed ns; traced modes append the event count ("<ns> <events>").
run_timed() {
  local mode="$1" snippet="$2" t0 t1 work out nev
  if [ "$mode" = "plain" ]; then
    t0=$(now_ns)
    bash -c "$snippet" > /dev/null 2>&1
    t1=$(now_ns)
    echo $((t1 - t0))
  else
    work=$(mktemp -d)
    out="$work/run.sgr"
    t0=$(now_ns)
    "$SG" run --out="$out" --backend="$mode" -- bash -c "$snippet" > /dev/null 2>&1
    t1=$(now_ns)
    nev=$(wc -l < "$out/events.jsonl")
    rm -rf "$work"
    echo "$((t1 - t0)) $nev"
  fi
}

bench_one() {
  # bench_one <name> <snippet> — prints:
  # name|plain_med|min|max|ptrace_med|min|max|ptrace_events|notify_med|min|max|notify_events
  local name="$1" snippet="$2" i p t e u v
  p=""
  t=""
  e=""
  u=""
  v=""
  i=1
  while [ "$i" -le 5 ]; do
    p="$p $(run_timed plain "$snippet")"
    # shellcheck disable=SC2086
    te=$(run_timed ptrace "$snippet")
    t="$t $(echo "$te" | cut -d' ' -f1)"
    e="$e $(echo "$te" | cut -d' ' -f2)"
    # shellcheck disable=SC2086
    ue=$(run_timed notify "$snippet")
    u="$u $(echo "$ue" | cut -d' ' -f1)"
    v="$v $(echo "$ue" | cut -d' ' -f2)"
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
  # shellcheck disable=SC2086
  set -- $e
  local tem
  tem=$(median5 "$@")
  # shellcheck disable=SC2086
  set -- $u
  local um un ux
  um=$(median5 "$@")
  un=$(min5 "$@")
  ux=$(max5 "$@")
  # shellcheck disable=SC2086
  set -- $v
  local uem
  uem=$(median5 "$@")
  echo "$name|$pm|$mn|$mx|$tm|$tn|$tx|$tem|$um|$un|$ux|$uem"
}

{
  echo "# backend comparison: ptrace vs notify (Phase 4 Block 2)"
  echo
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "machine: $(uname -m) nproc=$(nproc) mem_kb=$(awk '/MemTotal/{print $2}' /proc/meminfo)"
  echo "kernel: $(uname -r)"
  echo "uid: $(id -u)"
  echo "snowglobe: $("$SG" version 2>&1 | head -1)"
  echo "repo: $REPO commit=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "repo_dirty_files: $(git -C "$REPO" status --porcelain 2>/dev/null | wc -l)"
  echo "method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through \`snowglobe run --backend=<be>\` with identical \`bash -c\` shape (.sgr event counts are medians); .sgr dirs removed after each run"
  echo
  echo "| workload | untraced med ms (min-max) | ptrace med ms (min-max) | ptrace ratio | ptrace events | notify med ms (min-max) | notify ratio | notify events |"
  echo "|---|---|---|---|---|---|---|---|"
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
    tem=$(echo "$line" | cut -d'|' -f8)
    um=$(echo "$line" | cut -d'|' -f9)
    un=$(echo "$line" | cut -d'|' -f10)
    ux=$(echo "$line" | cut -d'|' -f11)
    uem=$(echo "$line" | cut -d'|' -f12)
    tratio=$(awk "BEGIN{printf \"%.2f\", $tm/$pm}")
    uratio=$(awk "BEGIN{printf \"%.2f\", $um/$pm}")
    echo "| $name | $(ms1 "$pm") ($(ms1 "$mn")-$(ms1 "$mx")) | $(ms1 "$tm") ($(ms1 "$tn")-$(ms1 "$tx")) | ${tratio}x | $tem | $(ms1 "$um") ($(ms1 "$un")-$(ms1 "$ux")) | ${uratio}x | $uem |"
  done
} > "$OUT"
echo "wrote $OUT"
