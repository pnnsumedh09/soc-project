# P3.1: Q8_0 GEMV HLS kernel v1

Status: provisional source implemented. Native synthetic and real-Qwen-slice tests are bit-exact against the existing golden reference; UndefinedBehaviorSanitizer passes. Vitis C simulation, synthesis and RTL co-simulation have not run for this kernel. No FPGA resource, achieved II, timing or bandwidth claim is made. P1.1's completed results apply to the separate bandwidth reader, not this GEMV engine.

## Data path

`soc_gemv_hw` computes `y = W x` for a whole matrix, with the existing repack and activation formats:

1. Load native Q8_0 activation blocks once per call, converting their scales using the unchanged Lab 2/3 `dot_half_to_float` function.
2. Cache activation quants as 128-bit words in local BRAM and activation scales as FP32 values.
3. For each row, burst-read its FP16 weight scales into a small local array and convert/check them.
4. Read weight quants sequentially as 128-bit words. Each beat computes 16 signed int8 products through an integer reduction tree. Two beats form one 32-element Q8_0 block sum.
5. Compute `(float)sumi * (fp32(w_scale) * fp32(x_scale))`, then add to the FP32 row accumulator in block order. Write each finished FP32 row to DDR.

Weights remain in the existing separate quant and scale arrays; no new repack format or GGUF modification is introduced. Biases, activation quantization and all other graph operations remain in the existing CPU path. This kernel is standalone: it is not yet registered as a backend or connected to the llama.cpp hook. The FPGA backend/HAL remains P4.2.

The baseline preserves the golden accumulation order rather than using independent FP partial sums. The read-loop `II=1` pragma is a scheduling request, not an achieved result. FP add recurrence every two beats can raise the achieved II; inspect the report before making throughput claims. Row interleaving, DATAFLOW/explicit streaming FIFOs, scale prefetch overlap and fused calls are later optimization candidates. This version reads/converts each row's scales before consuming its quants and does not guarantee continuous DDR saturation or an unstalled AXI return channel.

## Memory and numerical contract

| Argument | Memory format |
|---|---|
| `w_quants` | `rows * nb * 32` bytes from the repack library, represented as `2 * rows * nb` 128-bit words; lowest-address byte is bits 7:0 |
| `w_scales` | `rows * nb` raw FP16 values, identical to the repacked scale array |
| `x_native` | `nb * 34` bytes: little-endian FP16 scale followed by 32 int8 values per block |
| `y` | `rows` FP32 outputs |
| `rows`, `nb` | Output count and blocks per row; input dimension is `32 * nb` |

Supported bounds are `rows <= 151936` and `nb <= 152` (input <= 4864). A nonempty matrix needs `nb >= 1`; `rows == 0` is a successful no-op for `nb` in 0..152. Invalid shapes return before any input/output memory access. Bounds keep all index products within uint32. They do not establish that a caller allocated enough memory: the future HAL must validate buffer sizes, alignment, lifetime, device addresses and non-overlap.

All FP16 scale bit patterns with finite values, including signed zero and subnormals, are supported. The reused integer FP16 conversion avoids the FPGA half-precision IP's denormal-flushing behavior. Intermediate products/sums for finite FP16 scales and these dimension bounds stay within normal FP32 range (or zero). Separate FP multiply/add, disabled compiler contraction and disabled unsafe FP optimizations preserve the reference order. Native bit-exactness still needs confirmation against generated RTL.

| Status | Meaning | Output validity |
|---|---|---|
| 0 | Success | All requested rows valid |
| 1 | Unsupported dimensions | No output writes |
| 2 | Nonfinite activation scale (NaN/Inf) | No output writes |
| 3 | Nonfinite weight scale | Earlier rows may already have been written; discard the entire job's output |

The future backend must return failure for nonzero status so the existing software path recomputes on CPU. Nonfinite scale inputs are rejected rather than claiming NaN payload/Inf arithmetic compatibility with HLS FP operators. A hardware timeout/reset protocol is not implemented by this arithmetic kernel and remains part of HAL/integration work.

Minimum pointer alignment is 16 bytes for quants, 2 for scales, 1 for activation bytes and 4 for outputs; board allocations should use the existing 4 KiB convention and an approved cache-maintenance strategy. No full output vector or weight matrix is stored on-chip. Declared local arrays hold 4,864 B activation quants, 608 B converted activation scales, 304 B raw row scales and 608 B converted row scales; hardware banking, adapter FIFOs and pipeline storage add to that logical capacity.

## Interfaces and draft register map

The HLS Tcl assigns independent AXI masters to quants (`weights`), scales (`scales`), activation (`activation`) and results (`output`). These are logical masters; they can share a physical HP port through an interconnect. This choice avoids forcing unlike pointer widths into one HLS bundle, but adds adapter/interconnect overhead that must be included in the resource/ASIC review. Only the quants interface is explicitly 128 bits; inspect actual widths and burst inference for all four interfaces after synthesis.

| AXI-Lite offset | Meaning |
|---|---|
| `0x00` | Standard HLS start/done/idle control |
| `0x10` / `0x14` | Quant base address low/high |
| `0x20` / `0x24` | Scale base address low/high |
| `0x30` / `0x34` | Activation base address low/high |
| `0x40` / `0x44` | Result base address low/high |
| `0x50` | Rows |
| `0x58` | Blocks per row |
| `0x60` | Result status (read after completion) |

These explicit offsets are a source-level draft. Verify the generated HLS register table/header before writing the real HAL. There is no bias register or fused-operation flag in v1. The P1.1 board runner/Vivado block design targets a different IP and cannot be used unchanged for this GEMV kernel.

## Local testing

From the llama.cpp root:

```bash
make -C soc/hw/gemv test
make -C soc/hw/gemv script-test
```

`script-test` uses mocked vendor commands to check script control flow, source paths and the Vitis 2022.2 Boolean-option spelling. It also compiles the real C reference object and verifies that the simulation command links it. It does not run Vitis or establish C/RTL simulation success.

The testbench compares every successful output row bit-for-bit with `soc_q8r_gemv_ref`, compiled as C with `-ffp-contract=off`. The native kernel uses the compiler's unsigned 128-bit type in place of `ap_uint<128>`. Synthetic tests cover different row counts and block lengths, alternating signs, int8 extremes, zero/subnormal/mixed scales, repeated calls with changing shapes/data, invalid descriptors and nonfinite scale errors. All 63,488 finite FP16 representations are compared between the reused converter and reference converter. Native canaries check output bounds and shape/activation-error no-write behavior.

Native tests additionally exercise 151,936 rows with one block per row to check the row-count bound; this is not a full LM-head test. HLS C/RTL simulation uses 54 top-level calls and at most 64 rows/152 blocks, keeping adapter data manageable. Co-simulation depths describe testbench allocation sizes; synthesized dimension limits remain the bounds above. The native maximum-row case is excluded from the HLS testbench with `SOC_GEMV_HW_HOST`.

Optional real tensors use the existing read-only dump tool (requires its NumPy/gguf dependencies):

```bash
make -C soc/hw/gemv test-real
# Or use previously dumped SOCQ8T1 files:
soc/hw/gemv/test_gemv_hw /absolute/path/to/tensor.bin
```

The testbench checks at most the first 64 rows of each supplied tensor file, with deterministic synthetic activations and the real weight values. The full model's generated text or score has not been tested through this hardware path.

## Next LRC boundary: GEMV Vitis C simulation

Sync the new `soc/hw/gemv/` directory to LRC along with its existing repack and Lab 3 dependencies. From the project root in the Xilinx 2022.2 environment:

The script compiles the unchanged golden reference with the native host C compiler as C11, then passes that object through `-ldflags` to both C simulation and RTL co-simulation. It does not register the `.c` reference with apcc, avoiding Vitis 2022.2's application of C++ options to that file. A native `cc` must be available; `SOC_GEMV_HW_CC` can select another native compiler executable (not an ARM cross-compiler). The object stays in the generated project and is not part of the FPGA design.

```bash
set -o pipefail
mkdir -p soc/docs/results/lrc_p3
SOC_GEMV_HW_STAGE=csim vitis_hls -f soc/hw/gemv/build_hls.tcl \
    2>&1 | tee soc/docs/results/lrc_p3/csim.log
```

Expected testbench summary: `GEMV HW: PASS`, 54 calls, zero failures. Stop on failure and preserve the complete log. This is a new kernel; the completed P1.1 simulations do not replace this test. No board definition, bitstream or QEMU setup is required for this boundary.

After C simulation passes, `SOC_GEMV_HW_STAGE=synth` runs synthesis; `cosim` additionally runs Verilog co-simulation; `export` additionally packages IP. The default stage is `csim`. Optional knobs are `SOC_GEMV_HW_MHZ=100/150/200`, `SOC_GEMV_HW_BURST=1/4/16/64/256`, `SOC_GEMV_HW_OUTSTANDING=1..32` and `SOC_GEMV_HW_PART=xczu3eg-sbva484-1-e` (default; -i also supported after target confirmation). Each configuration's work is under `soc/hw/hls_work/gemv/f150_b64_o8/`; rerunning a stage resets that configuration, so retain logs first. It does not reset the separate P1.1 bandwidth project.

Before promoting the kernel, review the achieved loop II, inferred quants/scales bursts, FP operator binding (separate fmul/fadd), port widths, resources and clock estimate; then require RTL co-simulation agreement. If the loop cannot meet II=1, treat its achieved schedule as the v1 baseline and optimize from the evidence. Board bandwidth, memory and ASIC gates remain pending.

References: existing `../../sw/repack/README.md`, `../../lab3_ref/part3/dot_q8_0_core.cpp`, [AMD FP operation ordering](https://docs.amd.com/r/2022.2-English/ug1399-vitis-hls/Optimizing-Logic-Expressions), and [AMD operator binding](https://docs.amd.com/r/2022.2-English/ug1399-vitis-hls/pragma-HLS-bind_op).
