# FPGA Design Versions

Use immutable tags for grading and reproduction. Development branches may
continue changing after a design has been validated.

## Validated designs

| Design | Development branch | Validated tag | Status |
|---|---|---|---|
| Golden Q8_0 GEMV | `gemv-golden` | `ultra96-q8-gemv-golden-v1` | Validated |
| Q8_0 GEMM/GEMV, N≤8 | `gemm-compiletime-quant` | `ultra96-q8-gemm-n8-validated-v1` | Ultra96-V2 validated |
| Q4_0 GEMM/GEMV | `gemm-compiletime-quant` | Not validated/tagged | HLS/RTL and bitstream build passed; shared-engine refactor and software/board validation pending |

## Development checkpoints

### Q4_0 split-datapath area baseline

- Branch: `gemm-compiletime-quant`
- Tag: `q4-split-datapath-area-baseline-v1`
- Commit: `162a346c698915cc0dbd68e32c2b1a4b4f52f495`
- Status: non-final measurement baseline; do not use as the area-constrained design.
- Functionality: N=1 uses an eight-row-interleaved DATAFLOW network; N>1
  uses the existing eight-column-reuse DATAFLOW network.
- Q4_0 C simulation and RTL co-simulation passed. The main N=1 compute loop,
  `compute_decode_block_rows`, achieved target II=1 and final II=1.
- The 100 MHz implementation met timing with setup WNS 4.874 ns and hold
  WHS 0.010 ns.
- HLS estimated the N=1 network at 81 BRAM18 and 28 DSP, the N>1 network at
  100 BRAM18 and 35 DSP, and the complete top at 181 BRAM18 and 63 DSP.
  The exact summation confirms that the arithmetic and buffering were not
  shared between the two runtime-selected networks.
- Vivado implementation used 73 Block RAM tiles and 89 DSPs.
- N=1 FIFO depth is 304 entries (two maximum-width rows); N>1 FIFO depth is
  2432 entries (sixteen maximum-width rows).
- Successor: replace both DATAFLOW networks with one shared dot-product,
  accumulator, FIFO, activation-storage, and output datapath. Interpret the
  eight contexts as rows for N=1 and columns for N>1.

## Development branch index

| Branch | Purpose | Validation status |
|---|---|---|
| `soc-gemv-accel` | Default integration branch and design documentation | Active |
| `gemv-golden` | Stable golden GEMV design | Validated |
| `gemm-compiletime-quant` | Compile-time Q8_0/Q4_0 GEMM development | Q8_0 validated |
| `gemv-q4-0` | Q4_0 GEMV development | Needs status documentation |
| `gemv-q4` | Earlier Q4 development | Needs status documentation |
| `gemv-activation-quant` | Activation-quantization development | Experimental |
| `combination-a-feasible` | Earlier design exploration | Historical |
| `qemu-gemv-ng` | QEMU/GEMV development | Historical |

## Golden Q8_0 GEMV

- Branch: `gemv-golden`
- Tag: `ultra96-q8-gemv-golden-v1`
- Purpose: stable single-column FPGA GEMV implementation.

To reproduce:

```bash
git fetch origin --tags
git checkout ultra96-q8-gemv-golden-v1

```

## Q8_0 GEMM/GEMV

- Branch: `gemm-compiletime-quant`
- Tag: `ultra96-q8-gemm-n8-validated-v1`
- Commit: `e84dc04a305a3c9e3dbe9123520ab1638914a45b`
- Supports `N=1` GEMV and `N<=8` GEMM.
- Uses eight-column weight reuse, DATAFLOW prefetch, four HP ports, and a
  persistent FPGA weight cache.

To reproduce:

```bash
git fetch origin --tags
git checkout ultra96-q8-gemm-n8-validated-v1
```
