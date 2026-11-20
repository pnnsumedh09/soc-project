# Register-test source validation

Date: 2026-11-20. Source written; local model/runner checks pass. Actual generated-bank RTL execution and board register readback are pending.

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
| Actual new SV compilation/simulation | NOT RUN locally: no xvlog/xsim/iverilog/verilator installed. Module header/ports were supplied, but full generated implementation is not available locally |
| Board register tests | NOT RUN: hardware/UIO/PS setup unconfirmed |
| DMA/DDR/activation-BRAM tests | Outside this register-only test; existing kernel tests cover internal storage indirectly, board DMA remains pending |

Commands/coverage/limitations are in `soc/hw/gemv/registers/README.md`. The next required result is `GEMV REGISTER RTL: PASS` from the new LRC run, not another kernel csim/synth/cosim. No pass for the actual AXI-Lite implementation is inferred from the model or fake-tool tests.
