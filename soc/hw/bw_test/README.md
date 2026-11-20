# P1.1: DDR-to-PL read bandwidth test

Status: source implemented; native C++ functional tests pass. Vitis HLS C simulation/synthesis/co-simulation, RTL monitor simulation, Vivado implementation and Ultra96 measurements are pending. No bandwidth or FPGA resource numbers have been measured here.

This is a standalone experiment, independent of llama.cpp and the existing emulation backend. It reads a contiguous DMA buffer through one 128-bit AXI master connected to PS HP0. A small checksum consumes every returned 32-bit lane. The test does not perform GEMV or write to DDR.

## Files and interfaces

| File | Purpose |
|---|---|
| `bw-read.cpp`, `bw-read.h` | Read-only HLS kernel; 16-byte beats, four parallel modulo-2^32 checksum accumulators |
| `tb.cpp`, `Makefile` | Functional cases: empty, short, multi-burst, page boundaries, nonzero offsets, zero/all-one/pseudorandom data, invalid sizes |
| `read-window.v`, `tb-read-window.v` | FPGA cycle counter and directed simulation of stalls, burst gaps, overlapping requests, clear/reset and empty jobs |
| `build_hls.tcl` | C simulation, synthesis, RTL co-simulation and IP export for the ZU3EG |
| `build_vivado.tcl` | PS configuration, control interconnect, memory interconnect, HP0, reset/clock, cycle-counter GPIOs, implementation and artifact export |
| `test_rtl.tcl` | Run the cycle-counter testbench using Vivado's simulator |
| `run_board.py` | Optional PYNQ board runner: DMA buffer, checksum verification, empty jobs and CSV samples |

Kernel AXI-Lite registers (byte offsets; confirm against the generated HLS driver header before board use):

| Offset | Register |
|---|---|
| `0x00` | Standard HLS control: start bit 0, done bit 1 (clear-on-read), idle bit 2; auto-restart disabled |
| `0x10`, `0x14` | DMA source address low/high |
| `0x20` | Number of bytes, multiple of 16, at most 256 MiB |
| `0x28` | Output checksum: sum of all little-endian uint32 words modulo 2^32 |
| `0x30` | Output status: 0 success, 1 invalid byte count |

Zero bytes is a valid empty job with checksum 0 and no DDR requests. Invalid lengths return status 1 and checksum 0 without reading memory. Hardware requires a valid, 16-byte-aligned DMA address and enough allocated memory; it cannot validate CPU allocation ownership. The board runner requires a 4 KiB-aligned allocation in low DDR. HLS depth is 65,536 beats for the co-simulation adapter, sufficient for this testbench; it does not limit the synthesized RTL's transfer length.

## What is timed

The monitor observes the HLS master's ARVALID/ARREADY and RVALID/RREADY handshakes, before the memory SmartConnect. It latches the span from the first accepted read address through the last accepted data beat, inclusive. It includes initial memory latency, gaps and backpressure. Polling after the last beat does not change the latched span. Clear it before each job, and only read the two counter halves after HLS reports completion.

`read_seconds = read_cycles / (PL_clock_MHz * 1e6)`

`read_GB_s = nbytes / read_seconds / 1e9`

The board runner separately times descriptor writes, start and polling with the CPU monotonic clock (`job_ns`). It performs 1,000 empty jobs by default. This measures Python/PYNQ software overhead, not the eventual C HAL's latency. Do not blindly subtract it from real-job times. Monitor clear, buffer initialization/flush and output-register reads are outside `job_ns`; the weight buffer is flushed once before the sweep.

The 128-bit interface can return at most one 16-byte beat per clock. At 150 MHz its theoretical peak is 2.4 GB/s, not a measured result. This read-only checksum engine estimates the usable read path under its own traffic pattern; it does not prove GEMV throughput, the full weight-allocation sequence, or feasibility of the complete accelerator.

## Local functional checks

```bash
cd soc/hw/bw_test
make test
# If Icarus Verilog is installed:
make rtl-test
```

The native build uses the compiler's unsigned 128-bit integer in place of `ap_uint<128>`. It exercises the same kernel body, but does not verify HLS scheduling, burst inference or AXI transactions. Vendor C simulation and RTL co-simulation are separate checks.

## LRC: simulate, synthesize and export HLS IP

Run from the llama.cpp root using the course Xilinx 2022.2 environment:

```bash
module load xilinx/2022
source /usr/local/packages/Xilinx_2022.2/Vivado/2022.2/settings64.sh
SOC_BW_MHZ=150 SOC_BW_BURST=64 SOC_BW_OUTSTANDING=8 SOC_BW_STAGE=export \
    vitis_hls -f soc/hw/bw_test/build_hls.tcl
vivado -mode batch -source soc/hw/bw_test/test_rtl.tcl
```

`SOC_BW_STAGE` accepts `csim`, `synth`, `cosim`, `export`. The export path runs all preceding checks, including co-simulation. Clock choices are 100/150/200 MHz; burst limits are 1/4/16/64/256 beats; outstanding reads are 1..32. These are build-time settings and require a new bitstream to change. Build directories are keyed by all three parameters. Repeating the same configuration resets its generated project.

Check `soc/hw/hls_work/bw_test/f150_b64_o8/solution/syn/report/`: the read loop must achieve II=1 and the memory port must remain 128 bits. Inspect warnings for missed burst inference. A configured maximum burst length is a limit, not proof of actual burst lengths; inspect RTL co-simulation traces or an ILA if needed. HLS must split bursts at 4 KiB boundaries. The 256-beat maximum only fits an entire page with a page-aligned start. Record the achieved II, clock estimate and resource usage before board use.

## LRC: build the Ultra96 overlay

The checkout has no verified Ultra96-V2 DDR/MIO preset. Supply exactly one of the following:

1. `SOC_BW_BOARD_PART`: the exact Ultra96-V2 board-part identifier installed in Vivado. Obtain it using `get_board_parts *ultra96*` in the Vivado Tcl console; do not substitute a different board revision. The script applies its board preset.
2. `SOC_BW_PS_CONFIG`: an absolute path to a Tcl file containing the PS `set_property -dict ... $ps` configuration block from a known-working course/Ultra96-V2 Vivado design. Export the working design with `write_bd_tcl`, then extract just that PS property block. The script has already created `$ps` as `zynq_ultra_ps_e_0`; this file must not create cells, projects, clocks or connections. Preserve DDR, MIO and peripheral settings. The build script then enables HPM0, HP0 and PL0 for this experiment.

Example after creating that configuration file:

```bash
SOC_BW_PS_CONFIG=/absolute/path/ultra96v2_ps.tcl \
SOC_BW_MHZ=150 SOC_BW_BURST=64 SOC_BW_OUTSTANDING=8 \
    vivado -mode batch -source soc/hw/bw_test/build_vivado.tcl
```

All PL blocks use the same clock. PS HPM0 controls the HLS core and two GPIOs; the HLS master reads DDR through HP0. One GPIO clears the monitor and the other exposes its 64-bit result. Addresses are assigned by Vivado and discovered from the HWH by PYNQ. Successful implementation writes matching `bw_test_f150_b64_o8.bit` and `.hwh` under `soc/hw/out/`, plus an XSA. Timing and utilization reports stay in the generated Vivado project. The script checks setup and hold slack; inspect the full timing summary, unconstrained paths and DRC warnings before loading hardware.

## Board: collect P1.2 measurements

The runner assumes the course image supports PYNQ `Overlay`, `allocate`, `Clocks` and DMA-buffer `flush`. This assumption is still subject to P0.5. If PYNQ is unavailable, retain the same hardware and register map and add the board-approved allocator/MMIO runner after the course loading/access flow is confirmed. Heap virtual addresses are not DMA addresses.

Copy the matching BIT/HWH pair and `run_board.py` to the board. Keep BIT/HWH filenames identical except for the extension. In the PYNQ Python environment:

```bash
sudo python3 run_board.py bw_test_f150_b64_o8.bit --clock-mhz 150 \
    --sizes 16384 1048576 16777216 --repeats 10 --empty-repeats 1000 \
    > pl_read_f150_b64_o8.csv
```

The runner checks the configured PL0 clock, verifies every checksum, discards one warm-up per size, and records individual samples. It reloads the overlay before releasing a DMA buffer if a job is interrupted or times out. Larger sizes up to 256 MiB are supported if the board's allocator can supply them; start small. A 16 MiB allocation does not validate the 145 MB LM-head allocation or the 525 MB total weight working set (P0.6).

For P1.2, repeat across burst/outstanding/clock settings and idle versus active CPU conditions. Record the board image, source revision, buffer sizes, CPU core/frequency settings, traffic conditions, build configuration, timing reports and raw CSV files. The first version uses one HP port; a two-port comparison requires a later design variant. Save the measured summary in `soc/docs/results/pl_read_bandwidth.md`. Do not populate it with simulated or estimated bandwidth.

## References

- [AMD Vitis HLS 2022.2: AXI interface directives](https://docs.amd.com/r/2022.2-English/ug1399-vitis-hls/pragma-HLS-interface)
- [AMD Vitis HLS 2022.2: controlling AXI bursts](https://docs.amd.com/r/2022.2-English/ug1399-vitis-hls/Controlling-AXI4-Burst-Behavior)
- [AMD 2022.2: enabling Zynq PS AXI interfaces](https://docs.amd.com/r/2022.2-English/Vitis-Tutorials-Vitis-Platform-Creation/Enable-AXI-Interfaces-for-the-Platform)
