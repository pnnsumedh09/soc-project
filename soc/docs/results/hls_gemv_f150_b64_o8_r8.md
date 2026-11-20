# GEMV eight-row interleaving candidate

Date: 2026-11-20. Status: native numerical validation passes; Vitis C simulation, synthesis and RTL co-simulation are pending. The prior sequential baseline passed actual Vitis C/RTL tests; those results do not transfer to this changed implementation.

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
