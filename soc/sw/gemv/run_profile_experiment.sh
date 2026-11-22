#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 ]]; then
    echo "usage: $0 LLAMA_BENCH MODEL OUT_DIR [LLAMA_BENCH_ARGS ...]" >&2
    exit 2
fi

bench=$1
model=$2
out_dir=$3
shift 3
bench_args=("$@")

if [[ ! -x $bench ]]; then
    echo "not executable: $bench" >&2
    exit 2
fi
if [[ ! -r $model ]]; then
    echo "not readable: $model" >&2
    exit 2
fi
if [[ ! -x /usr/bin/time ]]; then
    echo "/usr/bin/time is required" >&2
    exit 2
fi

mkdir -p "$out_dir"
out_dir=$(cd "$out_dir" && pwd)

run_case() {
    local name=$1
    shift
    echo "running $name"
    /usr/bin/time -f 'wall_seconds=%e' -o "$out_dir/$name.wall.txt" \
        env "$@" "$bench" -m "$model" "${bench_args[@]}" \
        >"$out_dir/$name.stdout.txt" 2>"$out_dir/$name.stderr.txt"
}

run_case cpu SOC_GEMV=0 SOC_GEMV_VERIFY=0 SOC_GEMV_PROFILE=
run_case emul SOC_GEMV=1 SOC_GEMV_BACKEND=emul SOC_GEMV_VERIFY=0 SOC_GEMV_PROFILE= SOC_GEMV_STATS=1 SOC_GEMV_STRICT=1
run_case emul_trace SOC_GEMV=1 SOC_GEMV_BACKEND=emul SOC_GEMV_VERIFY=0 SOC_GEMV_STATS=1 SOC_GEMV_STRICT=1 \
    SOC_GEMV_PROFILE="$out_dir/emul_trace.csv"

if [[ ${SOC_GEMV_RUN_FPGA:-0} == 1 ]]; then
    run_case fpga SOC_GEMV=1 SOC_GEMV_BACKEND=fpga SOC_GEMV_VERIFY=0 SOC_GEMV_PROFILE= SOC_GEMV_STATS=1 SOC_GEMV_STRICT=1
    run_case fpga_trace SOC_GEMV=1 SOC_GEMV_BACKEND=fpga SOC_GEMV_VERIFY=0 SOC_GEMV_STATS=1 SOC_GEMV_STRICT=1 \
        SOC_GEMV_PROFILE="$out_dir/fpga_trace.csv"
fi

echo "results: $out_dir"
echo "Use timing runs for speed and *_trace runs only for operation diagnosis."
