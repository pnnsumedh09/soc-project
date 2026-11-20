# Architecture Notes: Edge LLM GEMV Accelerator

Reference document for understanding the hardware/software design decisions.
Read after `SOC_PROJECT_PLAN.md` sections 1 and 6.

---

## 1. The bottleneck: why GEMV

Decode (autoregressive token generation) multiplies each weight matrix by a single activation vector. That is a matrix-vector product (GEMV), not a matrix-matrix multiply (GEMM). There are 169 such GEMVs per token (97 with QKV + gate/up fusion).

gprof at 300 MHz / 1 core: `ggml_vec_dot_q8_0_q8_0` = **89%** of decode time (130 s out of 145 s, 25.4 M calls). Everything else (graph dispatch, attention, tokenization) is ~10%.

Each token reads **524,833,792 bytes** of weights from memory — the entire model, once per token. The operation is memory-bandwidth-bound: each weight byte is used exactly once per token (no reuse).

---

## 2. Zynq UltraScale+ memory architecture

```
                    ┌─────────────────────────────────────┐
                    │          Zynq UltraScale+ SoC       │
                    │                                     │
                    │  ┌──────────────────────────────┐   │
                    │  │      PS (Processing System)   │   │
                    │  │                              │   │
                    │  │  ARM A53 ◄──► L2 ◄──► CCI   │   │
                    │  │                       │      │   │
                    │  │              DDR Controller   │   │
                    │  │                  │            │   │
                    │  │   HP0  HP1  HP2  HP3          │   │
                    │  │    ▲    ▲    ▲    ▲           │   │
                    │  └────┼────┼────┼────┼───────────┘   │
                    │       │    │    │    │               │
                    │  ┌────┼────┼────┼────┼───────────┐   │
                    │  │    │    │    │    │  PL (FPGA) │   │
                    │  │  AXI Master interfaces        │   │
                    │  │  (your HLS IP connects here)  │   │
                    │  └──────────────────────────────┘   │
                    └─────────────────────────────────────┘
                                     │
                              Physical DDR pins
                                     │
                              ┌──────┴──────┐
                              │  DDR4 DRAM  │
                              │   (2 GB)    │
                              └─────────────┘
```

Key points:

- **DDR is physically wired to the PS only.** There are no direct traces from the FPGA fabric to the DDR chips on the Ultra96 board.
- The PL (FPGA) accesses DDR through **HP (High Performance) ports** — these are AXI slave interfaces exposed by the PS, designed for bulk PL-to-DDR data transfer.
- The data path for an FPGA read: `HLS IP m_axi → HP port → PS interconnect → DDR controller → physical DDR`.
- The ZU3EG has 4 HP ports (HP0-HP3). Each is 128 bits wide.
- Theoretical peak bandwidth of one HP port at 150 MHz: `128 bits × 150 MHz = 2.4 GB/s`.
- Real bandwidth depends on burst length, outstanding transactions, CPU contention, CMA allocation, and clock speed. We estimate 0.5-3.2 GB/s (must measure — this is task P1.2).

This path is **hardwired in the Zynq silicon**. No special board traces needed. You just enable the HP port in the Vivado block design and connect your AXI master to it.

---

## 3. Lab 3 architecture (slave) vs our project design (master)

### Lab 3: CPU pushes data to a slave accelerator

```
Q8_0 blocks in DDR
       │
       ▼  (CPU reads via normal load instructions)
HAL: stack buffers (de-interleave Q8_0 → separate quants + scales)
       │
       ▼  (ioctl system call)
Driver: kernel heap copy (kzalloc)
       │
       ▼  (writel() — one 32-bit MMIO write per 4 bytes)
Accelerator BRAM (AXI-Lite slave registers)
       │
       ▼  (accelerator computes internally)
Result register ──► driver reads back ──► HAL returns float
```

The accelerator is an **AXI-Lite slave**. It has on-chip BRAM that the CPU fills via MMIO writes. The accelerator never touches DDR. For one 896-element row (28 blocks):
- 28 × 32 / 4 = 224 writes for xq, 224 for yq, 14 for xd, 14 for yd = **476 uncached 32-bit MMIO writes**
- Each is a full AXI-Lite transaction (address phase + data phase + response)
- Multiply by 456,064 rows per token → completely impractical for the real model

Where the local copies live:
- **HAL** (`dot_hal.c`): `int8_t xq[DOT_XQ_BYTES]` etc. on the **stack** of `dot_hal_dot_q8_0()`
- **Driver** (`dot_accel_drv.c`): `struct dot_inputs` allocated with **`kzalloc`** (kernel heap), filled by `copy_from_user()`
- **Accelerator**: BRAM region mapped as `d->mem`, written by `dot_load()` → `writel()`

### Our project: accelerator reads DDR itself (AXI master)

```
Repacked weights in DDR (4 KB-aligned quants + scales arrays)
       │
       │  (accelerator reads via AXI HP port — 128-bit bursts)
       ▼
GEMV engine (AXI master, reads weights autonomously)
       │
       │  x vector loaded once into on-chip BRAM (small: 896 int8 = 896 B)
       ▼
Dot-product lanes (MAC array) ──► accumulate ──► scale ──► y[row]
       │
       ▼  (write result to output BRAM or AXI)
CPU reads y vector (small: at most 4864 floats = 19 KB)
```

The CPU sends a **short descriptor** (base address, dimensions, stride) via AXI-Lite control registers. The accelerator's AXI master port reads weight data directly from DDR through the HP port. No per-element MMIO, no stack copies, no kernel heap copies.

Per-call CPU cost: ~10 register writes (descriptor) + 1 start + poll for done. Versus Lab 3's 476+ MMIO writes per row.

---

## 4. Data format: Q8_0 and repack

### Native Q8_0 (as stored in the .gguf file)
Each block = 34 bytes: `[FP16 scale (2 B)][32 × int8 weights (32 B)]`

Problem: 34 bytes is not a multiple of 16 (the AXI beat width). Hardware would need complex realignment logic.

### Repacked format (in-memory only, .gguf untouched)
Two separate arrays per matrix:
- `quants[]`: 32 B per block (exactly 2 AXI beats) — contiguous, 4 KB-aligned
- `scales[]`: 2 B per block — contiguous, 4 KB-aligned

This lets the hardware stream weights with simple INCR bursts, no realignment. The repack is done once when the model loads. Size overhead is only alignment padding (~0.002%).

### Golden arithmetic (bit-exact reference)
```
for each block b:
    sumi += sum(w_quants[b][i] * x_quants[b][i])  // int32 accumulation
sumf += (float)sumi * fp32(w_scale[b]) * fp32(x_scale[b])  // FP32, no FMA
```
No fused multiply-add. Matches `ggml_vec_dot_q8_0_q8_0_generic` with `-ffp-contract=off`.

---

## 5. Bandwidth test (P1.1) rationale

Before building the full GEMV kernel (weeks of work), we build a trivial ~50-line HLS IP that:
1. Takes a DDR address and byte count via AXI-Lite registers
2. Reads that many bytes from DDR through the HP port using its `m_axi` interface
3. Counts clock cycles
4. Optionally computes a checksum

Purpose: measure **actual sustained HP port bandwidth** under realistic conditions. This takes ~1 day to build and synthesize. The measurement (P1.2) sweeps:
- Burst length (1, 4, 16, 64, 256 beats)
- Outstanding transactions
- Clock speed (100, 150, 200 MHz)
- 1 vs 2 HP ports
- With/without CPU activity

The result directly sets the tok/s ceiling. If bandwidth is too low, we know before investing in the full accelerator. Required bandwidth for 0.5 tok/s: `525 MB × 0.5 = 262 MB/s` (easily achievable). For 2 tok/s: `1.05 GB/s` (needs good burst behavior).

---

## 6. Memory budget

| Component | Size | Notes |
|---|---|---|
| Repacked weights | ~525 MB | quants + scales for all 169 matrices |
| Non-weight GGUF (embeddings, norms, biases) | ~80 MB | mmapped, shared with original |
| KV cache | ~50 MB | at max context (varies) |
| llama-server process | ~100 MB | heap, stacks, code |
| OS + kernel | ~100 MB | PetaLinux |
| **Total** | **~855 MB** | Fits in 2 GB, but CMA config is the risk |

CMA (Contiguous Memory Allocator) is needed for DMA-able weight buffers. If CMA is too small or fragmented, fallback options: selective repack (only the largest matrices), native Q8 streaming (hardware handles 34 B blocks), or per-tensor DMA allocation.

---

## 7. Promotion gates

Before finalizing the architecture, four gates must pass:

| Gate | Question | How to answer |
|---|---|---|
| M0 | Is Qwen2.5-0.5B Q8_0 still the best model? | Model sweep at 300 MHz (P0.3) |
| D0 | Does the DMA data path work? | Bandwidth test on board (P1.2) |
| B0 | Is HP port bandwidth sufficient for target tok/s? | Bandwidth measurement vs formula |
| A0 | Does the accelerator fit in 0.5 mm² at 45/28 nm? | ASIC area estimate (P1.3) |

---

## 8. Clock and area targets

- FPGA clock: **150 MHz** (conservative first), 200 MHz stretch goal
- MAC lanes: 16-wide (baseline), explore 4/8/16 for area trade-off
- ASIC area budget: 0.5 mm² at 45 nm or 28 nm
- Power budget: < 100 mW additional

---

## 9. Call count per token

| Scenario | Calls/token |
|---|---|
| Unfused (every matmul separate) | 169 |
| QKV + gate/up fused | 97 (4 calls/layer × 24 layers + 1 LM head) |
| Minimum (all fusible ops fused) | 73 |

Calculation for 97: each layer has 7 matmuls, fusing QKV saves 2, fusing gate+up saves 1 → 4 per layer. 4 × 24 + 1 (output) = 97.
