# P1.1 source validation

Date: 2026-11-19. Author: Codex with Sumedh. Scope: `soc/hw/bw_test/`. These are local software checks, not Ultra96 bandwidth measurements.

| Check | Result |
|---|---|
| `make test` | PASS: 60 cases across zero, all-one and pseudorandom data; empty/short/page-boundary/1 MiB reads; offsets; invalid lengths; checksum wraparound |
| Same kernel/testbench with `-fsanitize=undefined` | PASS, no diagnostics |
| Python syntax and `run_board.py --help` | PASS |
| Board runner with mocked NumPy/PYNQ interfaces | PASS: valid transfers/CSV, checksum error, timeout and interruption; in-flight jobs reset before DMA allocation release |
| Tcl source completeness and invalid-parameter guards | PASS using local `tclsh`; no vendor commands executed |
| Source ASCII and whitespace checks | PASS |
| AddressSanitizer | Run did not complete in this environment and was interrupted; no result inferred |

The native build substitutes unsigned 128-bit integer arithmetic for HLS `ap_uint<128>`. Mocked board checks validate software control flow and allocation lifetime only. Neither establishes AXI behavior or performance.

Still pending on LRC: Vitis HLS C simulation, synthesis and RTL co-simulation; inspect actual burst inference, 128-bit width, read-loop II and resources. Run `test_rtl.tcl` to simulate the real Verilog monitor, then build/validate the block design and check implementation timing. Vitis HLS, Vivado and an RTL simulator were unavailable locally.

Still pending for the board flow: supply a verified Ultra96-V2 preset or PS configuration; confirm PYNQ availability under P0.5. P1.2 will collect actual read-window cycles, bandwidth and empty-job software overhead. No bitstream, hardware measurement or accelerator feasibility result is claimed by this file.
