# GEMV v1: first HLS synthesis review

Date: 2026-11-20. Run: Vitis HLS 2022.2 on LRC, 01:43:53-01:44:32 CST. Project: f150_b64_o8, target xczu3eg-sbva484-1-e, 150 MHz target. Evidence: Sumedh-supplied workspace `project/lrc/synth.log` and `project/lrc/report/soc_gemv_hw*csynth.*`. The shared folder also contains older bandwidth-reader files; this review uses the GEMV-named reports and the GEMV summary, not the older P1.1 numbers.

Status: synthesis completed and generated Verilog/VHDL; requested pipeline targets were not all achieved. This is a scheduled v1 baseline, not an optimized or RTL-verified accelerator. The run also repeats the successful C simulation (54 calls, 720 rows, max absolute error 0).

## Synthesis findings

| Item | Reported value | Interpretation |
|---|---|---|
| Quant read/compute loop | Target II=1, achieved II=5, depth 25 | Actual scheduling constraint miss; do not report one beat/cycle |
| Activation load loop | II=42 per native Q8_0 block, depth 51 | Serialized byte reads and loop/exit scheduling; paid once per GEMV |
| Weight-scale read loop | II=1, depth 3 | Intended sequential scale read schedule achieved |
| Weight-scale conversion | II=2, depth 3 | Per-row scale preparation also adds work |
| Weight master | 128-bit data, 64-bit address, inferred variable-length bursts | Matches primary data-path width; configured max burst 64/outstanding 8 is not measured bus utilization |
| Scale accesses | 16-bit elements/accesses, inferred bursts | Access width is not the physical AXI RDATA width; see P4 width correction |
| Activation accesses | 8-bit elements/accesses, inferred 2-byte scale and 32-byte quant reads | Narrow accesses do not imply an 8-bit physical AXI bus |
| Output master | 32-bit data, generated name `m_axi_output_r` | Future integration must use the actual generated interface name |
| Control | AXI-Lite offsets match the source draft, status 0x60 / valid 0x64 | No register-map change required from these reports |
| Resources | 9,317 LUT, 6,210 FF, 12 DSP, 7 BRAM_18K, 0 URAM | HLS estimates for the GEMV IP, excluding system interconnect/integration; not ASIC area/power |
| Timing | Target 6.67 ns, estimated 4.867 ns, uncertainty 1.80 ns | Summary slack is 0.00 at displayed precision. This is not post-route timing or evidence of 205 MHz system operation |

## Main throughput limit

The log explicitly identifies a carried dependency through `sumf`. The binding report shows separate FP multiply and FP add operations; the accumulator add has latency 5. Both scale/term multiplies share a reported FP multiplier instance. Integer multiply/add combinations in DSPs are not FP fused multiply-adds.

At II=5, the loop starts a new 128-bit quant beat every five clocks in its scheduled steady state. The analytical quant-consumption ceiling at 150 MHz is `16 bytes * 150e6 / 5 = 0.48 GB/s`, before stalls, per-row startup/scale work and CPU overhead. This is a schedule-derived upper bound, not measured DDR bandwidth, complete GEMV latency or tokens/s. In particular, do not use the P1.1 reader's II=1 as the GEMV rate.

The integer decoder currently maps to signed 9-bit multipliers according to binding names; tightening the operand representation may reduce logic, but is secondary to the FP recurrence. Only measurement/report comparisons can establish the benefit.

## Warnings and next checks

The `All loop constraints were NOT satisfied` summary corresponds to genuine II misses, not an aborted synthesis. Row-loop flattening was also unavailable because the row has several phases. The 128-bit weight-port widening message is expected at its configured width cap. Scale/activation widening misses identify separate alignment/access-pattern limits and should not be dismissed as the same warning.

Warnings tying AR outputs to zero name internal pipeline submodules, while top-level bursts/interfaces are generated. They are not enough to conclude the top-level memory master is disconnected; inspect generated RTL/co-simulation before making that claim. The log also selects synchronous active-low reset, consistent with the interface.

Next: preserve this baseline and run Verilog C/RTL co-simulation for numerical/protocol validation. Do not change kernel source before that baseline is captured. After it passes, explore scheduling independent rows (preserving each row's block order), separating block accumulation from beat consumption, and improving activation bulk reads/scale staging. Never suppress a real dependency with a false-dependence pragma or reorder FP sums while claiming golden bit-exactness. Validate each changed version against the same numerical suite and compare achieved schedules/resources.

No new kernel source, bitstream or board configuration is required for the next co-simulation run. The current script resets/rebuilds the same HLS configuration, so save its logs/reports before running the next stage. Board bandwidth, memory allocation and ASIC fit remain unverified.

Top-level XML SHA256: `c677fd40f7f3f9598e9887b4557a806ad1099cf1338c2b976b94db53a7b4d90e`.

## Subsequent baseline verification and candidate

The replacement LRC cosim log completes Verilog co-simulation PASS at 02:01:50 CST on 2026-11-20: 54 calls, 720 rows, max absolute error 0, zero failures. This supersedes the RTL-pending next-step text above. The verified sequential implementation remains selectable with `SOC_GEMV_HW_ROW_INTERLEAVE=1`. The later eight-row candidate is separate and not covered by these vendor results; see `hls_gemv_f150_b64_o8_r8.md`.

## P4 interpretation correction

The earlier scale/activation widths described element/access widths, not physical AXI bus widths. The current r8 top signal table explicitly reports 32-bit RDATA on both scale/activation masters and 128-bit weights. Use the selected export's RTL/interface signal table for physical wiring widths; do not infer them from the C pointer type or burst-element width. See `p4_integration_source_validation.md`. This correction does not alter the recorded II or numerical results.
