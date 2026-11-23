# Combination A HLS review: 100 MHz sequential kernel

Date reviewed: 2026-11-22

Source package: `combination_a_hls_review`

Historical note: this report records the scalar decision at the time it was reviewed. The later r8/double-buffer experiment in `../COMBINATION_A_R8_COUNTERS_END_TO_END.md` supersedes the implementation recommendation but not these baseline measurements.

Configuration:

```text
SOC_GEMV_HW_MHZ=100
SOC_GEMV_HW_BURST=64
SOC_GEMV_HW_OUTSTANDING=8
SOC_GEMV_HW_ROW_INTERLEAVE=1
part=xczu3eg-sbva484-1-e
tag=f100_b64_o8
```

## Result

This is a valid HLS baseline, not yet a board result.

- C simulation passed all valid and invalid-input tests.
- Verilog RTL co-simulation passed with zero numerical failures.
- The testbench summary was `calls=85 rows=1110 max_abs_error=0 failures=0`.
- RTL co-simulation reported 79 minimum, 13,066 average and 99,698 maximum cycles across its transactions. Total simulated execution was 1,109,280 cycles.
- The largest valid test, 64 rows by 152 Q8 blocks, took 99,698 cycles, or about 996.98 microseconds at 100 MHz.
- Export completed and produced the Vivado IP component.

The HLS clock estimate was 7.300 ns against the 10.00 ns target, equivalent to an estimated 136.99 MHz. This is synthesis estimation only. Routed Vivado timing must still pass before deployment.

Estimated resources on the target device were:

| Resource | Used | Available | HLS utilization |
|---|---:|---:|---:|
| BRAM18K | 7 | 432 | 1% |
| DSP | 12 | 360 | 3% |
| FF | 6,058 | 141,120 | 4% |
| LUT | 9,395 | 70,560 | 13% |

There is ample estimated resource headroom for experiments, but the complete block design and routed implementation will consume more resources.

## Scheduling evidence

| Loop | Target II | Achieved II | Interpretation |
|---|---:|---:|---|
| `load_x` | 1 | 42 | Activation loading is issued as expensive scalar-style AXI accesses. |
| `read_scales` | 1 | 1 | Scale reads meet the requested pipeline interval. |
| `convert_scales` | 1 | 2 | Floating-point conversion carries a small scheduling cost. |
| `read_words` | 1 | 3 | The ordered floating-point accumulation dependence limits the main weight loop. |

For large matrices the repeated weight/accumulate loop remains the primary concern. The one-time activation load can still be significant for small-row Q/K/V jobs, despite activation bytes being tiny relative to weight bytes.

## Decision

1. Preserve `f100_b64_o8` row-interleave 1 as the scalar board baseline.
2. Import the Ultra96-V2 board definition, build the block design, and require routed timing to pass.
3. Save the baseline `.bit`, `.hwh`, `.xsa`, utilization and timing reports before changing the selected kernel.
4. Next evaluate the already-existing row-interleave 8 candidate at the same 100 MHz, burst and outstanding settings under the distinct `f100_b64_o8_r8` tag.
5. Compare RTL cycles, routed resources, routed timing and board latency for identical vectors.
6. Do not implement ping-pong buffering yet. First determine whether r8 is compute-limited or memory-limited on the board and obtain a load/compute phase breakdown.

Descriptor DMA, QKV grouping and fused FFN remain later stages. This report does not show that control overhead or intermediate CPU work dominates end-to-end inference.

## Claims not yet supported

- No routed timing result is available.
- No physical DDR bandwidth or AXI-stall measurement is available.
- No Ultra96-V2 correctness or latency result is available.
- No llama.cpp end-to-end speedup is available.
- HLS logical cycles must not be presented as board wall time without board validation.
