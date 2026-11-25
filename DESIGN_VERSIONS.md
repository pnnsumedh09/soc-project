# FPGA Design Versions

Use immutable tags where available for grading and reproduction. For untagged
checkpoints, use the source commit listed below; branch heads can change.
This index covers all ten remote branches as of 2026-11-25.

## Validated designs

| Design | Development branch | Validated tag | Status |
|---|---|---|---|
| Golden Q8_0 GEMV | `gemv-golden` | `ultra96-q8-gemv-golden-v1` | Validated |
| Q8_0 GEMM/GEMV, N≤8 | `gemm-compiletime-quant` | `ultra96-q8-gemm-n8-validated-v1` | Ultra96-V2 validated |
| Q4_0 GEMM/GEMV, grouped 100 MHz | `gemm-compiletime-quant` | Not tagged | Ultra96-V2 correctness validated; submitted design |

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

### Q4_0 shared-engine hardware candidate

- Branch: `gemm-compiletime-quant`
- Commit: `fe250ce7fb844cd5057abbf12c6458e84e376485`
- Status: submitted, board-validated grouped 100 MHz design. This commit is the
  pre-flattening hardware source; the later documentation commit is not needed to build it.
- Architecture: one shared FIFO set, activation store, 32-element dot-product
  pipeline, and eight-context accumulator bank. Contexts represent rows for
  N=1 and columns for N>1.
- The shared FIFOs use depth 152, equal to one maximum-width row per lane.
- Q4_0 C simulation and RTL co-simulation passed, including N=1 and N>1 test
  shapes. The `shared_compute_slots` loop achieved target II=1 and final II=1.
- HLS top estimate: 144 BRAM18, 36 DSP, 13,383 FF, and 15,215 LUT.
- The 100 MHz Ultra96-V2 implementation met timing with setup WNS 4.088 ns,
  TNS 0, hold WHS 0.010 ns, and THS 0.
- Vivado implementation used 51.5 Block RAM tiles, 49 DSPs, 9,679 CLB LUTs,
  and 11,944 CLB registers.
- Relative to the split-datapath implementation, this reduces implemented
  Block RAM tiles by 29.5% and DSPs by 44.9%.
- Ultra96-V2 Q4_0 model test: forced GEMV completed 7,101 N=1 offloads;
  the N≤8 test completed 1,161 offloads (171 N=1, 990 N>1, max N=8).
  Both had zero CPU fallbacks and zero stale repacks. The maximum relative
  FPGA/CPU verification error was 1.09e-6 and 1.08e-6 respectively.
- The persistent FPGA cache held 168 matrices using 201.6 MB.

### Q4_0 flattened shared engine, 150 MHz hardware candidate

- Branch: `gemm-q4-flatten-f150`
- Hardware source commit: `102f3eeb6905c5810926258cdfeccb59974ae8ed`
- Status: C simulation, HLS synthesis, RTL co-simulation, and Vivado
  implementation passed. Board correctness and performance are pending.
- One compute pipeline spans all row groups in each activation tile. Final
  results enter an output stream consumed by a concurrent AXI writer.
- At N≤8, there is one compute-pipeline run for the entire accelerator job.
  Both `shared_compute_slots` and `write_shared_output_packets` have final II=1.
- The 150 MHz implementation uses PL0 period 6.666 ns and meets timing:
  setup WNS 1.027 ns, TNS 0; hold WHS 0.010 ns, THS 0.
- The 100 MHz flattened implementation also passed timing and used 51.5
  Block RAM tiles, 48 DSPs, 9,798 CLB LUTs, and 12,119 CLB registers.

## Development branch index

| Branch | Purpose | Validation status |
|---|---|---|
| `soc-gemv-accel` | Default integration branch and design documentation | Active |
| `gemv-golden` | Stable golden GEMV design | Validated |
| `gemm-compiletime-quant` | Compile-time Q8_0/Q4_0 GEMM development | Q8_0 and grouped Q4_0 board validated |
| `gemm-q4-flatten-f150` | Flattened Q4_0 shared engine at 150 MHz | HLS/RTL and Vivado passed; board validation pending |
| `gemv-q4-0` | Standalone Q4_0 GEMV and build handoff | Development; board validation not recorded here |
| `gemv-q4` | Standalone Q4_K_M GEMV | Development; board validation not recorded here |
| `gemv-activation-quant` | On-chip FP32 activation quantization for Q8_0 GEMV | Board experiment; no meaningful speedup in recorded run |
| `combination-a-feasible` | Eight-row GEMV interleaving, double buffering, and counters | Experimental profiling branch |
| `qemu-gemv-ng` | QEMU/SystemC GEMV model and testbench | Simulation development branch |
| `profiling/lrc-baseline` | LRC CPU baseline profiles for Qwen and LFM2 | Profiling data; no separate FPGA design |

## Other branch checkpoints

### Standalone Q4_K_M GEMV

- Branch: `gemv-q4`
- Source commit: `28308437c430208eb6ebd028e2cbe6e66d45faf2`
- Adds Q4_K_M GEMV hardware, software, repacking, and a build/board run guide.
  This is a separate GEMV development line, not Q4_K_M support in the GEMM
  engine. No validated tag or board result is recorded in this index.

### Standalone Q4_0 GEMV

- Branch: `gemv-q4-0`
- Q4_0 implementation commit: `f2a85d871f66317716f788e8d20e6ce484b64515`
- Branch head: `447472edfd2a19cc2435c6e37da84f6f521ffc77`
- Extends the standalone GEMV line with Q4_0 hardware, software, tests, and
  `Q4_0_VITIS_VIVADO_HANDOFF.md`. The handoff describes validation steps; it
  does not establish that a board run passed. This branch is distinct from the
  board-validated Q4_0 GEMM/GEMV shared engine above.

### On-chip activation quantization

- Branch: `gemv-activation-quant`
- Candidate source commit: `4a14a8729960bd3153a9deaaf40385bf7b086d3a`
- Reads FP32 activations and quantizes them on the FPGA before Q8_0 GEMV.
  The branch's `Activation_Recomputation_result_analysis` records a successful
  Ultra96 run with 2,547 offloads and zero fallbacks. Backend time changed
  from 36.818 s to 36.727 s against golden GEMV in one comparison; both
  displayed 0.4 generation tokens/s. Treat accuracy and speed as experimental.

### Eight-row GEMV and double-buffer experiment

- Branch: `combination-a-feasible`
- Row-interleave and profiling commit: `222cd6c56ea5e8f13c8c3d525077c66a3da1612d`
- The branch documents eight-row interleaving, two 16-block weight/scale
  banks, and PL cycle and logical traffic counters in
  `soc/docs/COMBINATION_A_R8_COUNTERS_END_TO_END.md`. It uses UIO and
  `u-dma-buf`, unlike the XRT-backed GEMM runtime. Its workflow and profiling
  experiments are separate from the validated Q8 and Q4 GEMM checkpoints.

### QEMU/SystemC GEMV model

- Branch: `qemu-gemv-ng`
- Initial QEMU/SystemC source commit: `f61de57243adf78126f13d1d795c2117f7573c5e`
- Models and tests the GEMV integration in QEMU/SystemC. This is a simulation
  branch, not a separate board-validated bitstream.

### LRC CPU baseline profiling

- Branch: `profiling/lrc-baseline`
- Profiling results commit: `22128261c60538de899623c91838663776e70091`
- Stores Qwen and LFM2 CPU `gprof` and Callgrind baseline reports under
  `profiling-results/lrc-baseline`. It branches from the shared Q4 hardware
  checkpoint but does not add another accelerator design.

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
