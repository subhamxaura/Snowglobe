#!/bin/bash
# bench/ptrace_baseline.sh — Phase 1A Block 3 "before" numbers for Phase 4.
# Network-free workloads, 5 runs each, plain vs strace-reference vs traced,
# medians + min/max + median traced event counts. Numbers only; methodology
# is the script itself.
# Usage: bench/ptrace_baseline.sh <snowglobe-bin> <out-md> <repo-dir>
set -u

SG="$1"
OUT="$2"
REPO="$3"
mkdir -p "$(dirname "$OUT")"

if command -v strace > /dev/null 2>&1; then
  HAVE_STRACE=1
else
  HAVE_STRACE=0
  echo "note: strace not found — reference column will read n/a" >&2
fi

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

# run_timed <mode> <snippet> — mode is "plain", "strace" or "traced".
# Prints elapsed ns; traced mode appends the event count ("<ns> <events>").
run_timed() {
  local mode="$1" snippet="$2" t0 t1 work out nev
  if [ "$mode" = "traced" ]; then
    work=$(mktemp -d)
    out="$work/run.sgr"
    t0=$(now_ns)
    "$SG" run --out="$out" -- bash -c "$snippet" > /dev/null 2>&1
    t1=$(now_ns)
    nev=$(wc -l < "$out/events.jsonl")
    rm -rf "$work"
    echo "$((t1 - t0)) $nev"
  elif [ "$mode" = "strace" ]; then
    t0=$(now_ns)
    strace -f -qq -o /dev/null bash -c "$snippet" > /dev/null 2>&1
    t1=$(now_ns)
    echo $((t1 - t0))
  else
    t0=$(now_ns)
    bash -c "$snippet" > /dev/null 2>&1
    t1=$(now_ns)
    echo $((t1 - t0))
  fi
}

bench_one() {
  # bench_one <name> <snippet> — prints:
  # name|plain_med|min|max|strace_med|min|max|traced_med|min|max|events_med
  # (strace fields are "na" when strace is not installed).
  local name="$1" snippet="$2" i p s t e
  p=""
  s=""
  t=""
  e=""
  i=1
  while [ "$i" -le 5 ]; do
    p="$p $(run_timed plain "$snippet")"
    if [ "$HAVE_STRACE" = 1 ]; then
      # shellcheck disable=SC2086
      s="$s $(run_timed strace "$snippet")"
    fi
    # shellcheck disable=SC2086
    te=$(run_timed traced "$snippet")
    t="$t $(echo "$te" | cut -d' ' -f1)"
    e="$e $(echo "$te" | cut -d' ' -f2)"
    i=$((i + 1))
  done
  # shellcheck disable=SC2086
  set -- $p
  local pm mn mx
  pm=$(median5 "$@")
  mn=$(min5 "$@")
  mx=$(max5 "$@")
  local sm sn sx
  if [ "$HAVE_STRACE" = 1 ]; then
    # shellcheck disable=SC2086
    set -- $s
    sm=$(median5 "$@")
    sn=$(min5 "$@")
    sx=$(max5 "$@")
  else
    sm=na
    sn=na
    sx=na
  fi
  # shellcheck disable=SC2086
  set -- $t
  local tm tn tx
  tm=$(median5 "$@")
  tn=$(min5 "$@")
  tx=$(max5 "$@")
  # shellcheck disable=SC2086
  set -- $e
  local em
  em=$(median5 "$@")
  echo "$name|$pm|$mn|$mx|$sm|$sn|$sx|$tm|$tn|$tx|$em"
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
  echo "strace: $(command -v strace >/dev/null && strace --version | head -1 || echo missing)"
  echo "method: 5 runs each, wall clock (date +%s%N), median with min/max; traced runs go through \`snowglobe run\` with identical \`bash -c\` shape (.sgr event counts are medians); strace runs use \`strace -f -qq -o /dev/null\` as the reference tracer; .sgr dirs removed after each run"
  echo
  echo "| workload | untraced med ms (min-max) | strace med ms (min-max) | snowglobe med ms (min-max) | overhead vs plain ms | ratio | events |"
  echo "|---|---|---|---|---|---|---|"
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
    sm=$(echo "$line" | cut -d'|' -f5)
    sn=$(echo "$line" | cut -d'|' -f6)
    sx=$(echo "$line" | cut -d'|' -f7)
    tm=$(echo "$line" | cut -d'|' -f8)
    tn=$(echo "$line" | cut -d'|' -f9)
    tx=$(echo "$line" | cut -d'|' -f10)
    em=$(echo "$line" | cut -d'|' -f11)
    if [ "$sm" = na ]; then
      sms="n/a"
    else
      sms="$(ms1 "$sm") ($(ms1 "$sn")-$(ms1 "$sx"))"
    fi
    ovms=$(ms1 "$((tm - pm))")
    ratio=$(awk "BEGIN{printf \"%.2f\", $tm/$pm}")
    echo "| $name | $(ms1 "$pm") ($(ms1 "$mn")-$(ms1 "$mx")) | $sms | $(ms1 "$tm") ($(ms1 "$tn")-$(ms1 "$tx")) | $ovms | ${ratio}x | $em |"
  done
} > "$OUT"
echo "wrote $OUT"
