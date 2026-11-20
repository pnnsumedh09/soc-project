# GEMV AXI-Lite register tests

These tests target the selected sequential kernel's **actual HLS-generated register bank** independently of the compute engine. They do not replace GEMV numerical tests, Vivado integration or board DMA/coherency testing.

## Standalone RTL simulation on LRC

With the Xilinx 2022.2 tools loaded, run from the repository root:

```sh
mkdir -p soc/docs/results/lrc_p4
vivado -mode batch -source soc/hw/gemv/registers/test_rtl.tcl \
    2>&1 | tee soc/docs/results/lrc_p4/register_rtl.log
```

Enable shell pipeline-failure handling if supported (`set -o pipefail` in bash). The runner uses `xvlog`, `xelab` and standalone `xsim`. It copies and compiles the existing file:

```
soc/hw/hls_work/gemv/f150_b64_o8/solution/syn/verilog/soc_gemv_hw_control_s_axi.v
```

No C simulation, HLS synthesis, kernel co-simulation, IP packaging, PS preset, bitstream or physical board is needed. Do not rerun the old kernel tests for this step. The module ports and 7-bit address width were matched against the actual Vitis 2022.2 header supplied by Sumedh.

Expected final testbench message:

```
GEMV REGISTER RTL: PASS; reads=... writes=... checks=...
```

A watchdog, assertion, nonzero tool exit or missing PASS causes the runner to fail. A stale previous simulator PASS cannot satisfy a new run. Keep the full outer `register_rtl.log`; work/logs/source snapshots are in `soc/hw/hls_work/gemv_registers/f150_b64_o8/` (`compile_output.log`, `elaborate_output.log`, `simulation_output.log`, `xsim.log`, `manifest.txt`, DUT/TB copies).

## RTL coverage

- Reset values and parameter outputs to the engine.
- All four 64-bit addresses, including their independent high halves; 32-bit rows/nb.
- Zero/all-ones/alternating/walking-one/walking-zero patterns with neighbour and engine-port checks to detect aliasing or truncation.
- Simultaneous AW/W, AW-before-W and W-before-AW. A channel's VALID remains asserted until its own READY handshake.
- Delayed BREADY/RREADY, stable response/data while stalled, OKAY responses.
- All byte strobes, zero strobe, reserved-address isolation.
- START latched until READY, DONE and READY clear-on-read, IDLE read-only.
- Kernel status capture/valid clear-on-read, read-only status/status-valid.
- Auto-restart enable/disable, GIE/IER masking, ISR toggle-on-write and synthetic interrupt events. The integration itself still uses polling, not IRQs.
- Reset after modified registers/status/control state.

The testbench drives synthetic ap_done/ap_ready/ap_idle/status/status_ap_vld inputs. It does NOT instantiate the GEMV arithmetic, DDR, DMA masters, internal activation BRAM or PS interconnect. A PASS proves the tested standalone bank's bus/register behavior, not the complete accelerator/integration.

## Local checks versus actual RTL results

No Verilog simulator is installed in the current local workspace. The new SystemVerilog test has not been compiled or run locally; actual LRC execution remains required. Local runner tests use fake tools only:

```sh
python3 soc/hw/gemv/registers/test_runner.py
```

They verify tool sequencing, source preservation, error propagation, explicit failure-marker rejection, missing PASS and stale-log handling. They are not RTL correctness evidence.

Optional overrides: shared HLS MHz/burst/outstanding select the existing sequential project; `SOC_GEMV_REGISTER_RTL` selects a supplied generated bank file and `SOC_GEMV_REGISTER_WORK` selects a dedicated generated work directory. The runner accepts only sequential selection (`SOC_GEMV_HW_ROW_INTERLEAVE=1`). The TB clock is 100 MHz for protocol testing; it is not a performance or timing-closure measurement.

## Later board register readback (without a DMA allocator)

After RTL PASS, matching hardware/PS/clock setup, confirmed UIO mapping and exclusive idle-engine ownership:

```sh
make -C soc/sw/hal2 board_registers
./soc/sw/hal2/board_registers 0 0xa0000000
```

Here `0` selects `/dev/uio0`, and `0xa0000000` is the expected actual control base; change these to the verified mapping. The program checks sysfs map0 base/size/offset and takes the same UIO/persistent-marker locks as the backend. It needs no u-dma-buf driver or scratch-buffer allocation.

It saves all ten parameter words, tests 68 patterns per word with neighbour checks, verifies/restores parameters and reports the first mismatching offset/expected/actual value. It never writes START or submits a DMA job. Reading CTRL acknowledges old DONE/READY flags, so use it only with exclusive idle ownership. An interrupted/failed test leaves the marker armed: inspect/reset the hardware before clearing it. It does not prove internal BRAM or DDR coherency.

The portable C pattern tester is checked locally against working and deliberately faulty models, plus UBSan:

```sh
make -C soc/sw/hal2 test
```

Board execution remains pending. Actual memory/coherency/GEMV tests follow separately using `board_smoke` and the P4.2 prerequisites.

Tool reference: [AMD 2022.2 simulator quick reference](https://docs.amd.com/r/2022.2-English/ug900-vivado-logic-simulation/Vivado-Simulator-Quick-Reference-Guide).
