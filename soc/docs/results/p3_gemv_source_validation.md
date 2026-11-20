# P3.1 GEMV source validation

Date: 2026-11-20. Author: Codex with Sumedh. Environment: Apple clang 17.0.0, arm64 macOS. Scope: `soc/hw/gemv/`. These are native source-correctness results, not Vitis synthesis, generated-RTL validation or board measurements.

## Implemented source

`soc_gemv_hw` consumes the existing split Q8_0 weight layout plus native 34-byte activation blocks, caches the activation, computes 16 signed products per 128-bit quant beat, preserves ordered FP32 block accumulation and writes results through an AXI memory interface. Uses the unchanged Lab 2/3 FP16 converter. Rejects unsupported dimensions and nonfinite scales with explicit status. The HLS script declares separate quant/scale/activation/output masters and fixed draft AXI-Lite offsets; no backend/HAL or existing inference code is modified.

## Checks

| Check | Result |
|---|---|
| Native synthetic/status suite | PASS: 55 calls, 152,656 successful output rows bit-exact against `soc_q8r_gemv_ref`, max absolute error 0 |
| Input widths | Covers nb=1, 3, 28 and 152, including input dimensions 896 and 4864 |
| Integer/numerical cases | Zero and signed-zero scales, finite subnormal/mixed scales, random values, signed int8 extremes and positive/negative maximum block sums |
| Maximum row count | 151,936 rows with one block per row pass; not a full LM-head or simultaneous max-rows/max-nb test |
| FP16 converter | All 63,488 finite FP16 representations agree bit-for-bit with the existing reference converter |
| Descriptor/error checks | Empty calls, zero width for nonempty input, oversized dimensions, UINT32_MAX arguments, NaN/Inf activation and late-row weight scales; native output canaries pass |
| Real Qwen weights | PASS: first 64 rows from each of eight matmul tensors, 512 additional rows, bit-exact with deterministic synthetic activations |
| Combined synthetic/real suite | PASS: 63 calls, 153,168 successful output rows, max absolute error 0 |
| UndefinedBehaviorSanitizer | PASS for the combined 63-call suite, no diagnostics |
| Tensor-file parser | Truncated and zero-dimension headers rejected with failure exit; no success claimed for malformed data |
| HLS Tcl control flow | Local mocked checks pass for csim/synth/cosim/export stage selection, source paths, basename-only project creation, strict FP configuration and four distinct AXI bundles. No vendor commands executed |

Commands for ordinary native tests are in `../../hw/gemv/README.md`. The sanitizer run separately compiles the C reference and C++ kernel/testbench/legacy conversion core with `-fsanitize=undefined -ffp-contract=off`. The native substitute for `ap_uint<128>` is the compiler's unsigned 128-bit integer; actual HLS compilation is still needed.

## Real input provenance

Read-only source: workspace `qwen2.5-0.5b-instruct-q8_0.gguf`, 675,710,816 bytes. SHA256: `ca59ca7f13d0e15a8cfa77bd17e65d24f6844b554a7b6c12e07a5f89ff76844e`.

Tested the first 64 rows of `blk.0.attn_q.weight`, `attn_k`, `attn_v`, `attn_output`, `ffn_gate`, `ffn_up`, `ffn_down`, and `output.weight`. All are Q8_0; FFN-down has 4864 inputs, the other slices have 896. Fixtures use the existing SOCQ8T1 format. Since NumPy is unavailable locally, a temporary Python standard-library reader extracted tensor metadata/offsets and raw row bytes directly from the GGUF; the model was not changed. The existing `soc/tools/dump_q8_tensor.py` is the maintained fixture generator for environments with its dependencies.

The golden result comes from the existing C `soc_q8r_gemv_ref`, independently compiled with contraction off. Hardware quant words are packed explicitly from the existing repack's bytes, avoiding pointer-aliasing or alignment assumptions. Synthetic activations exercise arithmetic and do not establish actual model logits, generation quality or benchmark score.

## Pending acceptance

Next is Vitis C simulation on LRC using the new `soc/hw/gemv/build_hls.tcl`. The default HLS testbench has 54 top-level calls and 720 successful output rows; the native-only maximum-row case is excluded to keep co-simulation adapters bounded. After C simulation, review synthesis for actual burst inference, separate FP operations, achieved II, resources and timing, then run RTL co-simulation. A pipeline pragma is not evidence of the achieved II. Strict FP recurrence may require a slower baseline or a later row-interleaved design.

No Vivado design, bitstream, FPGA backend integration, real GEMV throughput or ASIC area/power result is established by these native tests. Model, DMA-memory, bandwidth and ASIC promotion gates remain pending.

## First LRC attempt and script fix: 2026-11-20

Sumedh's first Vitis HLS 2022.2 run reached solution setup but failed before C simulation: `config_compile -unsafe_math_optimizations false` treated the separate `false` token as an unknown option. The tool's help specifies the `-option[=true|false]` form. Corrected both FP options to `-unsafe_math_optimizations=false` and `-no_signed_zeros=false`.

Added `test_hls_script.tcl` and `make script-test`: the local mocked check rejects the exact old syntax, accepts both corrected options, verifies source paths and reaches csim-only dispatch. It passes. This does not constitute vendor-tool validation; the actual Vitis rerun is pending. Kernel arithmetic and previously passed native numerical tests are unchanged.

## Mixed-language apcc compilation fix: 2026-11-20

The next LRC attempt accepted both corrected Boolean options but failed while compiling the registered C reference with apcc: `invalid argument '-std=c++11' not allowed with 'C/ObjC'`. The testbench did not execute. This is a build-language failure, not a reported numerical mismatch.

The HLS script now compiles the unchanged `soc_repack.c` explicitly as C11 with native host `cc` and contraction disabled, then passes the resulting object using `csim_design -ldflags` and `cosim_design -ldflags`. The reference C file is no longer registered as an apcc testbench source. `SOC_GEMV_HW_CC` can select a different native host C compiler; this must not be an ARM cross-compiler. AMD documents linker flags for both simulation commands ([C simulation](https://docs.amd.com/r/2022.2-English/ug1399-vitis-hls/csim_design?contentId=eovOMlfA0ve_STE4KVUU4w), [RTL co-simulation](https://docs.amd.com/r/2022.2-English/ug1399-vitis-hls/cosim_design)).

Local regression passes: actual C object compilation, no C source registered with apcc, correct object supplied to both mocked simulation stages, and corrected Boolean syntax. The native testbench linked to that exact Tcl-built C object passes the combined synthetic/real suite: 63 calls, 153,168 successful rows, max absolute error 0, zero failures. Kernel arithmetic and reference source are unchanged. Actual Vitis C simulation and later RTL validation remain pending.

## Vitis C simulation PASS: 2026-11-20

Sumedh supplied the completed LRC Vitis HLS 2022.2 C-simulation result at 01:42:14 CST for `f150_b64_o8`: `GEMV HW: PASS; calls=54 rows=720 max_abs_error=0 failures=0`, followed by `CSim done with 0 errors` and the stage-completed message. This establishes that the bounded GEMV testbench now compiles and runs with the real HLS types and the separately linked C oracle. The earlier Boolean and mixed-language build failures are resolved for C simulation.

The 54-call/720-row count is the expected HLS suite; the native-only maximum-row case and optional real-tensor files are not part of this default run. This is C-level numerical validation, not generated-RTL or board evidence. Next: `SOC_GEMV_HW_STAGE=synth`; review achieved II, burst inference, FP operator binding, interfaces, resources and timing before RTL co-simulation.

## First synthesis baseline reviewed: 2026-11-20

Reviewed supplied `project/lrc/synth.log` and GEMV-named RPT/XML files. Synthesis completed, but main read/compute loop achieved II=5 versus target 1 because of the FP accumulator dependency. Activation load II=42 per block; scale reads II=1; conversion II=2. Inferred weight/scales bursts and register offsets match the design. Resource estimates: 9,317 LUT, 6,210 FF, 12 DSP, 7 BRAM_18K. Detailed review and provenance: `hls_gemv_f150_b64_o8_v1.md`. The next boundary is baseline Verilog co-simulation; no kernel source change or hardware performance pass is claimed.

## Uploaded cosim.log is C-simulation-only: 2026-11-20

The next uploaded `project/lrc/cosim.log` (01:51:13-01:51:30 CST) reports the expected 54-call/720-row C-simulation PASS, then explicitly ends with `P3.1 GEMV HLS stage csim completed`. It contains no `cosim_design` invocation or C/RTL completion result. No GEMV co-simulation RPT is present in the supplied report folder (the older `soc_bw_read_cosim.rpt` belongs to P1.1). This evidence does not establish that GEMV RTL co-simulation ran or passed. It may be a miscopied log or a csim-stage invocation; the supplied log does not show the original shell command, so do not assume which. Obtain an existing actual co-simulation log/report if available; otherwise invoke the cosim stage explicitly. No source change is indicated by this upload.

## Verilog C/RTL co-simulation PASS: 2026-11-20

The replacement `project/lrc/cosim.log` now contains the actual `cosim_design -rtl verilog -trace_level port -ldflags .../golden_ref/soc_repack.o` invocation and `C/RTL co-simulation finished: PASS`. The final RTL-backed testbench reports `GEMV HW: PASS; calls=54 rows=720 max_abs_error=0 failures=0`. The script completes stage `cosim` at 02:01:50 CST, with co-simulation elapsed time 458.28 seconds. No ERROR/FATAL lines appear in this log. This supersedes the previous C-simulation-only upload as the latest verification evidence.

Generated Verilog agrees with the golden reference for the bounded synthetic/status suite, including repeated calls and nonfinite-scale rejection. Optional real-tensor fixtures and the native-only maximum-row case were not run in this default RTL suite. This establishes baseline RTL functional correctness for the tested cases; it does not establish board throughput or timing. P3.3 remains in progress because design exploration is outstanding. Next: preserve this baseline, then optimize the II=5 FP recurrence and activation loading, checking numerical correctness at each changed design boundary. No repeat of this unchanged baseline run is needed.

## Eight-row candidate source validation: 2026-11-20

Implemented row interleaving after the sequential baseline's RTL pass. The new default `SOC_GEMV_HW_ROW_INTERLEAVE=8` buffers 32 blocks per row and rotates eight independent accumulators while reusing the 16-product dot datapath. Each row's arithmetic order is preserved. The original sequential implementation remains selectable with `1`; candidate projects have a separate `_r8` suffix.

Expanded native suite passes for both variants: 86 calls, 153,046 rows, zero error. Eight existing Qwen slices bring candidate coverage to 94 calls/153,558 rows, all bit-exact; separately instrumented UBSan also passes. Added row-group/block-tile boundary shapes and a late invalid-scale case. Candidate HLS suite now expects 85 calls/1,110 successful rows. Actual candidate Vitis compilation, achieved II/resources and RTL correctness are pending. Details: `hls_gemv_f150_b64_o8_r8.md`.
