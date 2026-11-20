# soc/sw/repack: Q8_0 weight repack library (plan task P2.1)

## What it does

llama.cpp stores each Q8_0 weight matrix as rows of 34-byte blocks (`uint16` FP16 scale followed by 32 `int8` weights). A 34-byte block does not line up with the 16-byte (128-bit) beats of the AXI HP port, so the hardware would have to realign data constantly.

This library converts a matrix **once, in memory** (after llama.cpp has loaded/mmapped the model) into two arrays the GEMV engine can stream with plain bursts:

```
native (ggml):   [d0|q0 x32][d1|q1 x32] ...        34 B per block, rows of nb blocks
repacked:        quants: [q0 x32][q1 x32] ...      32 B per block = exactly 2 beats
                 scales: [d0][d1][d2] ...          2 B per block, same order
```

Block `(r, b)` (row r, block b, `nb = in/32` blocks per row) is at `quants[(r*nb + b)*32]` and `scales[r*nb + b]`. Both arrays are 4 KB-aligned and their sizes are rounded up to 4 KB with zero padding (AXI bursts must not cross a 4 KB boundary). Scales are copied bit-for-bit, so the conversion is lossless and reversible. The `.gguf` file on disk is never touched.

Size cost is only padding: the full LM head (`output.weight`, 151,936 x 896) goes from 144,643,072 B to 144,646,144 B.

## Files

| File | Purpose |
|---|---|
| `soc_repack.h/.c` | Library: size helpers, `soc_q8r_pack`/`unpack`, `soc_q8r_init` (caller-provided buffers, e.g. from the HAL), `soc_q8r_alloc_host` (heap, for emulation/tests), and the reference arithmetic (`soc_fp16_to_fp32`, `soc_q8r_row_dot`, `soc_q8r_gemv_ref`, `soc_q8_0_row_dot_native`). |
| `test_repack.c` | Unit tests: lossless round trip, layout, alignment and zero padding, bit-exact GEMV on repacked vs native layout, error vs a double-precision reference, exhaustive FP16 conversion, argument validation. Optional real-tensor files. |
| `../../tools/dump_q8_tensor.py` | Dumps Q8_0 tensors (or the first N rows) from the GGUF into small `.bin` files for tests and, later, HLS testbenches. Read-only on the GGUF. |
| `Makefile` | `make test`, `make test-real`, cross-build with `CC=aarch64-linux-gnu-gcc`. |

## How to run

```bash
cd llama.cpp/soc/sw/repack
make test                    # synthetic tests
make test-real               # also dumps 8 real Qwen tensors (needs python3 + numpy) and tests them
make clean
# full tensors, e.g. the whole LM head:
cd ../../.. && PYTHONPATH=gguf-py python3 soc/tools/dump_q8_tensor.py ../qwen2.5-0.5b-instruct-q8_0.gguf /tmp/q8 --tensor output.weight --rows 0
soc/sw/repack/test_repack /tmp/q8/output_weight.bin
```

Results on 2026-11-18 (x86 host, gcc 11.4): all tests pass; every Qwen matmul shape (attn_q/k/v/output, ffn_gate/up/down, full output.weight) round-trips exactly and the repacked GEMV is bit-identical to the native-layout GEMV. Also cross-compiles for `-mcpu=cortex-a53` (not yet run on the board).

## Arithmetic (golden model)

`soc_q8r_row_dot` uses exactly the operation order of `ggml_vec_dot_q8_0_q8_0_generic` (this llama.cpp version, `ggml/src/ggml-cpu/quants.c`): per block an int32 sum of 32 products, then `sumf += (float)sumi * (fp32(w.d) * fp32(x.d))`, blocks in order. It is the reference the hardware must match bit-exactly, so the hardware should also use separate FP32 multiply and add (no fused multiply-add), and the library is compiled with `-ffp-contract=off`. llama.cpp's NEON kernel (what actually runs on the A53 today) sums in a different order, so hardware vs NEON differences in the last bits are expected and acceptable.

## Not in this library (on purpose)

- Deciding *where* memory comes from: the caller passes buffers to `soc_q8r_init`. On the board this will be the HAL v2 contiguous allocator (P4.2); on the CPU-only path, `soc_q8r_alloc_host`.
- The per-tensor cache (repack on first use, keyed by `src0->data`): that belongs to the llama.cpp hook / emulation backend (P2.2, P2.3).
