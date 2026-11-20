# Register-test source validation

Date: 2026-11-20. Source written; local model/runner checks pass. Actual generated-bank RTL execution now passes on LRC; board register readback remains pending.

## New tests

- `soc/hw/gemv/registers/tb_registers.sv`: instantiates actual `soc_gemv_hw_control_s_axi`, using the exact ports and address width pasted from Sumedh's Vitis HLS 2022.2 generated header. Drives AXI-Lite plus synthetic engine handshake/status. Covers register reset, ten parameter words, high/low pointer halves, engine-side outputs, aliases, walking patterns, byte strobes, independent AW/W, B/R backpressure, start/ready/done/valid, read-only fields, auto-restart and interrupt registers. No arithmetic/DMA/BRAM/PS model.
- `test_rtl.tcl`: compiles/elaborates/simulates copied existing generated bank with xvlog/xelab/xsim, in a separate work directory. No HLS project reset or repeated kernel simulation/synthesis/export. Requires new testbench PASS, rejects failure/missing/stale markers and tool failures.
- `soc/sw/hal2/soc_gemv_register_test.c`: reusable idle-only C parameter tester with save/restore, 68 patterns per word, neighbour checks and first-failure diagnostics. Does not write START. Header exposes the test API and optional CMake FPGA source list includes it.
- `board_registers.c`: Linux/AArch64 UIO-only target utility, independent of the DMA allocator. Checks map identity and exclusive ownership/marker; runs the parameter tester and reports mismatches. Compiles locally, not executed on hardware. Correct loaded hardware and prior standalone RTL validation are prerequisites.

## Evidence and limits

| Check | Result |
|---|---|
| Portable C tester on working model | PASS: all 680 patterns, 700 parameter writes including restores, original values restored, no CTRL write |
| Injected stuck bit / address alias / dropped write | PASS: each fault detected with the expected first-failure offset and differing expected/actual words |
| Busy / auto-restart / poisoned rejection | PASS: no parameter writes |
| C tester UBSan | PASS, no diagnostics |
| Existing HAL/arena/fallback tests | PASS after test additions |
| C target utility compilation | PASS with C11/warnings-as-errors; Linux/AArch64 runtime guard present |
| Tcl runner unit tests | PASS with fake tools: success, failure at each tool, absent RTL, no PASS, explicit FAIL, stale previous PASS rejected; source preserved |
| Actual new SV compilation/simulation | PASS on LRC with Vivado/xsim 2022.2; local simulator remains unavailable |
| Board register tests | NOT RUN: hardware/UIO/PS setup unconfirmed |
| DMA/DDR/activation-BRAM tests | Outside this register-only test; existing kernel tests cover internal storage indirectly, board DMA remains pending |

Commands/coverage/limitations are in `soc/hw/gemv/registers/README.md`. The next required result is `GEMV REGISTER RTL: PASS` from the new LRC run, not another kernel csim/synth/cosim. No pass for the actual AXI-Lite implementation is inferred from the model or fake-tool tests.

## Actual LRC register RTL PASS: 2026-11-20

Reviewed supplied `project/lrc/register_rtl.log`. Actual xvlog/xelab/xsim 2022.2 execution completes normally at 12:00:13 CST with `GEMV REGISTER RTL: PASS; reads=7396 writes=765 checks=35989`, followed by runner completion and exit 0. No actual ERROR/FATAL/WARNING lines found. All four test phases execute: patterns/address separation/channel skew/stalls; strobes/reserved isolation; start/ready/done/status/read-only behavior; auto-restart and interrupt TOW. The ap_done/ap_ready interrupt-monitor messages are the expected synthetic events, not failures.

The actual generated register bank now passes the tested 7,396 reads, 765 writes and 35,989 checks. Simulator finish at 370,150 ns describes the protocol test duration at its testbench clock, not GEMV latency or FPGA timing closure. No kernel synthesis/co-simulation rerun was performed. Preserve this result; no repeat needed for the unchanged register bank/testbench.

This supersedes the pending-RTL next step above. It does not validate PS interconnect, actual base assignment, board MMIO, DMA/coherency or scratch memory. Next is confirmed course PS configuration, Vivado BD/route and board register/DMA bring-up.

Log SHA256: `f183cf167791bf24c96cd3ad8a1a9394d4a1fe798ebf6f950f78625e897a4bfa`.
