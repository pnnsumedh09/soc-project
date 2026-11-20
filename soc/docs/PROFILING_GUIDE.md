# Board Profiling Guide

Step-by-step commands for profiling on the Ultra96. Everything runs on the board at MRD config (1 A53 core, 300 MHz) unless stated otherwise. Cross-compile on LRC, copy binaries to the board.

Estimated time: 2-3 days for one person. Can run in parallel with HLS work on LRC.

---

## Prerequisites

You need:
- Board access (Ultra96-V2) with the course Linux image
- LRC access for cross-compilation
- The course llama.cpp fork checked out
- Models downloaded (see Step 2)

---

## Step 0: Environment setup (LRC, 30 min)

### 0.1 Record the environment

Do this ONCE and save it. Every result file should reference this.

```bash
# On LRC, in your llama.cpp checkout
echo "=== Git ===" > environment.txt
git rev-parse HEAD >> environment.txt
git status --short >> environment.txt
echo "=== Compiler ===" >> environment.txt
module load xilinx/2022
module load gcc/14
aarch64-linux-gnu-gcc --version >> environment.txt
echo "=== CMake ===" >> environment.txt
/usr/bin/cmake --version >> environment.txt
```

### 0.2 Build TWO binaries on LRC

```bash
cd llama.cpp
cp /home/projects/gerstl/ece382m/ultra96_toolchain.cmake .

# --- Build 1: RELEASE (for speed measurements) ---
mkdir build-release && cd build-release
/usr/bin/cmake -DCMAKE_TOOLCHAIN_FILE=../ultra96_toolchain.cmake \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    ..
make -j 8
cd ..

# --- Build 2: RELEASE + our GEMV hook ---
mkdir build-soc && cd build-soc
/usr/bin/cmake -DCMAKE_TOOLCHAIN_FILE=../ultra96_toolchain.cmake \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DGGML_SOC_GEMV=ON \
    ..
# Check for: "SoC GEMV offload enabled for ggml-cpu"
make -j 8
cd ..

# --- Build 3: PROFILE (for perf/gprof, optional) ---
mkdir build-profile && cd build-profile
/usr/bin/cmake -DCMAKE_TOOLCHAIN_FILE=../ultra96_toolchain.cmake \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_C_FLAGS="-O3 -g -DNDEBUG" \
    -DCMAKE_CXX_FLAGS="-O3 -g -DNDEBUG" \
    ..
make -j 8
cd ..
```

### 0.3 Copy binaries to the board

```bash
# From LRC to the board (adjust IP/hostname)
scp build-release/bin/llama-bench board:/root/
scp build-release/bin/llama-cli board:/root/
scp build-release/bin/llama-server board:/root/
scp build-soc/bin/llama-bench board:/root/bench-soc
scp build-soc/bin/llama-cli board:/root/cli-soc
scp build-soc/bin/llama-server board:/root/server-soc
```

---

## Step 1: Board environment (board, 15 min)

### 1.1 Record board state

```bash
# On the board
echo "=== Board ===" > /root/board_env.txt
uname -a >> /root/board_env.txt
cat /proc/cpuinfo | head -20 >> /root/board_env.txt
free -h >> /root/board_env.txt
cat /proc/meminfo | grep -i cma >> /root/board_env.txt
lscpu >> /root/board_env.txt 2>/dev/null
```

### 1.2 Set MRD CPU config

```bash
# Disable cores 1-3
sudo chcpu -d 1-3

# Set 300 MHz
echo userspace | sudo tee /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo 299999 | sudo tee /sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed

# VERIFY (do this before EVERY measurement)
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
# Must show 299999 or 300000
cat /sys/devices/system/cpu/online
# Must show 0
```

### 1.3 Revert (after all profiling)

```bash
sudo chcpu -e 0-3
echo 1199999 | sudo tee /sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed
```

---

## Step 2: Download models (board, 30 min)

Download the candidate models. All are from HuggingFace. If the board has WiFi:

```bash
cd /root   # or wherever you have space

# Qwen2.5-0.5B Q8_0 (course baseline, 676 MB)
wget https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q8_0.gguf

# Qwen2.5-0.5B Q4_0 (~330 MB)
wget https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_0.gguf

# Qwen2.5-0.5B Q4_K_M (~420 MB)
wget https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf

# Record hashes
sha256sum *.gguf > model_hashes.txt
```

For LFM2 and SmolLM2 — check if the course fork supports them first (Step 3 smoke test). If Lab 1 already ran them successfully, they're supported. Download URLs:

```bash
# LFM2-350M (check exact filename on HuggingFace — may need community GGUF)
# SmolLM2-360M (check exact filename)
```

> **Important:** If you used these models in Lab 1, you already have them. Check `/home/projects/gerstl/ece382m/` on LRC or the board's storage for pre-downloaded models.

---

## Step 3: Smoke test (board, 30 min)

Before timing anything, confirm each model loads and generates coherent text.

```bash
# Verify MRD config first
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq

# For each model:
taskset -c 0 ./llama-cli -m qwen2.5-0.5b-instruct-q8_0.gguf \
    -t 1 -n 16 -p "The capital of France is" --no-display-prompt 2>/dev/null

# Check: does it generate text? Is it coherent? Any warnings in stderr?
# Redirect stderr to see warnings:
taskset -c 0 ./llama-cli -m qwen2.5-0.5b-instruct-q8_0.gguf \
    -t 1 -n 16 -p "Hello" 2> smoke_qwen_q8.log

# Record for each model:
# - Does it load? (Y/N)
# - Does it generate text? (Y/N)
# - Any unsupported-op warnings? (copy them)
# - Peak RSS: check with /usr/bin/time -v if available, or:
#   Run the command in background, then check /proc/<pid>/status | grep VmRSS
```

**Kill list:** Remove any model that:
- Fails to load
- Produces empty/garbage output
- Causes swap or OOM (check `dmesg` for oom-killer)
- Has unsupported operations (warnings about unknown ops)

---

## Step 4: Speed benchmarks with llama-bench (board, 2-3 hours)

This is the core measurement. `llama-bench` separates prefill (pp) and decode (tg) and runs multiple repetitions.

```bash
# Verify MRD config
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq

# === Qwen2.5-0.5B Q8_0 ===
taskset -c 0 ./llama-bench \
    -m qwen2.5-0.5b-instruct-q8_0.gguf \
    -t 1 \
    -p 128 -n 128 \
    -r 5 \
    -o csv > bench_qwen_q8_0.csv 2> bench_qwen_q8_0.log

# === Qwen2.5-0.5B Q4_0 ===
taskset -c 0 ./llama-bench \
    -m qwen2.5-0.5b-instruct-q4_0.gguf \
    -t 1 \
    -p 128 -n 128 \
    -r 5 \
    -o csv > bench_qwen_q4_0.csv 2> bench_qwen_q4_0.log

# === Qwen2.5-0.5B Q4_K_M ===
taskset -c 0 ./llama-bench \
    -m qwen2.5-0.5b-instruct-q4_k_m.gguf \
    -t 1 \
    -p 128 -n 128 \
    -r 5 \
    -o csv > bench_qwen_q4km.csv 2> bench_qwen_q4km.log

# === LFM2-350M (if smoke test passed) ===
taskset -c 0 ./llama-bench \
    -m lfm2-350m-*.gguf \
    -t 1 \
    -p 128 -n 128 \
    -r 5 \
    -o csv > bench_lfm2_350m.csv 2> bench_lfm2_350m.log

# === SmolLM2-360M (if smoke test passed) ===
taskset -c 0 ./llama-bench \
    -m smollm2-360m-*.gguf \
    -t 1 \
    -p 128 -n 128 \
    -r 5 \
    -o csv > bench_smollm2.csv 2> bench_smollm2.log
```

### Context sweep (adds ~1 hour)

For the top 2-3 candidates, test at different context depths to see if decode time grows (attention-bound) or stays flat (GEMV-bound):

```bash
# Qwen Q8_0 context sweep
taskset -c 0 ./llama-bench \
    -m qwen2.5-0.5b-instruct-q8_0.gguf \
    -t 1 \
    -p 0 -n 64 \
    -d 0,64,128,256,512 \
    -r 3 \
    -o csv > context_sweep_qwen_q8.csv

# If decode time is nearly constant across -d values: GEMV dominates (expected).
# If it grows: attention/KV traffic matters at longer contexts.
```

---

## Step 5: Accuracy scores (board, 2-4 hours)

Use the official `benchmark.py` to get accuracy scores. This takes the longest because it runs the full evaluation suite.

```bash
# Get the benchmark scripts (on LRC)
cd /home/projects/gerstl/ece382m/
ls llama-bench.tar.gz   # or wherever the benchmark tarball is
# Unpack and copy benchmark.py to the board

# On the board, for each model:
# Start llama-server and run the benchmark

# === Qwen Q8_0 ===
taskset -c 0 ./llama-server -m qwen2.5-0.5b-instruct-q8_0.gguf -t 1 &
sleep 5
python3 benchmark.py --llama-server http://localhost:8080 \
    --model qwen2.5-0.5b-instruct-q8_0.gguf \
    > score_qwen_q8.txt 2>&1
kill %1

# Repeat for each candidate model
# Record: per-task scores (HellaSwag, ARC-Easy, IFEval), combined score, total time
```

> **Note:** Each full benchmark run takes 15-30+ minutes at 300 MHz. Budget time accordingly. You may want to run only the top speed candidates here.

> **Check benchmark.py --help** first to see the exact flags. The command above is a template.

---

## Step 6: Memory measurements (board, 30 min)

For each surviving candidate:

```bash
# Before loading anything
free -h > mem_before.txt
cat /proc/meminfo >> mem_before.txt

# Start llama-server, let it load the model, then check
taskset -c 0 ./llama-server -m qwen2.5-0.5b-instruct-q8_0.gguf -t 1 &
sleep 10
free -h > mem_qwen_q8_loaded.txt
cat /proc/meminfo >> mem_qwen_q8_loaded.txt
cat /proc/$(pidof llama-server)/status | grep -E 'VmRSS|VmPeak|VmSwap' >> mem_qwen_q8_loaded.txt

# Generate a few tokens to warm up KV cache
curl http://localhost:8080/completion -d '{"prompt":"Hello","n_predict":32}'
sleep 2
cat /proc/$(pidof llama-server)/status | grep -E 'VmRSS|VmPeak|VmSwap' >> mem_qwen_q8_loaded.txt

# Check CMA specifically (critical for our FPGA design)
cat /proc/meminfo | grep -i cma >> mem_qwen_q8_loaded.txt

kill %1
```

**Key numbers to extract:**
- Peak RSS per model
- Free memory remaining after model load
- CmaTotal and CmaFree
- Any swap usage (must be 0)
- Major page faults (from `/usr/bin/time -v` if available)

---

## Step 7: Function-level profiling (board, 1-2 hours)

If `perf` is available on the board:

```bash
# Check if perf exists
which perf

# If yes:
taskset -c 0 perf stat -r 3 \
    -e cycles,instructions,cache-references,cache-misses \
    ./llama-cli -m qwen2.5-0.5b-instruct-q8_0.gguf \
    -t 1 -n 32 -p "Describe a system-on-chip." \
    > /dev/null 2> perf_stat_qwen_q8.txt

taskset -c 0 perf record -g \
    ./llama-cli -m qwen2.5-0.5b-instruct-q8_0.gguf \
    -t 1 -n 32 -p "Describe a system-on-chip."

perf report --stdio > perf_report_qwen_q8.txt
```

If `perf` is NOT available, use the gprof build:

```bash
# Copy the -pg build to the board
taskset -c 0 ./llama-cli-profile -m qwen2.5-0.5b-instruct-q8_0.gguf \
    -t 1 -n 32 -p "Describe a system-on-chip."
gprof llama-cli-profile gmon.out > gprof_qwen_q8.txt
```

**What to look for:** The percentage of time in:
- `ggml_vec_dot_q8_0_q8_0` (or the Q4 variant) — this is GEMV
- `ggml_compute_forward_mul_mat` — the outer matmul
- `quantize_row_q8_0` — activation quantization
- `ggml_compute_forward_rms_norm` — normalization
- `ggml_compute_forward_rope` — position encoding
- `ggml_compute_forward_soft_max` — attention softmax
- Everything else

---

## Step 8: Test our GEMV hook (board, 1 hour)

After the stock profiling, test our hook to confirm it works on the real board:

```bash
# Stock vs hook comparison
taskset -c 0 ./llama-cli -m qwen2.5-0.5b-instruct-q8_0.gguf \
    -t 1 -n 48 -p "Describe a system-on-chip in two sentences." \
    --no-display-prompt > stock_out.txt 2> stock_log.txt

taskset -c 0 SOC_GEMV_STATS=1 ./cli-soc -m qwen2.5-0.5b-instruct-q8_0.gguf \
    -t 1 -n 48 -p "Describe a system-on-chip in two sentences." \
    --no-display-prompt > hook_out.txt 2> hook_log.txt

# Output must be identical
diff stock_out.txt hook_out.txt

# Check stats in hook_log.txt: 169 matrices cached, 0 fallbacks

# Run benchmark.py with the hook to verify accuracy is preserved
taskset -c 0 ./server-soc -m qwen2.5-0.5b-instruct-q8_0.gguf -t 1 &
sleep 10
python3 benchmark.py ... > score_hook.txt 2>&1
kill %1
```

---

## Step 9: Collect results (board → laptop, 30 min)

Copy all result files off the board:

```bash
# From your laptop/LRC
scp board:/root/bench_*.csv soc/docs/results/
scp board:/root/score_*.txt soc/docs/results/
scp board:/root/mem_*.txt soc/docs/results/
scp board:/root/perf_*.txt soc/docs/results/
scp board:/root/board_env.txt soc/docs/results/
scp board:/root/environment.txt soc/docs/results/
scp board:/root/model_hashes.txt soc/docs/results/
```

---

## Step 10: Decision summary

Fill in this table from the results:

```
| Model              | Quant  | GGUF MB | Peak RSS | Score | Decode tok/s | Prefill tok/s | Suite min | Swap? | GEMV % |
|--------------------|--------|---------|----------|-------|--------------|---------------|-----------|-------|--------|
| Qwen2.5-0.5B      | Q8_0   | 676     |          |       |              |               |           |       |        |
| Qwen2.5-0.5B      | Q4_0   | ~330    |          |       |              |               |           |       |        |
| Qwen2.5-0.5B      | Q4_K_M | ~420    |          |       |              |               |           |       |        |
| LFM2-350M          | Q8_0   |         |          |       |              |               |           |       |        |
| SmolLM2-360M       | best   |         |          |       |              |               |           |       |        |
```

### Decision rules

1. **Eliminate** any model with: score <= 20 (or borderline with high variance), swap usage, suite time > 25 min, or unsupported ops.
2. **From survivors**, pick the one that gives the best competitive position on the speed-vs-accuracy plot: either highest accuracy at a given speed tier, or highest speed with comfortable accuracy margin.
3. **For hardware:** check what fraction of matmul tensors are Q8_0 in the chosen model. If it's a Q4_K_M model, some tensors will be Q4_K, Q6_K, etc. — our hook only accelerates Q8_0 today.
4. **Confirm** the context sweep shows decode is GEMV-bound (flat), not attention-bound (growing).

### Promotion gate checklist

After profiling, evaluate the four gates from the plan:

- [ ] **M0 (Model):** Selected model scores > 20 with margin, fits in 2 GB, suite < 30 min
- [ ] **D0 (Data path):** CMA has enough space for at least selective repack (check CmaTotal/CmaFree)
- [ ] **B0 (Bandwidth):** Deferred to P1.2 (needs HLS IP on the board)
- [ ] **A0 (ASIC envelope):** Deferred to P1.3 (analytical estimate, can be done from files)

---

## What we already know (don't re-measure)

- Qwen2.5-0.5B tensor census: all 170 matmul tensors are Q8_0, 169 offloadable GEMVs/token, 525 MB/token. (Already in `soc/docs/results/model_dims.md`)
- Lab 1 gprof at 300 MHz / 1 thread: 89% in vec_dot, 25.4M calls. (Reconfirm with the new build, but the ballpark won't change.)
- Our hook produces identical output to stock. (Tested in sandbox; reconfirm on board in Step 8.)
- The emul backend is a correctness model, not a speed model. (Don't draw speed conclusions from it.)
