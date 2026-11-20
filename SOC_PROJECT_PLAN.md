# ECE382M.20 SoC Project: Edge LLM Inference SoC, GEMV Acceleration Plan

Living document. Read section 0 first. Update sections 8 (status), 13 (decisions) and 14 (session log) at the end of every working session.

Last updated: 2026-11-20 (session 20). Project due: **2026-12-08**.

---

## 0. How to use this file (every session, human or Claude)

1. Read this file top to bottom (about 10 minutes). Then read `FPGA_OFFLOAD_DESIGN.md` (earlier design draft) and skim `soc/docs/reference/` as needed.
2. Pick tasks from section 8 whose status is `TODO` and whose dependencies are `DONE`. Put your name and the date in the Owner column when you start.
3. Mark things you did not measure as `[EST]` and things not yet checked as `[VERIFY]`. Never present an estimate as a measurement. Measured numbers go in section 4 or 5 with the source (file, date, who).
4. At the end of a session: update statuses, add decisions to section 13, and append to the session log (section 14). Commit to the project branch (see 3.3).
5. Claude sessions cannot reach the LRC servers, Vivado or the Ultra96 board. They write code, scripts and docs, and review results. Team members run anything that needs the LRC or the board and paste results back (log files, report excerpts) into `soc/docs/results/`.
6. This clone of llama.cpp is a private class-project copy. Do **not** open PRs against ggml-org/llama.cpp (their AGENTS.md rules apply only to upstream PRs). Do not edit upstream files more than necessary; keep project code under `soc/` and keep upstream-file edits small and listed in section 3.2.

---

## 1. Project summary

Course project: HW/SW co-design of a low-power, real-time edge SoC that runs LLM inference (llama.cpp) on an ARM Cortex-A53 plus custom accelerator(s). Prototype on the Ultra96 (Zynq UltraScale+ ZU3EG, ARM + FPGA). Target ASIC: 45 nm or 32/28 nm, single A53 at 300 MHz plus accelerators.

### 1.1 MRD (Market Requirements Document)

| Metric | Requirement |
|---|---|
| Decode speed | >= 0.5 token/s |
| Benchmark accuracy score | > 20.0 (ARC-easy, Hellaswag, IFEval mix from the Lab 1 `benchmark.py`) |
| Accelerator area | 0.5 mm^2 (ASIC estimate) |
| Additional accelerator power | < 100 mW |
| Target CPU | single A53 at 300 MHz |

### 1.2 Required activities (from the project handout)

PRD; model profiling and selection; software bottleneck profiling; HW/SW partitioning; verification against spec; RTL synthesis of the accelerator; FPGA prototype; SW/HW communication; co-verification; high-level performance and power model; timing/area/power estimate and validation against the MRD. The accelerated function does **not** have to be the dot product.

### 1.3 Submission and grading

- Due 2026-12-08. Slides on Canvas, code on Classroom 50, plus an individual peer-contribution report on Canvas (rate teammates across all labs and the project).
- **Competitive autograder**: scores speed and task accuracy against other teams using the Lab 1 benchmark scripts. Commit to `main` of the Classroom 50 assignment to be graded (other branches are not graded). The design must finish all benchmarks within 30 minutes. Scores are published once a day.
- Repo must contain: modified llama.cpp, any HLS/RTL, the FPGA bitstream (`*.bit`) and hardware handoff (`*.hwh`), and a top-level `submission.json` that points at the chosen `*.gguf` and the `*.bit`/`*.hwh`.
  - Example: `/home/projects/gerstl/ece382m/project/submission.json` (LRC).
  - Generator: `/home/projects/gerstl/ece382m/project/make_submission.py`.
  - Schema: `/home/projects/gerstl/ece382m/project/submission.schema.json`.
- The handout also links a PRD example page and an "Ultra96 development board tutorial". **Read the board tutorial** (not in this repo yet; see task P0.5): it likely decides how the bitstream is loaded (PYNQ overlay is suspected).
- Design reviews and final presentations happen in class; dates unknown `[VERIFY]`.

---

## 2. Team

Team 22 (Lab 1 and 2 reports; the Lab 3 repo was named group-18, so check which name Classroom 50 uses for the project `[VERIFY]`): Sumedh Peyyeti, Shreyas Sridhara, Nirupama Govindarajan.

Owners are not yet assigned. Suggested split (edit freely):

| Area | Suggested owner |
|---|---|
| Board runs, baselines, benchmark runs, model sweep | TBD |
| HLS/Vivado, bitstream, board bring-up | TBD |
| llama.cpp hook, repack tool, HAL/driver | TBD (Claude sessions can draft all of it) |
| PRD, perf/power/area model, slides, report | TBD |

---

## 3. Repository and file map

### 3.1 Locations

| Thing | Location |
|---|---|
| Workspace root (Mac) | `/Users/sumedh/UT/SOC_Design/project/` |
| Main project repo (this file lives here) | `/Users/sumedh/UT/SOC_Design/project/llama.cpp/` (upstream llama.cpp at commit `7592b61d3`, 2026-11-10) |
| Project branch | `soc-gemv-accel` (created 2026-11-18 from `master`) |
| Qwen model (Q8_0, 676 MB) | `/Users/sumedh/UT/SOC_Design/project/qwen2.5-0.5b-instruct-q8_0.gguf` |
| Original Lab 3 repo copy | `/Users/sumedh/UT/SOC_Design/project/ece382m-20-fa26-lab-3-group-18-main/` (read-only reference) |
| Lab reports and handouts (PDF) | `/Users/sumedh/UT/SOC_Design/project/*.pdf` |
| Same material as text | `soc/docs/reference/*.txt` (inside the repo, for sessions that cannot read PDFs) |
| LRC scratch | `/misc/scratch/<user>/` (not backed up) |
| Class resources on LRC | `/home/projects/gerstl/ece382m/` (toolchain file, benchmark tarball, linux-xlnx, qemu-boot, submission helpers) |

### 3.2 Layout inside `llama.cpp/`

```
SOC_PROJECT_PLAN.md          this file
FPGA_OFFLOAD_DESIGN.md       earlier design draft (Option A); superseded by section 6 where they differ
soc/
  docs/
    reference/               text copies of lab handouts, lab reports, project handout
    results/                 measurements (logs, tables) go here, one file per experiment, dated
  lab3_ref/                  Lab 3 files copied UNCHANGED, kept as reference. Do not edit.
    LAB3_README.md
    part3/                   SystemC dot-product accelerator model + QEMU/SystemC platform (dot_accel.h, zynqmp_dot.cc, ...)
    part4/                   Lab 3 driver, HAL, GGML hook (vec_dot level) + llama-cpu.patch
  tools/
    dump_q8_tensor.py        dump Q8_0 tensors (or N rows) from the GGUF to .bin test files (read-only on the GGUF)
    build_nocmake_sandbox.sh build llama-simple (stock + hook) without cmake; Claude sandbox only
  sw/
    repack/                  Q8_0 repack library + golden arithmetic + tests (P2.1, done)
    gemv/                    soc_gemv: hook entry point, weight cache, backend interface, emul backend,
                             CMake fragment, unit test (P2.2/P2.3, done; see its README)
  (to be created by tasks)
  sw/hal2/                   FPGA backend (HAL v2) implementing soc_gemv_backend (P4.2)
  hw/                        HLS sources, testbenches, Vivado scripts, bitstream outputs
```

Upstream files touched (keep this list current, keep edits minimal; all changes inside `#ifdef GGML_SOC_GEMV` / `if (GGML_SOC_GEMV)`):
- `ggml/src/ggml-cpu/ggml-cpu.c`: include, `ggml_soc_gemv_eligible()`, hook block after the barrier in `ggml_compute_forward_mul_mat` (see 6.6).
- `ggml/src/ggml-cpu/CMakeLists.txt`: include `soc/sw/gemv/ggml-soc-gemv.cmake` when `-DGGML_SOC_GEMV=ON`.

Sandbox note for Claude sessions: the sandbox is **aarch64** with gcc/g++ and `aarch64-linux-gnu-gcc` but **no cmake**. Builds with `-mcpu=cortex-a53` run natively and use the same NEON code as the Ultra96 (timing differs). Use `soc/tools/build_nocmake_sandbox.sh`.

### 3.3 Git

- Work on branch `soc-gemv-accel`. Commit small and often. The remote `origin` points at upstream ggml-org: **do not push there**. The submission goes to Classroom 50 (add that remote when the assignment repo is available: `git remote add classroom <url>`).
- Claude sessions do not commit on their own. They leave changes in the working tree and say what to commit.
- Git commands may fail from a sandboxed session on the mounted folder (lock file permissions). If so, run git from your own terminal.

---

## 4. What the labs established (measured results)

All numbers below are from the team's lab reports (copies in `soc/docs/reference/`).

### 4.1 Lab 1: benchmarking and profiling (Qwen2.5-0.5B Q8_0 unless noted)

Important caveat: the speed numbers below were taken at the board's default clock (about 1.2 GHz) with the default thread count unless stated. They are **not** the MRD configuration (1 core at 300 MHz). Re-baselining is task P0.2.

| Experiment | tok/s | Score (/100) |
|---|---|---|
| Qwen2.5-0.5B Q8_0, default (4 threads) | 5.81 | 51.4 |
| Same, Cortex-A53 flags, 1 thread, NEON | 1.72 | 51.4 |
| Same, 4 threads, A53 flags | 5.83 | 51.4 |
| Same, NEON disabled, 4 threads | 1.47 | 52.3 |
| Qwen2.5-0.5B Q4_K_M | 5.14 | 54.3 |
| Qwen2.5-0.5B Q2_K | 5.61 | 48.1 |
| Qwen1.5-0.5B (MHA) | 5.98 | 25.7 |
| LFM2-350M | 8.22 | 48.4 |
| LFM2-700M | 4.34 | 57.9 |
| LFM2-1.2B | 2.89 | 64.5 |
| SmolLM2-360M | 6.96 | 22.8 |
| TinyLlama-1.1B | 3.05 | 11.6 |
| Llama-3.2-1B | 1.71 | 59.8 |

Findings: decode is memory/compute bound and scales roughly inversely with parameter count. K-quants were slower than Q8_0 on the CPU because of unpacking cost on the A53, not because of memory. A large vocabulary costs speed (Llama-3.2 128k vs TinyLlama 32k).

Profile (gprof, 1 core at 300 MHz, short prompt, `-t 1`): total 145.12 s. `ggml_vec_dot_q8_0_q8_0` = 89% (about 130 s) over 25.4 M calls. Everything else about 10% (graph dispatch/attention 2.2%, tokenization/setup 2.8%, other 5.2%).

### 4.2 Lab 2: HLS dot product (Vitis HLS, target xczu3eg-sbva484-1-e)

- Function `dot_q8_0(nb, xq, xd, yq, yd)`: int8 MAC per 32-element block, scaled by FP16 x FP16 scales (converted to FP32 with denormal renormalization), accumulated in FP32 in block order. Max N = 4864 (152 blocks).
- 51-test testbench, bit-exact vs reference after the denormal fix (Vitis FP IP flushes denormals to zero; 13/51 failed before the fix).
- Best design: array reshape factor 8, BLOCK_LOOP II = 4, 21 DSP, about 2.3k LUT, about 1.3k FF. Latency 130 cycles (896 elements), 626 cycles (4864). Best clock 9 ns target (111 MHz): 1.18 us / 5.64 us.
- II was limited by BRAM ports (2 per array), not by compute. Below 9 ns target the tool raised II and hurt total time.
- Fixed-point attempt: needed 76-bit inputs and 102-bit accumulator for < 10% error; used 32 DSP / 4.2k LUT (worse than float), 5.6% max relative error. Conclusion: stay with float accumulation unless a narrow-range scheme is found.

### 4.3 Lab 3: SystemC/QEMU integration of the dot product

- SystemC TLM model `DotAccel` (timing `1 + 4*nb + 17` cycles at 10 ns, 4 scratchpads about 10.3 KB, about 6 BRAM), a pure bus slave at `0xA0100000` (AXI HPM0 FPD), IRQ on `pl2ps_irq[5]`.
- Linux misc-device driver (`/dev/dot_accel`, mutex, ioctl RUN, poll or IRQ), user-space HAL (de-interleaves Q8_0 blocks), GGML hook at the `vec_dot` level (`lab3_vec_dot_q8_0_q8_0`).
- Verified on TinyStories (`-n 10`) against a CPU reference build. Lab 3 builds required `GGML_CPU_REPACK=OFF`, `GGML_LLAMAFILE=OFF`, `GGML_NATIVE=OFF`, `GGML_BLAS=OFF`, `GGML_OPENMP=OFF`, `GGML_CPU_KLEIDIAI=OFF`.
- Limitation that drives this project's design: one dot product per call means about 476 MMIO writes (about 1.9 KB) per 896-element row, and x and the weights are re-sent every row. See section 6.1.

---

## 5. Technical facts for the design

### 5.1 Qwen2.5-0.5B shape (verified with gguf-py on our GGUF, 2026-11-18; details in `soc/docs/results/model_dims.md`)

24 layers, d_model 896, 14 Q heads, 2 KV heads (GQA), head_dim 64, FFN 4864 (SwiGLU), vocab 151,936. **Embeddings are not tied in this GGUF**: `output.weight` (LM head) and `token_embd.weight` are separate 144.6 MB Q8_0 tensors; `token_embd` is only used for a one-row lookup. All matmul weights are Q8_0; biases (Q/K/V) and norms are F32. All `in` dimensions are multiples of 32 (896 = 28 x 32; 4864 = 152 x 32).

### 5.2 GEMVs per decoded token (`y = W x`, W is out x in); shapes verified against the GGUF

| Op | out | in | rows |
|---|---|---|---|
| Q proj (+bias) | 896 | 896 | 896 |
| K proj (+bias) | 128 | 896 | 128 |
| V proj (+bias) | 128 | 896 | 128 |
| O proj | 896 | 896 | 896 |
| FFN gate | 4864 | 896 | 4864 |
| FFN up | 4864 | 896 | 4864 |
| FFN down | 896 | 4864 | 896 |
| LM head (once per token) | 151,936 | 896 | 151,936 |

- Rows per layer 12,672; per token 12,672 x 24 + 151,936 = **456,064 rows** (each row is one `vec_dot` call). Lab 1's 25.4 M calls is about 56 token passes (prompt plus generated tokens), consistent with the 145 s run. `[EST]`
- Calls per token as GEMVs: 169 unfused; with QKV fusion (3→1, save 2/layer) and gate+up fusion (2→1, save 1/layer): 4 calls/layer × 24 + 1 LM head = **97**.
- Weights: Q8_0 uses 34 bytes per 32 weights (8.5 bits/weight). Exact matmul bytes read per decoded token from the GGUF: **524,833,792 B (0.525 GB)** (the 676 MB file also holds `token_embd`, which is only used for a one-row lookup). The LM head (`output.weight`, 144.6 MB) is about 27.6% of the bytes read per token. Every weight is read exactly once per token, so decode has no weight reuse.

### 5.3 Platform facts

- Ultra96: 2 GB DRAM shared by CPU and PL; addressable range for CPU+PL 0x0 to 0x7FFFFFFF; 256 KB on-chip memory (OCM) at 0xFFFC0000. Part: ZU3EG: 216 BRAM36 (7.6 Mb), about 360 DSP, about 70k LUT `[VERIFY resource counts in the datasheet]`.
- PS-PL ports (UG1085 `[VERIFY]`): `M_AXI_HPM0/1_FPD` (CPU master to PL registers, used by Lab 3), `S_AXI_HP0..3_FPD` (PL master to DDR, up to 128-bit, non-coherent), `S_AXI_HPC0/1_FPD` (coherent).
- Board CPU config for MRD emulation (resets on reboot): `echo 299999 | sudo tee /sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed; sudo chcpu -d 1-3`. Revert: `echo 1199999 | sudo tee ...; sudo chcpu -e 0-3`. Always `sudo halt` before unplugging.

---

## 6. Primary candidate: bus-mastering Q8_0 GEMV engine

This is the leading candidate, not a final architecture. The software hook, repack library, backend interface, and emulation backend are reusable regardless of what the board measurements show. The design is promoted to "chosen architecture" only after these gates pass:

| Gate | Required evidence | Task |
|---|---|---|
| M0: Model | Selected model clears accuracy > 20 (with margin), fits in 2 GB without swap, and completes the suite < 30 min at 1 A53 / 300 MHz | P0.2, P0.3 |
| D0: Data path | Repacked weights (or an alternative layout) can be allocated DMA-visible with llama-server running; or selective repack of the highest-value matrices fits | P0.6 |
| B0: Bandwidth | Measured PL-to-DDR bandwidth supports a useful end-to-end speedup over CPU-only decode | P1.2 |
| A0: ASIC envelope | Proposed lane count, SRAM, FP units, AXI engine, and controller plausibly fit 0.5 mm² / 100 mW at the chosen process node | P1.3 |

### 6.1 Why not the Lab 3 structure

Lab 3 offloads one `vec_dot` row per call over a slave-only MMIO interface. Per 896-element row: a syscall, a kernel copy, about 476 uncached 32-bit writes, a start/poll. The CPU does the same row in about 5 us on average at 300 MHz (130 s / 25.4 M), while the Lab 2 datapath needs about 1.2 us. If an uncached MMIO write costs 0.2 to 1 us `[EST]`, the transfer alone is 100 to 500 us per row. Lab 3 is correct but would be far slower than the CPU on the real board. x is also resent for each row and the weights for every call.

### 6.2 The idea

Offload whole matrix-vector products. The engine is an AXI master that streams the weights from DDR itself. The CPU sends a short descriptor and gets back a result vector.

```
CPU (A53, llama.cpp)                       PL (FPGA)
 ├ RMSNorm, RoPE, attention, SiLU*up        ┌─────────────────────────────────┐
 ├ quantize x -> Q8_0 buffer (DDR)          │ AXI-Lite regs <- descriptor      │
 ├ write descriptor, START ───────────────► │ AXI master (HP port) <- weights  │
 ├ wait (poll or IRQ)                       │ x BRAM -> N MAC lanes            │
 └ read y from DDR <────────────────────────│ block sum * scales -> FP32 acc   │
                                            │ y[i] -> DDR (write channel)      │
                                            └─────────────────────────────────┘
```

### 6.3 Data layout (repack at load time)

Q8_0 native block is 34 B (2 B FP16 scale + 32 int8), which does not align to 16 B bus beats. Repack each offloaded weight matrix into two arrays:

- `quants`: `rows x (in/32) x 32 B` contiguous int8 (2 beats per block).
- `scales`: `rows x (in/32) x 2 B` contiguous FP16.

Rules: 4 KB-aligned base addresses (AXI bursts must not cross 4 KB), physically contiguous per matrix (largest is the LM head `output.weight`, 144.6 MB), kept only as the repacked copy (the board has 2 GB). Only the 170 Q8_0 matmul tensors need repacking; `token_embd.weight` stays in the mmapped GGUF for the one-row lookup. Repack lazily on the first call per weight tensor (keyed by the tensor data pointer + shape). The stale-content check fingerprints only the first and last Q8_0 block (68 bytes per cache hit; 169 hits/token = ~11.5 KB, negligible vs 525 MB of weight reads). Measure first-token latency on the board (all 169 repacks happen on the first decode token); consider eager repack at model load if startup time matters for the autograder.

**Memory budget (2 GB total):** The GGUF is mmap'd, so weight pages are demand-loaded and can be evicted by the kernel after repack. Steady-state working set estimate:

| Component | MB |
|---|---|
| Linux kernel + OS | ~100 |
| Repacked weights (CMA/reserved) | 525 |
| Non-weight GGUF data still resident (token_embd for GET_ROWS, norms, biases) | ~80 |
| KV cache (Qwen 0.5B, short context) | ~50 |
| llama-server, activations, scratch | ~100 |
| **Total** | **~855** |

This fits in 2 GB with ~1.1 GB margin for OS/filesystem caches. The risk is CMA configuration: the default reservation on many Zynq images is 128–256 MB, insufficient for 525 MB. Task P0.6 checks this. If CMA is too small, the cache marks entries `CPU_ONLY` on allocation failure and falls back gracefully. Priority order for selective repack: LM head (145 MB, 27.6% of traffic), FFN down projections, FFN gate/up, then attention projections.

**Fallback layouts if full repack doesn't fit:**

| Layout | Advantage | Cost |
|---|---|---|
| Selective repack (largest matrices only) | Fits smaller CMA; graceful CPU fallback for the rest | Mixed CPU/FPGA execution |
| Native 34-byte Q8_0 streaming | No duplicate memory | Hardware alignment/unpacking logic, slightly lower burst efficiency |
| Per-tensor DMA alloc at model load | DMA-safe without long-lived second copy | Loader changes, fragmentation risk |

Alternative layout `[VERIFY in P3]`: group 8 blocks' scales with their quants (16 B scales + 256 B quants = 17 beats) to get a single stream.

### 6.4 Engine microarchitecture (proposed)

1. Read the x vector once per GEMV into on-chip BRAM (x quants up to 4864 B, x scales up to 304 B). Proposed: the CPU leaves x in ggml's native 34-byte blocks; the engine de-interleaves while loading (it is a tiny one-time read).
2. Stream weights: INCR bursts up to 256 beats of 128 bit, several outstanding, credit-based FIFO between the read engine and the lanes (issue a burst only when the FIFO has space; never stall the R channel).
3. Lane: 16 int8 x int8 multiplies per beat (DSP48 or LUT multipliers), adder tree, int32 block sum after 2 beats; multiply by `(scale_w * scale_x)` in FP32; accumulate per row in FP32. Reuse Lab 2's integer-based FP16 -> FP32 conversion (denormal-safe).
4. Hide the FP32 accumulate loop-carried latency by interleaving rows or using several accumulators (II target = 2 beats per block).
5. At the end of each row write `y[i]` (FP32) to the DDR output buffer; continue with the next row (rows are contiguous). Optional bias add (Q/K/V have biases).
6. Target PL clock: 150 MHz first (conservative for HLS with multiply-accumulate trees on the ZU3EG), 200 MHz as a stretch goal. Compare total job latency at different clocks, not frequency alone — a lower clock can improve HLS II or routing. Scale up only if bandwidth allows: more lanes (rows in parallel, shared x) or a second HP port.

Numerics: block order matches the Lab 2 order (sequential over blocks within a row), which is also the order of `ggml_vec_dot_q8_0_q8_0_generic`. The golden model is `soc_q8r_row_dot` in `soc/sw/repack/` (built with `-ffp-contract=off`); to be bit-exact against it the hardware must do a separate FP32 multiply `(float)sumi * (w_d * x_d)` and a separate FP32 add (no fused multiply-add), or we accept a small, measured difference. llama.cpp's NEON kernel sums in a different order, so hardware and NEON will differ in the last float bits. Acceptance is by benchmark score and a relative-error bound, not bit-exactness. Keep the emulation model (P2.3) in the hardware's order so HW vs emulation can be compared bit-exactly.

### 6.5 Control interface

Proposed AXI-Lite register map (draft, change in `soc/hw/regs.h` when implemented): CTRL (start, clear, irq_en), STATUS (busy, done, err), ID, CYCLES, CALLS, then `W_ADDR` (64-bit), `S_ADDR`, `X_ADDR`, `Y_ADDR`, `BIAS_ADDR`, `ROWS`, `NB` (blocks per row), `FLAGS`. In Vitis HLS this maps to `m_axi ... offset=slave` plus `s_axilite`. Later: a descriptor table in DDR for chained calls (fusion).

### 6.6 Software side

- Hook location (verified in source 2026-11-18): `ggml_compute_forward_mul_mat()` in `ggml/src/ggml-cpu/ggml-cpu.c` (about line 1264). Order inside it: (1) Hadamard hint, (2) `ggml_compute_forward_mul_mat_tiled` (only for batch >= 8 and K-quants, so decode never takes it), (3) first `llamafile_sgemm` attempt (F32 src1), (4) src1 quantized to Q8_0 blocks into `params->wdata` by `from_float` (`quantize_row_q8_0`), (5) `ggml_barrier`, (6) second `llamafile_sgemm` attempt, (7) the `vec_dot` chunk loop. **Our hook goes right after (5)**: x is then already in native Q8_0 blocks in `wdata`. Conditions: `src0->type == Q8_0`, `src1` F32, `ne11 == 1`, `ne12 == ne13 == 1`, contiguous rows. Everything else (prefill, other types) falls through unchanged.
- Threads: thread 0 runs the engine call; the other threads return immediately for that op. This must work for any `-t`, because the autograder's `benchmark.py` launches `llama-server` with its own flags (Q1). Changes must live in the ggml library so both `llama-cli` and `llama-server` get them.
- llama.cpp's own CPU features do not interfere on the A53 (verified in source): the Q8_0 CPU "repack" buffer type is only chosen at runtime when the CPU has `dotprod` or `i8mm` (the A53 is ARMv8.0 and has neither), and llamafile's Q8_0 ARM kernel (`tinyBLAS_Q0_ARM`) is compiled only with `__ARM_FEATURE_DOTPROD`. So on the board, decode GEMVs reach the `vec_dot` loop with weights in the native GGUF layout. Still build with `GGML_CPU_REPACK=OFF` and `GGML_LLAMAFILE=OFF` while bringing up the hook (as in Lab 3), then re-test with defaults.
- Implemented (P2.2/P2.3, `soc/sw/gemv/README.md`): the hook calls `soc_gemv_q8_0()`, which owns the weight cache (repack on first use via the backend's `alloc`, keyed by weight pointer + shape, stale-content check), calls `backend->run()`, and falls back to llama.cpp's `vec_dot` on any failure. Backends implement `soc_gemv_backend` (init/shutdown/alloc/free/run); today only `emul`. Runtime switches: `SOC_GEMV=0` (off), `SOC_GEMV_BACKEND`, `SOC_GEMV_VERIFY=1`, `SOC_GEMV_STATS=1`, `SOC_GEMV_STRICT=1`.
- Coherency: the HP port is non-coherent. Weights are written once at load (flush once). x and y buffers: allocate non-cacheable, or flush/invalidate around each call (decide in P4.2), or use the HPC port.
- HLS data streams: the separate quants and scales arrays create two AXI read streams. Confirm in P3.1 whether HLS generates separate `m_axi` bundles or serializes accesses on one port. Measure whether two streams reduce burst efficiency or add interconnect contention. A grouped layout (interleaved scales + quants per block group) is an alternative that simplifies to one stream.
- HAL v2: user-space register access if the autograder cannot load our kernel module (depends on P0.5). Lab 3's driver/HAL (in `soc/lab3_ref/part4`) is the starting point for the register, ioctl and error-handling patterns.
- Contiguous memory: check CMA / reserved memory size on the board image (task P0.6). Total about 525 MB, largest single matrix about 145 MB.

### 6.7 Expected performance `[EST, all to be validated on the board]`

**Caution:** CPU frequency scaling is not linear for a memory-bound workload (DDR speed is independent of A53 clock, and single-core memory behavior differs from multi-core). The 1.72 tok/s Lab 1 figure was at the board's default clock (~1.2 GHz), 1 thread. Dividing by 4 gives ~0.43 tok/s as a rough lower bound, but the actual 300 MHz baseline must be measured (P0.2).

**Bandwidth scenarios for the GEMV portion** (525 MB of weight data per token):

| Effective PL read BW | Weight stream time | GEMV-only tok/s |
|---|---|---|
| 0.5 GB/s | 1.05 s | ~0.95 |
| 1.0 GB/s | 0.525 s | ~1.9 |
| 2.0 GB/s | 0.263 s | ~3.8 |
| 3.2 GB/s | 0.164 s | ~6.1 |

**Per-token model:**
```
T_token = T_CPU_remaining + sum(T_hardware_jobs) + T_contention
T_job   = T_submit + T_cache_sync + max(T_read, T_compute) + T_writeback + T_completion
```

The CPU keeps attention, norms, activation quantization, sampling (~10% of current runtime; measure in P2.4). Per-call overhead (register writes, poll/IRQ) is multiplied by 97–169 calls per token; measure in P1.2. Report a range until board data is available, not a single predicted token rate.

- **Area/power:** Needs an early ASIC envelope (P1.3) covering integer MACs, FP16→FP32 conversion, FP32 accumulator, AXI read/write engines, FIFOs, control logic, activation SRAM, and scale storage. Do not use a fixed LUT/DSP-to-mm² multiplier. See P1.3 for methodology.

---

## 7. Open questions (answer and move to section 13)

| # | Question | Who/how |
|---|---|---|
| Q1 | Does the autograder run at 300 MHz on 1 core or at the board default clock? Which thread count? | Ask on Ed / TA |
| Q2 | How does the autograder load the bitstream (PYNQ overlay with `.bit` + `.hwh`)? Can a kernel module be loaded, or must access be user-space (UIO, /dev/mem, pynq buffers)? | Board tutorial, `submission.schema.json`, Ed |
| Q3 | CMA/reserved memory available on the board image? | P0.6 |
| Q4 | Real sustained PL read bandwidth from DDR on the board? | P1.2 |
| Q5 | Does the benchmark speed metric count prefill or decode only? How long are the prompts? | Read `benchmark.py` (LRC tarball) |
| Q6 | Is "additional system power for accelerators" meant to include DDR traffic? | TA / PRD discussion |
| Q7 | Design review and presentation dates? | Syllabus / Canvas |
| Q8 | Which model do we submit (Qwen2.5-0.5B Q8_0, a smaller model, a lower-bit quantization)? | P0.3 results |
| Q9 | Are llama.cpp default features (REPACK, LLAMAFILE) on or off in the autograder build, and does our hook coexist with them? | Partly answered from source (6.6): neither activates for Q8_0 on the A53. Confirm on the board in P2.2. |

---

## 8. Task list and status

Status: `TODO`, `DOING`, `DONE`, `BLOCKED`. Owner: name + date when started. "C" = can be done by a Claude session from the files. "B" = needs the board or LRC (team member).

### Phase 0: Setup and baselines (target Nov 18 to 20)

| ID | Task | Type | Status | Owner | Acceptance / output |
|---|---|---|---|---|---|
| P0.0 | Create plan file, branch `soc-gemv-accel`, copy Lab 3 files into `soc/lab3_ref/`, save reference text | C | DONE | Claude, 2026-11-18 | This file; `soc/lab3_ref/`; `soc/docs/reference/` |
| P0.1 | Add Classroom 50 remote and a `.gitignore` for build products (`build*/`, `soc/hw/vivado/project/`, `soc/hw/hls_work/`) but explicitly unignore the output dir (`!soc/hw/out/`, `!soc/hw/out/*.bit`, `!soc/hw/out/*.hwh`) so the submission bitstream is committed | C/B | TODO | | Remote added; ignore rules committed |
| P0.2 | Re-baseline Qwen2.5-0.5B Q8_0 at MRD config (1 core, 300 MHz, `-t 1`): decode tok/s, prefill tok/s, benchmark score. Use the same build flags we will ship (state them). Also record the default-flag CPU build. | B | TODO | | `soc/docs/results/baseline_300mhz.md` |
| P0.3 | Model/quant sweep at 300 MHz (Qwen2.5-0.5B Q8_0/Q4_0/Q4_K_M/Q2_K, LFM2-350M, SmolLM2-360M, others). Table of tok/s and score. Check each loads in 2 GB. | B | TODO | | `soc/docs/results/model_sweep_300mhz.md`; shortlist for Q8 |
| P0.4 | `gguf-dump` (or `gguf-py`) of the chosen model: confirm layer shapes, tied embeddings, biases, quant type of every tensor (some tensors may not be Q8_0). Update section 5. | C | DONE | Claude, 2026-11-18 | `soc/docs/results/model_dims.md`. Result: embeddings NOT tied, all matmuls Q8_0, 524.8 MB read per token |
| P0.5 | Read the Ultra96 board tutorial and `submission.schema.json`; answer Q2 (bitstream loading, kernel module allowed?). Fetch `benchmark.py` and answer Q5. | B/C | TODO | | Notes in `soc/docs/results/autograder_flow.md` |
| P0.6 | On the board: `grep -i cma /proc/meminfo`, device-tree reserved memory, free memory with llama-server running; test allocating the actual per-matrix size sequence (not just one block), verify physical/DMA addresses. Answer Q3. Reboot and repeat for fragmentation. | B | TODO | | `soc/docs/results/board_memory.md` |
| P0.7 | PRD v0.1: target user/task, essential/desirable/optional requirements (R1–R8 from MRD + traceability to test/result files), model/context constraints, supported accelerator ops and CPU fallback, board/software assumptions, build-vs-reuse decisions, known limitations. Update at every gate. | C | DONE (initial draft) | Codex, 2026-11-19 | `soc/docs/PRD.md`: requirement-to-evidence mapping, provisional partition/interfaces, memory/numerics contract, promotion gates and verification policy. Update with measured results and confirmed course assumptions; final acceptance remains P6.1. |

### Phase 1: Bandwidth feasibility (Nov 19 to 22)

| ID | Task | Type | Status | Owner | Acceptance / output |
|---|---|---|---|---|---|
| P1.1 | Write a trivial HLS read-only AXI master (streams N bytes from an address, counts cycles, optional checksum) plus a Vivado block-design Tcl (Zynq PS, HP0 port, AXI-Lite) | C | DONE (HLS verified; monitor pass reported) | Codex, 2026-11-19 | `soc/hw/bw_test/`: HLS kernel, native/HLS testbench, RTL read-window counter and testbench, HLS/Vivado scripts, optional PYNQ runner, README. Native functional tests pass. LRC HLS synthesis reports reviewed 2026-11-20: read II=1, 128-bit AXI, burst inference, 2,450 LUT/1,455 FF. LRC export log now confirms C simulation, synthesis, Verilog RTL co-simulation (60 transactions) and IP export passed. Sumedh confirms standalone monitor simulation passes (raw log not supplied). Full-system Vivado integration/implementation remains pending. Requires a verified Ultra96-V2 board preset or PS configuration. Measurements remain P1.2. |
| P1.2 | Build bitstream on LRC, load on board, measure sustained read GB/s vs burst length, outstanding count, clock (100/150/200 MHz), 1 vs 2 HP ports, with and without CPU activity. Also measure empty-job overhead (1000 no-op register write + poll cycles). Answer Q4. | B | DOING (integration preparation) | Codex + Sumedh, 2026-11-20 | Staged Vivado script prepared; next is LRC preflight and matching Ultra96 PS configuration, then BD/implementation and board run. `soc/docs/results/pl_read_bandwidth.md` remains absent until actual measurements. |
| P1.3 | Early ASIC area/power envelope for 4-, 8-, and 16-MAC configurations. Include: integer multiplier + reduction tree, FP16→FP32 conversion, FP32 multiply/add + accumulators, activation SRAM, scale storage, AXI read/write engine + FIFOs, descriptor/control/status logic. State process node (45 nm or 28 nm), use published component and SRAM-macro estimates or bounded analytical estimates with uncertainty. Do not use FPGA-to-ASIC LUT multipliers. | C | DOING | Codex, 2026-11-20 | `soc/docs/results/asic_envelope.md`: initial lane/bandwidth/storage accounting drafted. Process-specific area/power bounds remain to be established; A0 stays pending. |

### Phase 2: Software-only GEMV path, no FPGA needed (Nov 19 to 24)

| ID | Task | Type | Status | Owner | Acceptance / output |
|---|---|---|---|---|---|
| P2.1 | Repack library: Q8_0 tensor -> 4 KB-aligned `quants` + `scales` arrays (see 6.3), caller-provided buffers, reference arithmetic (golden model), unit tests incl. real Qwen tensors, tensor dump script | C | DONE | Claude, 2026-11-18 | `soc/sw/repack/` (README, lib, tests, Makefile), `soc/tools/dump_q8_tensor.py`. All tests pass on x86; full LM head round-trips; cross-compiles for A53. Board run of `test_repack` still TODO (B). |
| P2.2 | Implement the hook (location and conditions in 6.6) behind `-DGGML_SOC_GEMV=ON` that routes supported decode GEMVs to `soc_gemv_q8_0()`; unsupported cases fall through; any thread count. Includes the per-tensor weight cache (repack on first use, keyed by weight pointer + shape, stale-content check). | C | DONE (sandbox) | Claude, 2026-11-18 | `soc/sw/gemv/` + 2 upstream edits (3.2). Qwen output identical to stock; 169 matrices cached. **LRC cmake build with the Ultra96 toolchain still to be run (B).** |
| P2.3 | Emulation backend built on `soc_q8r_gemv_ref`. Check bit-exactness against llama.cpp's generic `vec_dot` and agreement with NEON. | C+B | DONE (sandbox) | Claude, 2026-11-18 | `soc/sw/gemv/soc_gemv_backend_emul.c`; `soc/docs/results/p2_hook_emul_test.md`. 100% bit-exact vs generic (no-FMA build); NEON diff 7e-7 relative. **Board run + `benchmark.py` score with the hook still to do (B, after P0.2).** |
| P2.4 | Per-op profile (decode only) with the hook active: time in GEMV vs everything else. Gives the Amdahl remainder. | B | TODO | | `soc/docs/results/cpu_remainder.md` |

### Phase 3: GEMV hardware (Nov 22 to 29; Thanksgiving Nov 26)

| ID | Task | Type | Status | Owner | Acceptance / output |
|---|---|---|---|---|---|
| P3.1 | HLS kernel v1: `m_axi` 128-bit weight stream, x BRAM, 16-wide lane, FP32 accumulation, y writer, `s_axilite` descriptor regs. Reuse Lab 2 FP16 conversion. | C | DONE (source + Vitis C simulation) | Codex, 2026-11-20 | `soc/hw/gemv/`: standalone kernel/header, Makefile, HLS script, testbench and README. Native synthetic and 512 real Qwen weight rows are bit-exact against the golden C reference; UBSan passes. Four logical AXI masters and draft register map; not connected to the software backend. First synthesis reviewed under P3.3 (main II=5); baseline Verilog co-simulation passes (54 calls, 720 rows, zero error); implementation timing remains pending. |
| P3.2 | Testbench: random and corner-case matrices (reuse the Lab 2 corner cases, add real Qwen tensor slices), compare to `gemv_emul` bit-exactly; report max abs/rel error | C | DONE (testbench + Vitis C simulation) | Codex, 2026-11-20 | `soc/hw/gemv/tb.cpp`: 55 native calls, finite FP16 converter check, shape/status/canary checks and optional SOCQ8T1 tensor slices; combined 63-call synthetic/real suite passes bit-exactly, max absolute error 0. Vitis C simulation passed on LRC: 54 calls, 720 rows, max absolute error 0. Generated-RTL behavior remains P3.3; expand tests from actual RTL/report findings. |
| P3.3 | Vitis HLS: csim, csynth, cosim; record II, latency, DSP/LUT/FF/BRAM, clock. Explore at least 3 alternatives (lanes, clock, interleave depth) for the PRD. | B | DOING (r8 II=1 and RTL pass; exploration ongoing) | Sumedh + Codex, 2026-11-20 | `soc/docs/results/hls_gemv_f150_b64_o8_v1.md`: main II=5 vs target 1 (FP recurrence), activation load II=42, 9,317 LUT/6,210 FF/12 DSP/7 BRAM_18K. Baseline Verilog co-simulation PASS: 54 calls/720 rows, max absolute error 0. Eight-row candidate now passes expanded native/real-slice golden checks and UBSan; candidate Vitis C simulation passes (85 calls/1,110 rows) and synthesis achieves II=1, 12,580 LUT/10,660 FF/22 DSP/11 BRAM_18K. Candidate RTL PASS verified: 85 calls/1,110 rows, zero error. Largest bounded case (64×4864) takes 88,596 simulated cycles; matched baseline latency and further design exploration remain pending. See `soc/docs/results/hls_gemv_f150_b64_o8_r8.md`. No board timing/BW or ASIC fit established. |
| P3.4 | (Optional) SystemC/TLM model of the engine with an HP-port initiator socket, run on TinyStories in the Lab 3 QEMU platform to co-verify the HW/SW protocol | C+B | TODO | | `soc/sw/cosim/` |

### Phase 4: Board integration (Nov 28 to Dec 3)

| ID | Task | Type | Status | Owner | Acceptance / output |
|---|---|---|---|---|---|
| P4.1 | Vivado block design Tcl: Zynq PS + GEMV IP + HP port + AXI-Lite (+IRQ if used); produce `.bit` and `.hwh` | C+B | TODO | | `soc/hw/vivado/`; bitstream in `soc/hw/out/` |
| P4.2 | HAL v2 and (if allowed) driver: map regs, allocate contiguous buffers, descriptor table per (layer, op), start/poll, coherency handling. Start from `soc/lab3_ref/part4`. | C | TODO | | `soc/sw/hal2/` |
| P4.3 | Board bring-up: single GEMV vs CPU on a real tensor; then full Qwen decode through the hardware | B | TODO | | Log in `soc/docs/results/bringup.md` |

### Phase 5: Validate and optimize (Dec 1 to 6)

| ID | Task | Type | Status | Owner | Acceptance / output |
|---|---|---|---|---|---|
| P5.1 | Run `benchmark.py`: score > 20 and total time < 30 min; record tok/s. Commit to `main` on Classroom 50 early (autograder scores once a day). | B | TODO | | `soc/docs/results/final_bench.md` |
| P5.2 | Optimization queue, in order of expected payoff after measuring: QKV and gate+up fusion (cuts calls), more lanes or a second HP port, model/quantization change, 4-bit path with hardware unpacking, CPU-remainder hot spots (activation quantization, RMSNorm, softmax/attention) | C+B | TODO | | One results file per change |

### Phase 6: Deliverables (Dec 3 to 7)

| ID | Task | Type | Status | Owner | Acceptance / output |
|---|---|---|---|---|---|
| P6.1 | PRD final version: update the living PRD (started in P0.7) with final HW/SW partitioning, interfaces, all requirements traced to evidence files | C | TODO | | `soc/docs/PRD.md` (update, not create from scratch) |
| P6.2 | High-level performance and power model; timing, area, power estimates validated against the MRD (include a scaling argument from the FPGA numbers to 45 nm / 28 nm) | C | TODO | | `soc/docs/perf_power_model.md` |
| P6.3 | `submission.json` via `make_submission.py`; repo cleanup, READMEs, `.bit`/`.hwh` committed | B | TODO | | Autograder run succeeds on `main` |
| P6.4 | Final slides (Canvas) and in-class presentation prep; design review material | C | TODO | | `soc/docs/slides/` |
| P6.5 | Individual peer-contribution report (each member, Canvas) | B | TODO | | Submitted |

---

## 9. Timeline

| Date | Milestone |
|---|---|
| Wed Nov 18 | Plan, branch, Lab 3 files copied (done). Repack + hook + emul backend done in sandbox. Plan updated from teammate review. Start P0.2, P0.5, P0.6, P0.7 on the board. |
| Fri Nov 20 | Baselines and autograder flow known (P0.2 to P0.7). PRD v0.1 drafted. ASIC envelope started (P1.3). |
| Sun Nov 22 | PL read bandwidth + launch overhead measured (P1.2). Promotion gates M0/D0/B0 evaluated. |
| Tue Nov 24 | LRC cmake build verified. HLS kernel v1 drafted (P3.1). ASIC envelope done (P1.3, gate A0). |
| Thu Nov 26 | Thanksgiving (limited availability). |
| Sun Nov 29 | HLS kernel synthesized and co-simulated (P3.3). |
| Thu Dec 3 | Bitstream on the board; single GEMV and full decode working (P4.x). |
| Sat Dec 5 | First autograder-eligible commit on `main` (P5.1). Optimization round (P5.2). |
| Mon Dec 7 | Slides, PRD, power/area model, submission.json finalized. |
| Tue Dec 8 | **Due.** |

If a milestone slips by more than a day, drop optional items first (P3.4, 4-bit path, deep fusion) and keep the end-to-end path.

---

## 10. Risks

| Risk | Mitigation |
|---|---|
| Kernel module not loadable in the autograder | Decide in P0.5; HAL v2 uses user-space access (UIO, /dev/mem, pynq-style buffers) if needed |
| Not enough contiguous memory for repacked weights (about 525 MB total, 145 MB largest) | P0.6 early with llama-server running; cache marks entries CPU_ONLY on alloc failure (graceful fallback); selective repack of highest-value matrices; native Q8 streaming as fallback layout; smaller model/quantization reduces traffic |
| PL read bandwidth much lower than 3 to 4 GB/s | P1.2 early; second port, wider bursts, clock tuning; shrink weights (Q4) to cut traffic |
| Cache coherency bugs (stale x or y) | Non-cacheable x/y buffers first (simple, small); optimize later |
| CPU remainder dominates after offload | P2.4 gives the number early; fuse calls, move elementwise ops later |
| FP32 accumulate latency limits II | Interleave rows or use multiple accumulators |
| Accuracy drop from numeric differences | Compare to emulation bit-exactly, to CPU by score; keep float accumulation |
| Hook interacts with REPACK/LLAMAFILE defaults | Test both in P2.2; ship flags that keep the CPU fallback fast |
| QEMU/SystemC too slow for full Qwen | Co-verify on TinyStories; validate Qwen on the real board |
| Schedule (20 days, Thanksgiving) | Section 9 cut-list; test on the board early and often |

---

## 11. Backlog of alternative or stretch ideas

- Lower-bit weights (Q4_0, Q4_K) with hardware unpacking: halves DDR traffic; Lab 1 showed the CPU is slower on K-quants due to unpacking.
- Smaller or different model (LFM2-350M: 8.22 tok/s, 48.4 at default clock; Qwen Q2_K 48.1). Accuracy bar is only > 20.
- Fused FFN (gate+up -> SiLU*up -> down) and whole-block offload; reduces CPU round trips.
- Multi-vector GEMV (several x per weight pass) so prefill reuses weights.
- LM-head specialization (27% of weights): lower precision; vocabulary restriction is risky for IFEval.
- Offload remaining CPU hot spots: activation quantization, RMSNorm, softmax, attention.
- Interface tuning: HPC vs HP, polling vs interrupt, descriptor chaining.
- CPU-only tuning of the fallback path (flags, threads, prompt/context limits).

---

## 12. Command cheat sheet (from the lab handouts)

LRC environment and cross-compile:

```bash
module load xilinx/2022
source /usr/local/packages/Xilinx_2022.2/Vivado/2022.2/settings64.sh
module load gcc/14
cp /home/projects/gerstl/ece382m/ultra96_toolchain.cmake .
mkdir build && cd build
/usr/bin/cmake -DCMAKE_TOOLCHAIN_FILE=../ultra96_toolchain.cmake -DBUILD_SHARED_LIBS=OFF ..
make -j 8
```

With our GEMV hook: add `-DGGML_SOC_GEMV=ON` to the cmake line (configure prints `SoC GEMV offload enabled for ggml-cpu`). Run-time: `SOC_GEMV=0` disables it, `SOC_GEMV_STATS=1` prints offload statistics, `SOC_GEMV_VERIFY=1` checks every GEMV against llama.cpp.

Profiling build: add `-DCMAKE_C_FLAGS="-pg" -DCMAKE_CXX_FLAGS="-pg"`, run `./llama-cli -m qwen2.5-0.5b-instruct-q8_0.gguf -t 1 -tb 1 -st -p "Describe a system-on-chip in two sentences."`, then `gprof llama-cli gmon.out > llama.perf`.

Benchmark (needs `/home/projects/gerstl/ece382m/llama-bench.tar.gz` unpacked; upload `bin/llama-server` to the board):

```bash
python benchmark.py --llama-server <path to llama-server> --model <path to model.gguf>
```

Board: serial `minicom -D /dev/ttyUSB1 -b 115200 -8 -o`; WiFi `sudo /root/wifi.sh`; MRD CPU config and revert in section 5.3; `sudo halt` before power-off. Model download: `wget https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q8_0.gguf`.

HLS: `module load xilinx/2022; vitis_hls`, part `xczu3eg-sbva484-1-e`, set interface pragma `depth` equal to the array size or co-simulation fails.

Lab 3 platform (reference, from `soc/lab3_ref/LAB3_README.md`): cross-build flags `-DSOC_DOT_HAL_DIR=... -DGGML_NATIVE=OFF -DGGML_BLAS=OFF -DGGML_LLAMAFILE=OFF -DGGML_CPU_REPACK=OFF -DGGML_CPU_KLEIDIAI=OFF -DGGML_OPENMP=OFF`; QEMU `/home/projects/gerstl/ece382m/qemu-boot -c`; SystemC `./zynqmp_dot unix:../tmp/qemu-rport-_amba@0_cosim@0 1000000 -v 2`.

---

## 13. Decision log

| Date | Decision | Reason |
|---|---|---|
| 2026-11-18 | Core design is a bus-mastering Q8_0 GEMV engine (section 6), not a per-row dot-product accelerator | Lab 3 structure is dominated by per-call MMIO and resends x and weights; decode is bandwidth bound with no weight reuse |
| 2026-11-18 | Keep float (FP32) accumulation | Lab 2: wide fixed-point cost more area than float and was not bit-exact |
| 2026-11-18 | Project code lives under `soc/` in the llama.cpp repo; Lab 3 originals kept unchanged in `soc/lab3_ref/` | Keeps upstream edits small and reference code clean |
| 2026-11-18 | Branch `soc-gemv-accel`; no pushes to upstream origin | Private class-project clone |
| 2026-11-18 | Repack is an in-memory step at load/first use, never a change to the .gguf; the library takes caller-provided buffers so the HAL decides where memory lives | Submission points at the original model; PL needs contiguous memory the HAL owns |
| 2026-11-18 | Hook after the src1 quantization + barrier in `ggml_compute_forward_mul_mat`; thread 0 does the call | x already in native Q8_0 blocks there; must work for any thread count |
| 2026-11-18 | Golden arithmetic = llama.cpp scalar algorithm with separate FP32 multiply and add (no FMA); hardware must match it | Bit-exact vs `ggml_vec_dot_q8_0_q8_0_generic` built with `-ffp-contract=off` (default GCC build fuses one `fmadd`); separate mul/add is also the cheaper hardware |
| 2026-11-18 | Backend interface (`soc_gemv_backend`: init/shutdown/alloc/free/run) so the FPGA HAL drops in without touching the hook or cache | Keeps P4.2 to one new file |
| 2026-11-18 | Run the model/quant sweep in parallel with hardware work | Accuracy bar (> 20) is far below our 51.4; a smaller or lower-bit model could be the cheapest speedup |
| 2026-11-18 | Reclassify bus-mastering Q8_0 GEMV from "chosen architecture" to "primary candidate" with explicit promotion gates (M0/D0/B0/A0) | Teammate review: architecture was declared before model sweep, CMA check, bandwidth measurement, and ASIC estimation. All code written so far is reusable regardless of gate outcomes. |
| 2026-11-18 | Add early ASIC area/power envelope (P1.3) before committing to lane count | Teammate review (C5): full accelerator includes MACs, FP conversion, AXI engines, FIFOs, SRAM, control — not just the datapath. FPGA LUT counts are not a conversion to mm². |
| 2026-11-18 | Fix fused call count: 97 (not 73) with QKV + gate/up fusion | Teammate review (T1): 7 − 2 − 1 = 4 calls/layer × 24 + 1 = 97. The 73 figure was an arithmetic error. |
| 2026-11-18 | Default PL clock target 150 MHz (stretch: 200 MHz) | Teammate review (T7): 200–250 MHz is aggressive for HLS with MAC trees on ZU3EG. Lower clock can improve II and routing. |
| 2026-11-18 | Add memory budget analysis and fallback layouts (selective repack, native Q8 streaming) | Teammate review (C3): 525 MB CMA is the largest feasibility risk. Steady-state ~855 MB fits, but CMA config may be the bottleneck. Cache already handles partial failure (CPU_ONLY fallback). |
| 2026-11-18 | Replace single-number speed estimates with bandwidth-scenario ranges | Teammate review (C4): CPU frequency scaling is not linear for memory-bound workloads. Report ranges until board data. |
| 2026-11-18 | Move PRD to P0.7 (living document from the start, finalized in P6.1) | Teammate review (C6): PRD should guide model choice and acceptance tests, not be a final-week deliverable. |
| 2026-11-19 | P1.1 uses an external RTL counter on the HLS AXI read handshakes; report its read-window cycles separately from CPU job time | Measures initial latency, gaps and stalls without relying on C-loop iterations as a cycle count. PYNQ empty-job timing measures that runner's overhead, not the final C HAL. |
| 2026-11-19 | Require an installed Ultra96-V2 board preset or a PS configuration exported from a known-working board design | No verified DDR/MIO preset is present in this checkout. Do not invent board-specific settings. PYNQ availability remains subject to P0.5. |
| 2026-11-19 | Continue independent source/documentation work while testing each major boundary; defer a new GEMV QEMU model pending course clarification or debugging need | Project handout requires HW/SW co-verification but does not explicitly mandate QEMU. The old slave-only Lab 3 model does not validate the new DMA/GEMV design. HLS RTL checks and real-board comparisons remain required checkpoints for our verification strategy. |
| 2026-11-20 | P3.1 v1 preserves sequential FP32 block order and uses separate quant/scale/activation/output AXI masters; no row interleaving or bias fusion yet | Establish a checked golden-compatible baseline with existing layouts. The FP recurrence and per-row scale preparation may limit throughput; do not claim II=1 or DDR saturation before synthesis/measurements. Nonfinite scales return failure for future CPU fallback. |

---

| 2026-11-20 | Explore eight-row interleaving with 32-block weight tiles and a rotating accumulator register queue; retain selectable sequential baseline in its original HLS project | Preserve each row's golden FP order and contiguous weight bursts while separating accumulator feedback across independent rows. Candidate II/resources/RTL still require Vitis; no false-dependence directives or speedup claims. |

## 14. Session log (append only, newest last)

- **2026-11-18, session 1 (Claude with Sumedh):** Read the project and lab handouts, three lab reports, and the Lab 3 repo. Compared design options and chose the GEMV engine. Created this plan, branch `soc-gemv-accel`, `soc/lab3_ref/` (Lab 3 part3/part4 copied unchanged and verified identical), and `soc/docs/reference/` text copies. Nothing committed yet. Next: P0.2, P0.5, P0.6 on the board; P0.4 and P2.1 from files.
- **2026-11-18, session 1 (cont.):** P0.4 done. Dumped the Qwen GGUF with gguf-py: embeddings are not tied (separate `output.weight`), all 170 matmul tensors are Q8_0, biases on Q/K/V, 524,833,792 B of weights read per token, largest matrix 144.6 MB. Corrected sections 5.1, 5.2, 6.3 (the earlier `FPGA_OFFLOAD_DESIGN.md` draft assumed tied embeddings). Results in `soc/docs/results/model_dims.md`. Next from files: P2.1 (repack tool).
- **2026-11-18, session 2 (Claude on Opus, with Sumedh):** P2.1 done: `soc/sw/repack/` (library, golden arithmetic matching `ggml_vec_dot_q8_0_q8_0_generic`, tests) and `soc/tools/dump_q8_tensor.py`. All tests pass, including the full 151,936-row LM head (lossless, bit-exact GEMV vs native layout); cross-compiles for the A53. Reviewed the plan against llama.cpp source: pinned the hook location and conditions (6.6), confirmed llama.cpp's Q8_0 repack and llamafile paths do not activate on the A53, added the any-thread-count requirement (autograder uses `llama-server`), and the no-FMA numerics note (6.4). Reviewed the Phase 0 entries against the GGUF and lab reports: correct. Next: P2.2 hook + weight cache, P2.3 emulation backend. Board: run `test_repack` on the A53 and do P0.2/P0.5/P0.6.
- **2026-11-18, session 2 (cont.):** P2.2 and P2.3 done in the sandbox. Added `soc/sw/gemv/` (entry point `soc_gemv_q8_0`, weight cache, backend interface, emul backend, CMake fragment, unit test) and the two upstream edits (`ggml-cpu.c` hook after the barrier; `ggml-cpu/CMakeLists.txt` option). Discovered the sandbox is aarch64 without cmake; wrote `soc/tools/build_nocmake_sandbox.sh` and built with `-mcpu=cortex-a53`. Results (`soc/docs/results/p2_hook_emul_test.md`): Qwen output identical to stock on two prompts, 169 matrices / 525.2 MB cached, 0 fallbacks; verify vs NEON max diff 7e-7 relative; golden 100% bit-exact vs llama.cpp generic when built without FMA contraction (default build fuses one `fmadd`). Confirmed ggml has a barrier after every node and that the only CPU fusion (RMS_NORM+MUL) never touches MUL_MAT. `test_repack` A53 build passes natively. Still for the team: LRC cmake build with `-DGGML_SOC_GEMV=ON`, board run, benchmark score with the hook. Next from files: P1.1 bandwidth-test HLS + Vivado script, then P3.1 GEMV HLS kernel.
- **2026-11-18, session 3 (Claude on Opus, with Sumedh):** Reviewed teammate's starter plan (`SoC_Project_Starter_Plan.md`) and their detailed review of our plan (`SOC_PROJECT_PLAN_REVIEW.md`). Applied justified changes: (1) renamed section 6 from "Chosen architecture" to "Primary candidate" with four promotion gates M0/D0/B0/A0; (2) fixed fused call count 73→97; (3) fixed .bit gitignore to unignore `soc/hw/out/`; (4) added memory budget analysis and fallback layouts (selective repack, native Q8 streaming) to section 6.3; (5) replaced single-number speed estimates with bandwidth-scenario table in section 6.7; (6) added P1.3 early ASIC envelope task; (7) adjusted default PL clock to 150 MHz (stretch 200); (8) added two-stream HLS note to section 6.6; (9) moved PRD to P0.7 as a living document; (10) made stale-check cost explicit (68 B/hit, negligible); (11) expanded P0.6 to include llama-server memory test, per-matrix allocation sequence, and fragmentation reboot test; (12) added P1.2 empty-job overhead measurement. Declined changes: T3 (stale check already cheap), T9 (ggml barrier already ensures correctness), C2 model-specificity (hook is model-agnostic within Q8_0). All decisions logged in section 13. No code changes. Next from files: P1.1 bandwidth-test HLS, P1.3 ASIC envelope, P0.7 PRD v0.1, then P3.1 GEMV HLS kernel.
- **2026-11-19, session 4 (Codex with Sumedh):** Implemented P1.1 source under `soc/hw/bw_test/`: 128-bit sequential AXI reader with four-lane checksum and empty jobs, HLS testbench, external RTL read-window cycle counter and testbench, parameterized HLS/Vivado scripts, optional PYNQ CSV runner and README. Local kernel tests (60 cases) and UndefinedBehaviorSanitizer pass; mocked runner checks cover success, checksum failure, timeout and interruption. Tcl syntax completeness and parameter guards pass; vendor commands were not executed. AddressSanitizer did not complete locally and was interrupted. Validation details: `soc/docs/results/p1_bw_test_validation.md`. Added scoped hardware build ignores that retain BIT/HWH deliverables. No existing software or upstream code changed, and no commit made. P1.1 source is done; Vitis C simulation/synthesis/co-simulation, RTL simulation and Vivado validation need LRC. A verified Ultra96-V2 preset/PS configuration and confirmation of PYNQ support are required before board use. P1.2 measurements remain TODO. Next: run the provided LRC checks; in parallel from files, P1.3 ASIC envelope and P0.7 PRD.

- **2026-11-19, session 5 (Codex with Sumedh):** Created `soc/docs/NEXT_STEPS.md` as the requested LRC/board execution checklist, with simulation/build commands, baseline/memory/bandwidth evidence to return, stage checkpoints and a QEMU/SystemC recommendation. Reviewed the project handout, Lab 3 handout/report, original Lab 3 README/Makefile and platform source: the project requires HW/SW co-verification but does not explicitly mandate QEMU before board loading; the old model is slave-only and would need a new GEMV model plus HP-port initiator to cover DMA. Defer that optional model pending tutorial/rubric clarification, board access or debugging need. Noted the reference copy lacks the Makefile.dot expected by its setup script; the original Lab 3 directory has it and remains the rerun source. Drafted `soc/docs/PRD.md` (P0.7) with R1-R8 traceability, provisional HW/SW partition, DMA/coherency/numerics contracts, gates and verification policy. No gate has been promoted, no hardware evidence added, and no source code or Lab 3 reference files changed. Next local work: P1.3 ASIC envelope and provisional GEMV kernel/testbench, while team runs NEXT_STEPS checks.

- **2026-11-20, session 6 (Codex with Sumedh):** Sumedh reports the project was pushed and cloned on LRC. Added a current-session entry to NEXT_STEPS with the ordered native/C/RTL/export checks and pipefail/log handling. Began P1.3 in `soc/docs/results/asic_envelope.md`: analytical 4/8/16-product throughput/bandwidth comparison, storage inventory and complete area/power accounting requirements. No area/power fit claimed; process-specific bounds and A0 remain pending. No target runs or measurements reported yet. Next user work: C simulation and monitor RTL simulation, then HLS export on success; next local work: continue ASIC characterization and provisional GEMV/testbench design.

- **2026-11-20, session 7 (Codex with Sumedh):** Reviewed Sumedh's first LRC Vitis HLS 2022.2 log. Tool rejected the absolute project name at open_project, so C simulation never started. Fixed build_hls.tcl to create/enter the parent build directory and open the basename, preserving absolute source paths and the expected output tree. Local Tcl harness passes project-name, source-path and csim-stage checks; actual Vitis rerun remains pending. Updated p1_bw_test_validation.md with the failure and fix. No hardware/checkpoint pass claimed.

- **2026-11-20, session 8 (Codex with Sumedh):** Reviewed reports supplied in workspace `lrc/report/` from Vitis HLS 2022.2 project f150_b64_o8. Confirmed read-loop II=1/depth=3, 128-bit AXI/64-bit addresses, inferred variable-length read burst, configured burst 64/outstanding 8 and matching control-register offsets. Kernel estimates: 2,450 LUT, 1,455 FF, zero DSP/BRAM; timing estimate 4.867 ns plus 1.80 ns uncertainty against 6.67 ns target (reported slack 0.00). Widening-limit message is consistent with the intentional 128-bit port; unknown total latency reflects runtime transfer length. Synthesis structural/schedule checks pass. No C-sim/co-sim/monitor simulation pass logs, route timing or bandwidth supplied; those checks and B0 remain pending. Updated validation record and bandwidth README; no hardware source changes.

- **2026-11-20, session 9 (Codex with Sumedh):** Reviewed newly supplied lrc/export.log and simulation reports. Verified C simulation zero errors, completed synthesis, Verilog C/RTL co-simulation PASS with all 60 transactions and zero post-check failures, then successful IP packaging/export.zip generation and final export completion. No ERROR/FATAL log lines found. Updated validation record, bandwidth README and NEXT_STEPS to avoid rerunning completed stages. The standalone RTL cycle monitor is not part of the HLS top and still lacks a simulation log; full-system implementation timing and board bandwidth also remain pending. No hardware source changed and no commit made.

- **2026-11-20, session 10 (Codex with Sumedh):** Sumedh confirms separate cycle-counter simulation passes; recorded user-reported status without claiming a raw-log review. Began P1.2 integration preparation: added Vivado check/bd/bitstream stages, HLS/preset target-part consistency checks, case-insensitive installed-board discovery, optional user-owned board repository, BD Tcl/address CSV export and polling-only PS IRQ configuration. Both build scripts retain default -e and allow explicit -i only after actual target confirmation. Official current Avnet Ultra96-V2 BDF names -i, so preset/target resolution is needed before implementation; no silent grade substitution. Local mocked Tcl flow checks pass; no actual Vivado design/bitstream run here. Updated handoff instructions and validation notes. Next user action: sync updated Vivado script and run preflight once, returning its log.

- **2026-11-20, session 11 (Codex with Sumedh):** Started and implemented P3.1 under `soc/hw/gemv/`, following the user's direction to work on GEMV rather than bandwidth-board integration. Added ordered Q8_0 GEMV source (16 products/beat, activation BRAM, split weight layout, reused Lab 2/3 FP16 conversion, FP32 output, shape/nonfinite status), standalone build and HLS scripts plus a P3.2 starter testbench. Native suite passes 55 calls/152,656 rows; eight real Qwen slices add 512 rows (63 calls/153,168 rows total), all bit-exact with max absolute error 0. UBSan combined suite passes. Checked all finite FP16 conversions, malformed tensor rejection and mocked HLS stage dispatch; no actual Vitis execution here. Validation/provenance in `soc/docs/results/p3_gemv_source_validation.md`. Source is provisional: FP recurrence can limit II, four AXI adapters add overhead, and no backend/HAL integration or MRD gate pass is claimed. Updated GEMV handoff: next is LRC C simulation for this new kernel, with P1.1 results retained and no rerun requested. No upstream or Lab 3 reference edits and no commit made.

- **2026-11-20, session 12 (Codex with Sumedh):** First LRC GEMV attempt stopped before compilation/C simulation because Vitis 2022.2 config_compile Boolean values require the equals form. Fixed both strict-FP options to -unsafe_math_optimizations=false and -no_signed_zeros=false. Added a local mocked Tcl regression that rejects the old spelling and checks corrected settings/source paths/csim dispatch; it passes. No kernel arithmetic change; actual Vitis C simulation remains pending. Preparing the correction for the same private P3.1 branch so Sumedh can pull and rerun.

- **2026-11-20, session 13 (Codex with Sumedh):** Reviewed pasted LRC attempt after Boolean fix: apcc applies a C++11 option while compiling the C reference, failing before the GEMV tests run. Changed HLS build to compile unchanged soc_repack.c as a native C11 object and link it via documented -ldflags in both C and RTL co-simulation, without registering the C source with apcc. Local control-flow/link regression passes and the actual Tcl-built object linked into the native kernel/testbench passes all 63 synthetic/real cases bit-exactly. No kernel arithmetic/reference changes; actual Vitis C simulation remains pending. Preparing scoped correction for the same private P3.1 branch.

- **2026-11-20, session 14 (Codex with Sumedh):** Sumedh supplied GEMV Vitis C-simulation PASS (54 calls, 720 rows, max absolute error 0, zero failures), with CSim done with 0 errors and final stage completion at 01:42:14 CST. Recorded this as the expected bounded HLS testbench result, distinct from prior native/real-slice coverage. Both earlier script/build issues are resolved for this run. Next boundary: GEMV synthesis and report review of actual II, bursts, FP operators, resources and timing; no RTL or board pass claimed.

- **2026-11-20, session 15 (Codex with Sumedh):** Reviewed GEMV synthesis files supplied in lrc/report and lrc/synth.log. Synthesis completes with expected 128-bit quants and 16-bit scales bursts and register offsets; actual main II=5 misses requested 1 due to ordered FP accumulator (fadd latency 5). Activation load II=42, scale reads II=1, conversion II=2; ports remain 8/16/32 bits for activation/scales/output. Estimated kernel resources: 9,317 LUT, 6,210 FF, 12 DSP, 7 BRAM_18K; estimated period 4.867 ns with 1.80 ns uncertainty at 6.67 ns target. Recorded v1 baseline and warnings without claiming pipeline-target success or measured bandwidth. Next: unchanged baseline RTL co-simulation, then evidence-based scheduling/layout optimization. No source change or push needed for next command.

- **2026-11-20, session 16 (Codex with Sumedh):** Inspected newly supplied lrc/cosim.log: its completed stage is csim (54 calls/720 rows PASS), with no synthesis or cosim invocation. No GEMV cosim RPT supplied; old bandwidth-reader cosim files remain in the mixed report directory. GEMV RTL result stays unverified. Request the actual existing cosim output if already run; otherwise invoke SOC_GEMV_HW_STAGE=cosim explicitly. No kernel/script change needed from this evidence.

- **2026-11-20, session 17 (Codex with Sumedh):** Reviewed replacement lrc/cosim.log: actual Verilog cosim invocation, final 54-call/720-row golden comparison with max absolute error 0 and zero failures, explicit C/RTL PASS and stage cosim completion at 02:01:50 CST. No ERROR/FATAL lines. Updated evidence and handoff; unchanged baseline needs no rerun. P3.3 remains in progress for performance alternatives: main II=5 and activation loading remain optimization targets. No kernel change, board result or hardware performance pass claimed.

- **2026-11-20, session 18 (Codex with Sumedh):** Implemented user-requested row interleaving: eight independent rows share the 16-product datapath, tile 32 blocks per row, and rotate eight accumulator registers; FP block order, interfaces and memory formats preserved. Original sequential implementation remains selectable (1); default candidate is 8 in a separate `_r8` HLS project. Added row/block-boundary and late-tile invalid-scale tests. Both variants pass 86-call expanded native suite bit-exactly; candidate plus eight real Qwen slices passes 94 calls/153,558 rows, max error 0, including separately instrumented UBSan. Mocked Tcl project-separation/strict-FP/link checks pass. Candidate vendor simulation/schedule/resources remain pending; prepared synth-stage handoff that includes C simulation. No baseline rerun or board setup needed.

- **2026-11-20, session 19 (Codex with Sumedh):** Reviewed newly supplied r8 synth.log and candidate/top XML/RPT. C simulation PASS: 85 calls/1,110 rows, zero error. interleave_words achieves II=1/depth=22 versus baseline II=5; tile quant/scale reads and output loop II=1. Activation load remains II=42, scale conversion II=2, explaining remaining pipeline warnings without a compute target miss. Inferred contiguous weight/scale reads and output burst writes. Candidate estimates: 12,580 LUT, 10,660 FF, 22 DSP, 11 BRAM_18K; estimated period 4.867 ns, not implemented timing. Recorded tradeoffs/provenance and prepared unchanged-candidate RTL cosim command. No fivefold whole-kernel speedup, board performance or candidate RTL pass claimed.

- **2026-11-20, session 20 (Codex with Sumedh):** Reviewed lrc/gemv_r8_review.tar.gz without rerunning Vitis. Verified explicit Verilog/xsim co-simulation PASS and final golden check: 85 calls/1,110 rows, max error 0, no failures or ERROR/FATAL lines. Mapped all 85 transaction records to testbench cases and recorded successful shape latencies: 32×896 takes 9,244 cycles; 64×4864 takes 88,596 cycles (590.64 µs at configured 150 MHz). Aggregate timings include invalid/empty calls and are not representative GEMV latency. Reports provide no load/compute phase breakdown or matched sequential baseline timing; no board bandwidth or fivefold full-call speedup claimed. Preserved r8 as verified candidate and documented profiling/double-buffering options for subsequent work. Third design alternative and board/ASIC gates remain pending.
