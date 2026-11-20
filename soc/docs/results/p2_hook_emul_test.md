# P2.2 / P2.3: hook + weight cache + emul backend, end-to-end test

Date: 2026-11-18. Who: Claude session (with Sumedh). Where: Claude sandbox, aarch64 host (Neoverse-class CPU), built with `-mcpu=cortex-a53` so llama.cpp uses the same NEON code paths as the Ultra96 (no dotprod/i8mm). Built with `soc/tools/build_nocmake_sandbox.sh` (no cmake in the sandbox), CPU repack off, llamafile on. Model `qwen2.5-0.5b-instruct-q8_0.gguf`. Program `llama-simple` (greedy), 4 threads (default). Timing numbers here are host numbers, NOT Ultra96 numbers.

## Correctness

| Test | Result |
|---|---|
| "Describe a system-on-chip in two sentences.", 48 tokens: stock vs hook (emul) | **identical text** |
| "The capital of France is", 32 tokens: stock vs hook | **identical text** |
| Hook binary with `SOC_GEMV=0` vs stock | identical text, 0 offloaded calls |
| Matrices cached | **169** (24 layers x 7 + LM head), 525.2 MB, repack 0.126 s on host, 0 fallbacks, 0 stale repacks |
| `SOC_GEMV_VERIFY=1` (emul vs llama.cpp NEON `vec_dot`), 16 tokens | 2,539 GEMVs, 7.0 M rows; max abs diff 2.67e-5; max diff / max abs y 7.1e-7; 21.75% rows bit-identical |
| Golden vs llama.cpp `ggml_vec_dot_q8_0_q8_0_generic`, 26,624 real rows (8 tensors x 512 rows x 8 random x) | default llama.cpp build: 39.5% bit-identical (GCC fuses into `fmadd`); llama.cpp `quants.c` rebuilt with `-ffp-contract=off`: **100% bit-identical** |
| `test_repack` built for `-mcpu=cortex-a53`, run natively | all pass |
| `test_gemv` (ASan + UBSan) | all pass, 100% bit-exact vs reference |

## Call counts (48-token run)

7,947 offloaded GEMVs = 169 per decoded token x 47 decode steps, plus the prompt's last-token LM head and warm-up. 21.6 M rows; 24.8 GB of repacked weights read in total (about 525 MB per token, as predicted in `model_dims.md`).

## Speed (host only, for orientation)

Stock NEON, 4 threads: 11.6 ms/token decode. Emul backend: 171 ms/token (single-threaded scalar golden model, 3.1 GB/s effective on this host). The emul backend is a correctness model, not a performance model; real numbers come from the FPGA (P4) and the 300 MHz baseline on the board (P0.2).

## Not yet tested

- The CMake integration (`-DGGML_SOC_GEMV=ON`) on the LRC with the Ultra96 toolchain.
- Running on the Ultra96 itself, and with `llama-server` / `benchmark.py` (score check).
