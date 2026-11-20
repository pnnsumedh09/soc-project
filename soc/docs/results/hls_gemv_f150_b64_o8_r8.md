# GEMV eight-row interleaving candidate

Date: 2026-11-20. Status: native numerical validation, Vitis C simulation and synthesis review pass; candidate RTL co-simulation is pending. The prior sequential baseline passed actual Vitis C/RTL tests; those results do not transfer to this changed implementation.

## Source design

Select `SOC_GEMV_HW_ROW_INTERLEAVE=8` (new default) or `1` (unchanged sequential datapath). HLS projects are separate: `f150_b64_o8_r8` versus existing `f150_b64_o8`. Public arguments, AXI-Lite offsets, memory layout, shape/status contract and 16-product dot datapath are retained.

Eight rows share a compute loop. A 32-block tile per row preserves contiguous external reads of up to 64 quant beats before round-robin local consumption. Each compute slot handles one 128-bit half-block for one row. Eight fully partitioned accumulator registers rotate each slot; each row returns after eight slots and updates only on odd beats. Even beats store the first integer half-sum. Blocks remain in ascending order within each row across all tiles. No reassociation, FP partial sums, false-dependence directive or FMA is introduced.

Inactive lanes in the final row group rotate but perform no memory access/arithmetic. Final block tiles load only valid blocks. Outputs are held for the eight-row group and then written sequentially. Logical extra tile storage is 8,192 B quants, 512 B raw scales, 1,024 B converted scales plus 64 B accumulator/half-sum state; activation storage is unchanged. These are source capacities, not measured BRAM usage.

## Local checks

- Default r8 expanded native synthetic/status suite: PASS, 86 calls, 153,046 successful rows, max absolute error 0, bit-exact golden comparison.
- Retained r1 implementation with the same expanded suite: PASS, 86 calls, 153,046 rows, max absolute error 0.
- r8 plus eight existing Qwen weight slices (64 rows each): PASS, 94 calls, 153,558 rows, max absolute error 0.
- UBSan with separately instrumented C oracle and C++ kernel/testbench/conversion core: same 94-call combined suite PASS, no diagnostics.
- Added shapes: (rows, blocks) = (8,32), (9,33), (17,65), (16,64), (15,31), for each of six numerical patterns. Existing tests include small/tail groups, max input width and native-only max rows. Added NaN weight scale in row 8/block 32 to cross both tile boundaries.
- Local mocked HLS Tcl regression: PASS; checks C-object linking, strict FP flags, supported row counts and distinct baseline/candidate project names. This is not vendor verification.

Logs: `/tmp/soc_gemv_r8_checks.log`, `/tmp/soc_gemv_r1_checks.log`, `/tmp/soc_gemv_r8_real.log`, `/tmp/soc_gemv_r8_ubsan.log`. The HLS-bounded suite omits one native-only max-row case: expected 85 calls and 1,110 successful rows; optional real slices are not included by default.

## Required next boundary

Run candidate C simulation and synthesis on LRC, preserving baseline artifacts. Review achieved `interleave_words` II, accumulator feedback, shared arithmetic/operator counts, contiguous `load_tile_words` bursts, total latency and resources. Then require candidate RTL co-simulation PASS. The source requests II=1; no achieved candidate II or speedup is claimed.

Tile loading and computation do not overlap, and tails occupy padded slots. Even if compute II improves, full-matrix speedup must account for these costs and DDR stalls. More rows do not imply eight replicated engines or an eightfold speedup. Further overlap/activation-load optimization remains separate work.

## LRC synthesis reviewed: 2026-11-20

Evidence: supplied `project/lrc/synth.log` (run completed 02:21:33 CST) and current GEMV-named XML/RPT files for source commit `d96cc9f0c`, target xczu3eg-sbva484-1-e, project f150_b64_o8_r8. The mixed report folder retains old sequential pipeline files; only current candidate-named pipelines and the replaced top report are used here.

C simulation PASS: 85 calls, 1,110 successful output rows, max absolute error 0, zero failures. Synthesis and RTL generation completed without ERROR lines.

| Item | Sequential baseline | Eight-row candidate |
|---|---:|---:|
| Main compute loop II | 5 | 1 |
| Main compute pipeline depth | 25 | 22 |
| LUT | 9,317 | 12,580 |
| FF | 6,210 | 10,660 |
| DSP | 12 | 22 |
| BRAM_18K | 7 | 11 |
| Estimated top clock period | 4.867 ns | 4.867 ns |

Candidate `load_tile_words` and `load_tile_scales` achieve II=1/depth=3; `write_rows` II=1/depth=2. `convert_tile_scales` remains II=2/depth=3 and `load_x` II=42/depth=51. The compute-loop scheduling target is achieved; the overall constraints warning does not mean that compute failed. Remaining automatic pipeline limitations occur in activation loading and scale conversion. Loop-flattening warnings reflect separate load/convert/compute phases.

The log infers variable-length 128-bit weight reads, 16-bit scale reads and 32-bit output burst writes. Activation reads remain 8-bit. Thus grouping outputs now permits an inferred burst; actual memory traffic still needs RTL/integration verification. Dangling AXI output warnings name internal pipeline submodules, and alone do not establish failure of top-level interfaces; candidate RTL co-simulation is required.

II=5 to II=1 gives five times the scheduled compute-loop initiation rate for full groups. It is not a measured fivefold GEMV speedup: tile reads and compute are sequential phases, scale preparation and activation load remain, partial groups include inactive slots, and DDR stalls are unmeasured. Total runtime is undefined in the synthesis report because dimensions are runtime arguments. Resource numbers cover the HLS IP and are not ASIC area/power. Estimated 4.867 ns with 1.80 ns uncertainty at a 6.67 ns target is not post-route timing or proven 205 MHz operation.

Next: keep this candidate unchanged and run Verilog co-simulation. The script repeats C simulation/synthesis as part of its cosim stage; no separate repeat of the synth command is requested. Candidate RTL PASS is still required before further optimization.

Current top XML SHA256: `a16962f37bd7cfb5287761a954231e9267aa92ea8af21e3e5e6ecf409a1ebdd6`.
