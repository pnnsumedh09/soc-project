#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/profile-results/models.env"

EXE="$ROOT/build-lrc-gprof/bin/llama-cli"
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
RESULTS="$ROOT/profile-results/gprof/$RUN_ID"

test -x "$EXE"
command -v gprof >/dev/null

for model in "$QWEN_MODEL" "$LFM350_MODEL" "$LFM700_MODEL"; do
    if [[ ! -s "$model" ]]; then
        echo "Missing or empty model: $model" >&2
        exit 1
    fi
done

mkdir -p "$RESULTS"

git -C "$ROOT" rev-parse HEAD > "$RESULTS/commit.txt"
git -C "$ROOT" status --short > "$RESULTS/git-status.txt"
cp "$ROOT/build-lrc-gprof/CMakeCache.txt" "$RESULTS/CMakeCache.txt"
sha256sum "$EXE" > "$RESULTS/executable.sha256"
printf '%s\n' "$RESULTS" > "$ROOT/profile-results/latest-gprof-run.txt"

run_one() {
    local label="$1"
    local model="$2"
    local out="$RESULTS/$label"

    mkdir -p "$out"
    printf '%s\n' "$model" > "$out/model-path.txt"

    echo "Starting $label"
    echo "Log: $out/run.log"

    (
        cd "$out"

        # Ensure gmon.out is written here, even if GMON_OUT_PREFIX
        # was set in the calling environment.
        unset GMON_OUT_PREFIX

        if "$EXE" \
            -m "$model" \
            -ngl 0 \
            -t 1 -tb 1 \
            -c 512 \
            -b 128 -ub 128 \
            -fa off \
            --no-warmup \
            --ignore-eos \
            -st \
            -n 10 \
            --seed 1234 \
            --temp 0 \
            -p "Describe a system-on-chip in two sentences." \
            > generation.txt 2> run.log
        then
            :
        else
            echo "FAILED: $label. Last log lines:" >&2
            tail -n 60 run.log >&2
            exit 1
        fi

        if [[ ! -s gmon.out ]]; then
            echo "No gmon.out for $label. Check profiling flags and normal exit." >&2
            exit 1
        fi

        gprof "$EXE" gmon.out > gprof-full.txt
        gprof -p "$EXE" gmon.out > gprof-flat.txt
        gprof -q "$EXE" gmon.out > gprof-callgraph.txt
    )

    echo "Completed $label"
}

run_one qwen "$QWEN_MODEL"
run_one lfm350 "$LFM350_MODEL"
run_one lfm700 "$LFM700_MODEL"

echo "All profiles saved under: $RESULTS"
