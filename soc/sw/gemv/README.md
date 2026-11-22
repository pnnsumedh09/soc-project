# soc/sw/gemv: llama.cpp GEMV offload hook, weight cache, backends (plan tasks P2.2, P2.3)

## What it is

Every decoded token runs 169 matrix-vector products (GEMVs) against Q8_0 weight matrices. A small hook in llama.cpp sends each of them to `soc_gemv_q8_0()`. That function:

1. looks the weight matrix up in a cache keyed by its address (plus shape);
2. on first use, asks the **backend** for engine-readable memory and **repacks** the matrix into it (`soc/sw/repack`);
3. calls the backend's `run()` to compute `y = W x`;
4. falls back to llama.cpp's own `vec_dot` if anything fails.

```
llama.cpp graph ──► ggml_compute_forward_mul_mat (ggml-cpu.c)
                      │ x quantized to Q8_0, barrier
                      │ eligible? (Q8_0 weight tensor, 1 activation column)
                      ▼ thread 0 only
                    soc_gemv_q8_0 ── cache miss ──► backend->alloc + soc_q8r_pack
                      │
                      ▼
                    backend->run(repacked W, x) ──► y      ("emul" now, FPGA in P4.2)
```

Backends implement five functions (`soc_gemv_backend.h`): `init`, `shutdown`, `alloc` (4 KB-aligned memory, CPU pointer + device address), `free`, `run`. The `emul` backend uses heap memory and the golden arithmetic `soc_q8r_gemv_ref`. The FPGA backend (HAL v2) will provide contiguous DMA memory and drive the engine registers. Nothing else changes.

## The hook (upstream edits, kept minimal)

| File | Change |
|---|---|
| `ggml/src/ggml-cpu/ggml-cpu.c` | `#include "soc_gemv.h"`, `ggml_soc_gemv_eligible()`, and an 11-line block right after the `ggml_barrier` in `ggml_compute_forward_mul_mat`; all inside `#ifdef GGML_SOC_GEMV` |
| `ggml/src/ggml-cpu/CMakeLists.txt` | 7 lines: if `GGML_SOC_GEMV` is set, include `soc/sw/gemv/ggml-soc-gemv.cmake` |

Eligibility (identical in every thread, so all take the same branch): not `use_ref`; weight Q8_0, activation F32, output F32; exactly one activation column (`ne11 == ne12 == ne13 == 1`, i.e. decode, or the last-token LM head in prefill); weight not a view, no `extra`, in a buffer marked `GGML_BACKEND_BUFFER_USAGE_WEIGHTS`; `soc_gemv_enabled()`. Thread 0 does the call, other threads return. ggml has a barrier after every graph node, so they wait for it. Prefill (several columns) and everything else take llama.cpp's normal path. The only CPU op fusion in this version (RMS_NORM+MUL) never involves MUL_MAT.

## Build

```bash
# LRC, cross-compile for the Ultra96 (same as Lab 1, plus one flag)
cmake -DCMAKE_TOOLCHAIN_FILE=../ultra96_toolchain.cmake -DBUILD_SHARED_LIBS=OFF -DGGML_SOC_GEMV=ON ..
make -j 8
```

Optional: `-DGGML_SOC_DIR=/path/to/soc` if the `soc/` folder is not at the repo root. Without `-DGGML_SOC_GEMV=ON` the build is stock llama.cpp. **The CMake path has not been run yet** (the Claude sandbox has no cmake; it was tested with `soc/tools/build_nocmake_sandbox.sh`, which compiles the same sources with the same defines). First LRC build: check for the configure message `SoC GEMV offload enabled for ggml-cpu`.

## Runtime switches (environment variables)

| Variable | Effect |
|---|---|
| `SOC_GEMV=0` | turn offload off (stock CPU path) without rebuilding |
| `SOC_GEMV_BACKEND=emul` | backend (only `emul` today) |
| `SOC_GEMV_VERIFY=1` | recompute every offloaded GEMV with llama.cpp's `vec_dot` and report differences (slow) |
| `SOC_GEMV_STATS=1` | print counts, MB streamed, cache size, repack time at exit |
| `SOC_GEMV_PROFILE=path.csv` | write one row per GEMV with tensor ID, shape, path, logical bytes, MACs, cache hit and measured host elapsed time |
| `SOC_GEMV_STRICT=1` | abort instead of falling back on backend errors |

A one-line banner `soc_gemv: enabled (...)` is printed to stderr at first use.

`SOC_GEMV_STATS` now separates weight, activation and output payloads and reports logical MACs, cache hits, CPU fallback time and the minimum/maximum backend call time. These are software measurements around `backend->run()`. They are not accelerator clock cycles or observed AXI beats.

Analyze an optional trace with:

```bash
python3 soc/sw/gemv/profile_trace.py gemv-trace.csv
python3 soc/sw/gemv/profile_trace.py gemv-trace.csv --wall-seconds <measured-decode-interval>
```

The second form reports the measured backend share and the Amdahl speedup limit if backend time became zero. Trace output adds file I/O overhead, so collect competitive end-to-end timing without `SOC_GEMV_PROFILE`, then use a separate trace run to identify shapes and hot tensors. Use identical model, prompt, generated-token count, thread count, CPU affinity and clock settings for every comparison.

The experiment runner performs separate CPU, emulation timing and emulation trace runs. It forwards all arguments after the output directory to `llama-bench`:

```bash
bash soc/sw/gemv/run_profile_experiment.sh \
    ./build/bin/llama-bench /path/to/model.gguf ./profile-results \
    -p 32 -n 32 -r 5 -t 1
```

Set `SOC_GEMV_RUN_FPGA=1` to add strict FPGA timing and trace runs after board smoke passes. The output directory contains one stdout, stderr and wall-time file for each case plus CSV files for trace cases.

## Tests

```bash
# unit test (no llama.cpp needed): cache hits, stale-content detection, fallback, threads, verify
cd soc/sw && gcc -O2 -std=c11 -ffp-contract=off -Irepack -Igemv -o /tmp/test_gemv \
    gemv/test_gemv.c gemv/soc_gemv.c gemv/soc_gemv_backend_emul.c repack/soc_repack.c -lm -lpthread && /tmp/test_gemv
```

End-to-end results (2026-11-18, `soc/docs/results/p2_hook_emul_test.md`): Qwen2.5-0.5B Q8_0 through the hook produces output identical to stock llama.cpp, 169 matrices / 525.2 MB cached, zero fallbacks.

## Numerics

The golden arithmetic is llama.cpp's scalar `ggml_vec_dot_q8_0_q8_0_generic` with a separate FP32 multiply and add. It is bit-exact against that function when llama.cpp is compiled with `-ffp-contract=off` (100% of 26,624 real rows). The default GCC build fuses `sumf += sumi*(a*b)` into one `fmadd`, which changes the last bits (39.5% of rows bit-identical). Against the NEON kernel that runs on the A53 the differences are rounding-level (max diff 7e-7 of max|y|; 20 to 22% of rows bit-identical) because NEON sums in a different order. The hardware must match the golden model (separate multiply and add), not NEON.

## Optional FPGA backend (P4.2 source implemented)

The optional `soc/sw/hal2/` adapter implements the existing backend interface through UIO registers and a noncached u-dma-buf arena. Build selection is `-DGGML_SOC_GEMV_FPGA=ON` in addition to `GGML_SOC_GEMV`; runtime selection remains explicitly `SOC_GEMV_BACKEND=fpga`, with `emul` still the default. This adapter requires confirmed course-image support; it is not board-validated or an established submission dependency. See `../hal2/README.md` for local checks, allocator/coherency contract, failure quarantine and target smoke test. No hook location, cache format or CPU fallback arithmetic changed.
