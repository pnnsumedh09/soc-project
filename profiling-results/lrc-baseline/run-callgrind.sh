#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/profile-results/models.env"

EXE="$ROOT/build-lrc-callgrind-portable/bin/llama-cli"
RESULTS="$ROOT/profile-results/callgrind/$(date +%Y%m%d-%H%M%S)-$$"

command -v valgrind >/dev/null
command -v callgrind_annotate >/dev/null
test -x "$EXE"

mkdir -p "$RESULTS"
printf '%s\n' "$RESULTS" > "$ROOT/profile-results/latest-callgrind-run.txt"

run_one() {
    local label="$1"
    local model="$2"
    local out="$RESULTS/$label"

    mkdir -p "$out"
    echo "Starting Callgrind: $label"

    valgrind \
      --tool=callgrind \
      --separate-callers=8 \
      --callgrind-out-file="$out/callgrind.out" \
      "$EXE" \
      -m "$model" \
       -t 1 -tb 1 \
      -c 512 -b 128 -ub 128 \
      -fa off \
      --no-warmup --ignore-eos \
      -st -n 10 --seed 1234 --temp 0 \
      -p "Describe a system-on-chip in two sentences." \
      > "$out/generation.txt" 2> "$out/run.log"

    callgrind_annotate \
      --inclusive=yes \
      --tree=both \
      "$out/callgrind.out" \
      > "$out/callgrind-report.txt"

    echo "Completed Callgrind: $label"
}

run_one qwen "$QWEN_MODEL"
run_one lfm350 "$LFM350_MODEL"
run_one lfm700 "$LFM700_MODEL"

echo "Results: $RESULTS"
