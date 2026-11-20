# Edge LLM inference SoC: PRD v0.1

Date: 2026-11-19. Status: initial requirements draft (P0.7), not a validated product specification. Source of course requirements: `reference/project_handout_text.txt`. `../../SOC_PROJECT_PLAN.md` remains the master architecture, decisions and task-status document. Update this PRD when model, memory, bandwidth and ASIC gates are evaluated; finalize under P6.1.

## Intended use

Run basic language-model inference locally on a resource-constrained edge device. The target ASIC uses one Cortex-A53 at 300 MHz plus custom accelerator logic in 45 nm or 32/28 nm technology. The prototype is an Ultra96-V2/ZU3EG with shared 2 GB DDR. The course benchmark (ARC-Easy, HellaSwag and IFEval mix) provides the measurable task-quality target; no separate application-specific quality claim is made.

The candidate model is Qwen2.5-0.5B-Instruct Q8_0. Final model/quantization selection remains open until board results at the required CPU configuration exist. No context-length or prompt-length limit is finalized yet; it must fit the official benchmark and measured memory budget.

## Requirements and acceptance evidence

"Essential" means required for acceptance. "Desirable" means useful if essential requirements remain satisfied. "Optional" means explicitly outside the initial critical path. Paths below are expected evidence destinations; missing result files do not imply acceptance.

| ID | Priority | Requirement | Acceptance and evidence |
|---|---|---|---|
| R1 | Essential | Decode speed >= 0.5 token/s with one A53 core at 300 MHz plus the accelerator | Record actual core/frequency state, Release build, model hash, context and measured decode rate in `results/final_bench.md`; compare with `results/baseline_300mhz.md` |
| R2 | Essential | Official benchmark accuracy score > 20.0 | Run the course's actual benchmark interface against the final server/model/backend; preserve per-task and combined scores and command in `results/final_bench.md` |
| R3 | Essential | Accelerator ASIC area within the 0.5 mm^2 project budget | Early bounded estimate in `results/asic_envelope.md`; final process-specific model in `perf_power_model.md`, including compute, SRAM, FP units, interconnect engines/FIFOs and control. The <= interpretation is the working budget interpretation of the handout's 0.5 mm^2 metric |
| R4 | Essential | Additional system power for accelerators < 100 mW | State process, frequency, activity and included components in `perf_power_model.md`; clarify whether incremental DDR power is included. FPGA board power alone is not an ASIC power result |
| R5 | Essential | Complete every autograder benchmark within 30 minutes | Record full suite wall time with the submitted configuration and preserve autograder results in `results/final_bench.md` |
| R6 | Essential | Fit the prototype's 2 GB memory and support real device-accessible buffers | Record resident/peak memory, swap behavior, CMA/reserved-memory setup and actual per-matrix allocation sequence in `results/board_memory.md`. Design target: no swap/OOM during evaluation |
| R7 | Essential | Correct HW/SW execution, with supported cases accelerated and unsupported cases handled by the CPU | Verify arithmetic against the golden model, test cache/fallback and protocol behavior, then compare real-hardware inference/score with CPU reference. Preserve HLS co-simulation and board logs under `results/` |
| R8 | Essential | Reproducible course submission | Include modified llama.cpp, HLS/RTL and integration sources, matching BIT/HWH, build/run documentation and schema-valid top-level `submission.json`; verify Classroom main-branch grading flow before final submission |
| D1 | Desirable | Improve end-to-end speed over a measured CPU-only baseline | Use identical model, context, sampling/build settings and CPU state; report startup and steady-state behavior separately |
| D2 | Desirable | Preserve useful execution when an optional offload allocation or job fails | Demonstrate CPU fallback, error reporting and a strict diagnostic mode; define recovery after real hardware errors before integrating the FPGA backend |
| D3 | Desirable | Keep changes maintainable and measurable | Isolate project code under `soc/`; retain minimal guarded upstream integration, runtime offload disable and per-experiment source/configuration records |
| O1 | Optional | QKV/gate-up fusion, additional lanes/ports, alternative precision, attention/elementwise offload | Implement only when measurements show value and essential acceptance checks are repeated |
| O2 | Optional pending course clarification | New GEMV SystemC/QEMU virtual platform | Use for protocol/software validation if required or useful; it complements generated-RTL and board checks |

## Initial HW/SW partition

The primary candidate accelerates whole decode matrix-vector products `y = W x`. CPU software keeps tokenizer, embedding lookup, activation quantization, norms, RoPE, KV management, attention, SwiGLU, residuals and sampling. Initial bias addition stays in the existing CPU graph; hardware bias fusion is optional.

The existing CPU hook accepts supported Q8_0 weight/F32 activation/F32 output cases with one activation column and suitable layout. It reuses ggml's Q8_0 activation conversion. Multi-column prefill and unsupported types/layouts continue through the CPU path; a single-column output projection during prefill can also qualify. The hook must work with the server's thread settings, not just a single-thread CLI run.

The existing repack separates 34-byte Q8_0 weight blocks into quant and FP16 scale arrays, without modifying the model file. The backend owns allocation and device addresses. The proposed FPGA reads weights from DDR through a non-coherent HP port, loads the activation vector into local storage and writes FP32 results back to DDR. Reuse the current `init/shutdown/alloc/free/run` backend interface; the final FPGA register map and recovery behavior remain to be defined after P0.5/P1.2.

Verified tensor census: 24 layers, hidden dimension 896, FFN dimension 4864, separate output and embedding matrices, 169 decode GEMVs per token, approximately 525 MB of matmul weights read per token. The embedding table is a separate Q8_0 tensor and not a GEMV. QKV plus gate/up fusion would reduce calls to 97 but is not implemented or assumed for baseline acceptance. See `results/model_dims.md`.

## Data integrity and memory contract

- Golden arithmetic: integer block products followed by FP32 scale multiplication and ordered FP32 accumulation, with separate multiply/add and no FMA. Hardware should match `soc_q8r_gemv_ref` bit-exactly for the supported finite inputs; any relaxed numerical mode requires an explicit error bound and repeated benchmark acceptance.
- NEON uses a different accumulation order. Compare absolute and relative errors and generated-output/benchmark behavior; do not demand bit-exact NEON agreement as a substitute for the golden test.
- The current heap-backed emulation allocator is not a DMA allocator. Board allocations must provide real device addresses, adequate lifetime/alignment and a defined cache-maintenance strategy. Never submit ordinary CPU virtual addresses to the engine.
- CPU writes must be visible before a device read; device output must be visible before CPU consumption. Non-coherent HP use requires an approved flush/invalidate or non-cacheable strategy. Weight buffers can be flushed once after repack; activations/results require per-job treatment.
- Do not release or repurpose buffers until DMA has completed or the engine has been safely stopped/reset. Define timeout/error recovery before final HAL integration.
- Keep the GGUF mapping and necessary CPU fallback data accessible. Repacked memory may coexist with reclaimable model page-cache pages; confirm actual pressure on the board rather than assuming immediate reclamation.

## Promotion gates and alternatives

| Gate | Required evidence | Current status |
|---|---|---|
| M0: model | Score margin, memory fit and suite duration at the target CPU setting; model/quant sweep | Pending P0.2/P0.3 |
| D0: data path | Successful DMA-visible allocations for the actual matrix sequence, or a measured viable selective layout, with server loaded | Pending P0.6 |
| B0: bandwidth | Sustained PL read bandwidth and software overhead supporting useful end-to-end performance | P1.1 source exists; vendor/board validation and P1.2 measurements pending |
| A0: ASIC envelope | Plausible complete accelerator area/power at an identified process, including uncertainty | Pending P1.3 |

Model changes, selective repacking, native Q8_0 hardware unpacking or smaller compute configurations remain alternatives if a gate fails. The provisional 16-wide/150 MHz FPGA configuration is a design exploration point, not evidence of ASIC fit or achieved speed. Clock/lane choices must follow reports and measured bandwidth.

## Build versus reuse

Reuse llama.cpp/ggml CPU operations, the completed repack/cache/backend interface, Lab 2's denormal-safe FP16 conversion, and validated course PS configuration. Build the bus-mastering GEMV engine and its FPGA backend. Reuse Lab 3's software/control patterns where compatible, but its slave-only per-dot-product model is not the new DMA/GEMV design.

The P1.1 read-only experiment is deliberately separate from final inference. It provides a bandwidth baseline and timing instrumentation. It does not prove GEMV arithmetic, real weight-cache capacity, full-system coherence or compliance with the MRD.

## Verification and integration policy

Independent source work may proceed in parallel with target runs. Each path advances through evidence checkpoints: native golden-reference tests; Vitis C simulation; synthesis review and RTL co-simulation; block-design validation and implementation timing; a known single transfer/GEMV on the board; repeated jobs and cache/fallback checks; full inference; official benchmark.

HW/SW co-verification is required by the handout. The provided project handout does not explicitly require QEMU before board loading. Initial strategy: use HLS/RTL checks and real-board reference comparisons. Confirm the tutorial/rubric before excluding a required virtual-platform deliverable. If a new QEMU model is added, first test tiny GEMVs and then short TinyStories runs; QEMU/SystemC is not a substitute for RTL co-simulation, physical bandwidth or timing closure.

## Open assumptions and limits

The board tutorial, final autograder access/clock/thread rules, allowed allocator/driver mechanism and submission schema are not yet verified in this checkout. PYNQ is an optional runner assumption, not an established course requirement. Official prompt/context lengths and DDR-power accounting remain unresolved. ASIC estimates must remain labeled estimates; sandbox correctness tests and emulation timings are not board performance results.

The Lab 1 profile found about 89% in the Q8_0 dot-product routine for its measured run. This motivates GEMV offload but does not establish the exact remaining CPU fraction for every benchmark or context. Re-profile the relevant workload, and use end-to-end measured speed for acceptance.
