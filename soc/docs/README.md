# SoC Project Documentation Index

Current GEMV selection: sequential direct weight-read kernel (`ROW_INTERLEAVE=1`) is the default. Row interleaving and double-buffering work are deferred at Sumedh's request; the verified r8 experiment and its results are retained for reference.


**Project:** ECE382M.20 Edge LLM Inference SoC — GEMV Acceleration
**Due:** 2026-12-08
**Platform:** Ultra96 (Zynq UltraScale+ ZU3EG), ARM A53 + FPGA
**Model:** Qwen2.5-0.5B Q8_0 (primary candidate, pending board confirmation)

---

## How to use this index

This file maps every document in the project. When transferring context to another LLM session, provide documents in the **recommended reading order** below. Each document is self-contained but builds on the ones before it.

---

## Recommended reading order

### Phase 0: Understand the project

| # | File | What it covers | Priority |
|---|---|---|---|
| 1 | `llama.cpp/SOC_PROJECT_PLAN.md` | **Master plan.** Everything: MRD, team, repo layout, lab results, model analysis, architecture design, task list with status, timeline, decision log, session log. Read this first, always. | MUST READ |
| 2 | `soc/docs/ARCHITECTURE_NOTES.md` | Zynq memory architecture, DDR↔FPGA path via HP ports, Lab 3 slave vs our master design, Q8_0 repack format, bandwidth test rationale, memory budget, promotion gates. Good for understanding *why* the design is the way it is. | MUST READ |
| 3 | `soc/docs/PROFILING_GUIDE.md` | Step-by-step board profiling commands (10 steps). Cross-compile on LRC, run on board at 300 MHz. Covers model sweep, `llama-bench`, `benchmark.py`, memory checks, function-level profiling, hook testing. | READ when doing board work |
| 3a | `soc/docs/NEXT_STEPS.md` | Current user handoff: ordered LRC/board checklist, commands, evidence to return, verification checkpoints and QEMU decision. | READ for the next runs |

### Phase 1: Understand the existing code

| # | File | What it covers | Priority |
|---|---|---|---|
| 4 | `soc/sw/repack/README.md` | Repack library: converts Q8_0 blocks into hardware-friendly quants+scales arrays. API, file list, how to build and test. Task P2.1, done. | READ before touching repack code |
| 5 | `soc/sw/gemv/README.md` | GEMV hook: how llama.cpp calls into our accelerator path. Weight cache, backend interface, emulation backend. Task P2.2/P2.3, done. | READ before touching hook code |
| 5a | `soc/hw/gemv/README.md` | GEMV HLS v1 data path, arithmetic, interfaces, local tests and LRC C-simulation command. | READ before GEMV hardware work |
| 6 | `soc/lab3_ref/LAB3_README.md` | Lab 3 reference: how to build and run the SystemC accelerator + QEMU platform. Kept unchanged as reference. | OPTIONAL — only if you need Lab 3 context |

### Phase 2: Results and measurements

| # | File | What it covers | Status |
|---|---|---|---|
| 7 | `soc/docs/results/model_dims.md` | Qwen2.5-0.5B tensor dump: all 291 tensors, shapes, types, byte counts. Confirms embeddings not tied, all matmuls Q8_0, 525 MB read per token. | Done |
| 8 | `soc/docs/results/p2_hook_emul_test.md` | Emulation backend test results: bit-exact vs generic CPU (no-FMA), NEON diff 7e-7 relative. | Done (sandbox only, board run pending) |
| 8a | `soc/docs/results/p1_bw_test_validation.md` | P1.1 native checks, verified LRC HLS run and current integration status. | HLS verified; no board bandwidth measurement |
| 8b | `soc/docs/results/p3_gemv_source_validation.md` | P3.1 native/real-weight and Vitis C checks, plus synthesis follow-up. | C and baseline RTL checks pass; synthesis reviewed |
| 8c | `soc/docs/results/hls_gemv_f150_b64_o8_v1.md` | First GEMV synthesis baseline, achieved II, ports, resources, warnings and next checks. | Reviewed; II=5 misses target 1; baseline RTL passes |
| 8d | `soc/docs/results/hls_gemv_f150_b64_o8_r8.md` | Eight-row interleaving candidate, tile/accumulator design and expanded local checks. | Native/real-slice/UBSan and Vitis C pass; compute II=1; candidate RTL passes |
| 8e | `soc/docs/results/p4_integration_source_validation.md` | P4.1/P4.2 source, local checks, adapter assumptions and remaining target boundaries. | Local checks pass; vendor/board checks pending |
| 8f | `soc/docs/results/p4_register_test_source_validation.md` | Dedicated AXI-Lite RTL and board-readback test source and local checks. | Model/runner checks pass; actual RTL/board tests pending |
| 9 | `soc/docs/results/baseline_300mhz.md` | 300 MHz single-core baseline speed and accuracy | TODO (P0.2) |
| 10 | `soc/docs/results/model_sweep_300mhz.md` | Model/quant sweep results at 300 MHz | TODO (P0.3) |
| 11 | `soc/docs/results/board_memory.md` | CMA, free memory, allocation sequence test | TODO (P0.6) |
| 12 | `soc/docs/results/pl_read_bandwidth.md` | HP port bandwidth measurements (sets tok/s ceiling) | TODO (P1.2) |
| 13 | `soc/docs/results/cpu_remainder.md` | Per-op profile with hook active (Amdahl remainder) | TODO (P2.4) |

### Phase 3: Design documents

| # | File | What it covers | Status |
|---|---|---|---|
| 14 | `soc/docs/PRD.md` | Product Requirements Document v0.1 | Initial draft done (P0.7); update at gates and finalize in P6.1 |
| 15 | `soc/docs/perf_power_model.md` | High-level performance, area, power model | TODO (P6.2) |

### Reference material

| File | What it covers |
|---|---|
| `soc/docs/reference/*.txt` | Text copies of lab handouts and reports (for sessions that can't read PDFs) |
| `llama.cpp/FPGA_OFFLOAD_DESIGN.md` | Earlier design draft (Option A). Superseded by SOC_PROJECT_PLAN.md section 6 where they differ. |

---

## File tree

```
llama.cpp/
├── SOC_PROJECT_PLAN.md              ← MASTER PLAN (start here)
├── FPGA_OFFLOAD_DESIGN.md           ← earlier design draft (reference only)
├── soc/
│   ├── docs/
│   │   ├── README.md                ← THIS FILE (navigation index)
│   │   ├── ARCHITECTURE_NOTES.md    ← hardware/software architecture explainer
│   │   ├── PROFILING_GUIDE.md       ← step-by-step board profiling commands
│   │   ├── NEXT_STEPS.md            ← current LRC/board handoff and verification checklist
│   │   ├── PRD.md                   ← v0.1 requirements draft; final validation pending
│   │   ├── perf_power_model.md      ← (TODO) performance/area/power model
│   │   ├── reference/               ← text copies of lab handouts and reports
│   │   └── results/                 ← one file per experiment, dated
│   │       ├── model_dims.md            tensor dump of Qwen2.5-0.5B
│   │       ├── p2_hook_emul_test.md     emulation backend test results
│   │       ├── p1_bw_test_validation.md P1.1 local validation, not board measurements
│   │       ├── baseline_300mhz.md       (TODO) P0.2 baseline
│   │       ├── model_sweep_300mhz.md    (TODO) P0.3 model sweep
│   │       ├── board_memory.md          (TODO) P0.6 CMA check
│   │       ├── pl_read_bandwidth.md     (TODO) P1.2 bandwidth measurement
│   │       └── cpu_remainder.md         (TODO) P2.4 Amdahl remainder
│   ├── lab3_ref/                    ← Lab 3 files, UNCHANGED (reference only)
│   │   ├── LAB3_README.md
│   │   ├── part3/                       SystemC accelerator + QEMU platform
│   │   └── part4/                       driver, HAL, GGML hook (vec_dot level)
│   │       ├── hal/dot_hal.c            HAL: de-interleaves Q8_0, pushes via ioctl
│   │       └── driver/dot_accel_drv.c   kernel driver: copies to BRAM via MMIO writes
│   ├── tools/
│   │   ├── dump_q8_tensor.py        dump Q8_0 tensors from GGUF to .bin test files
│   │   └── build_nocmake_sandbox.sh build without cmake (sandbox only)
│   ├── sw/
│   │   ├── repack/                  Q8_0 repack library + tests (P2.1, done)
│   │   ├── gemv/                    GEMV hook + weight cache + backends (P2.2/P2.3, done)
│   │   └── hal2/                    (TODO) FPGA backend / HAL v2 (P4.2)
│   └── hw/                          (TODO) HLS sources, Vivado scripts, bitstream
│       ├── bw_test/                 bandwidth test HLS IP, timing RTL, scripts and runner (P1.1 source done; vendor validation pending)
│       ├── gemv/                    GEMV HLS kernel/testbench/scripts (P3.1 source done; Vitis pending)
│       ├── vivado/                  (TODO) block design Tcl (P4.1)
│       └── out/                     (TODO) .bit and .hwh for submission
```

---

## Project phases and task status

### Phase 0: Setup and baselines (gates M0, foundation for everything)
- P0.0 Plan file, branch, Lab 3 reference copy — **DONE**
- P0.1 .gitignore and Classroom 50 remote — TODO
- P0.2 300 MHz baseline (board) — TODO ← **DO THIS FIRST**
- P0.3 Model/quant sweep at 300 MHz (board) — TODO
- P0.4 Model tensor dump — **DONE**
- P0.5 Board tutorial + autograder flow — TODO
- P0.6 CMA memory check on board — TODO
- P0.7 PRD v0.1 — **DONE (initial draft)**; update at each architecture gate

### Phase 1: Architecture validation (gates D0, B0, A0)
- P1.1 HLS bandwidth test IP — **DONE**: HLS C/synthesis/RTL co-simulation/export verified; standalone monitor pass reported by Sumedh. Vivado integration/board checks remain P1.2.
- P1.2 HP port bandwidth measurement (board) — **DOING (integration preparation)**; staged Vivado preflight/BD scripts ready; no board measurement yet
- P1.3 ASIC area/power envelope — **DOING**; `soc/docs/results/asic_envelope.md` has initial capacity/storage analysis; area/power bounds and A0 remain pending

### Phase 2: Software path (hook + repack)
- P2.1 Repack library — **DONE**
- P2.2 GEMV hook + weight cache — **DONE**
- P2.3 Emulation backend — **DONE** (board validation pending)
- P2.4 Per-op profile with hook (board) — TODO

### Phase 3: HLS accelerator
- P3.1 GEMV HLS kernel v1 — **DONE (source + Vitis C simulation)**; first synthesis reviewed; baseline RTL passes
- P3.2 Testbench — **DONE (testbench + Vitis C simulation)**; RTL validation remains P3.3
- P3.3 HLS synthesis + design exploration (LRC) — **DOING**; v1 synthesis reviewed, main II=5, baseline RTL passes; eight-row candidate Vitis C/RTL pass and compute II=1; further exploration pending
- P3.4 (Optional) SystemC/TLM co-verification — TODO

### Phase 4: Integration
- P4.1 Vivado block design — **DOING**; source/guards written; vendor validation and matching PS config pending
- P4.2 HAL v2 / FPGA backend — **DOING**; common HAL and optional Linux adapter written/tested locally; board support/target execution pending
- P4.3 Board bring-up — TODO; target smoke test prepared, not run

### Phase 5: Optimization
- P5.1 Final benchmark run — TODO
- P5.2 Optimization queue — TODO

### Phase 6: Deliverables
- P6.1 PRD final — TODO
- P6.2 Performance/power model — TODO
- P6.3 submission.json + repo cleanup — TODO
- P6.4 Final slides — TODO
- P6.5 Peer contribution report — TODO

---

## Key numbers to remember

| Fact | Value |
|---|---|
| Weights read per token | 524,833,792 bytes (~525 MB) |
| Rows computed per token | 456,064 |
| GEMV calls per token (fused) | 97 |
| GEMV share of decode time | 89% (gprof, 300 MHz, 1 core) |
| Repack overhead | ~0.002% size increase (alignment padding only) |
| Memory budget (steady state) | ~855 MB of 2 GB |
| HP port peak BW (150 MHz) | 2.4 GB/s (theoretical) |
| BW needed for 0.5 tok/s | 262 MB/s |
| BW needed for 2 tok/s | 1.05 GB/s |
| FPGA clock target | 150 MHz (conservative), 200 MHz (stretch) |
| ASIC area budget | 0.5 mm² |
| Power budget | < 100 mW |

---

## For LLM context transfer

When starting a new LLM session for this project, provide files in this order:

1. **This file** (`soc/docs/README.md`) — gives the map
2. **`SOC_PROJECT_PLAN.md`** — the full plan with all context
3. **`soc/docs/ARCHITECTURE_NOTES.md`** — design rationale and hardware details
4. **The specific README** for whatever code area you're working on (repack, gemv, etc.)
5. **Any results files** relevant to the current task

For a quick session that just needs to write code, files 1-3 are usually sufficient. For board work, add `PROFILING_GUIDE.md`.

### Constraints to tell every session

- Keep only one master plan: `llama.cpp/SOC_PROJECT_PLAN.md`. Do not create copies.
- Claude sessions cannot commit. Leave changes in working tree and say what to commit.
- Do not push to upstream origin (ggml-org).
- Project code goes under `soc/`. Upstream file edits are minimal and inside `#ifdef GGML_SOC_GEMV`.
- Cross-compile on LRC, run on board. Mac/LRC are wrong architectures for performance numbers.
- Due date: 2026-12-08.
