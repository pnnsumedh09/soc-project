# SoC Working Context

Use this file as the entry point for SoC project work. Do not inventory or read the full llama.cpp repository at the start of a task.

## Context policy

1. Read this file first.
2. Open only the file named by the current task and its direct dependencies.
3. Search within `soc/` by default. Do not run repository-wide `find`, `tree`, `rg --files`, or broad Git history commands unless the task requires llama.cpp integration.
4. Do not read generated output directories. In particular, exclude `soc/hw/hls_work/`, build products, `.dSYM`, model files, PDFs, logs and result archives unless the user asks about them.
5. Treat `soc/lab3_ref/` as reference-only. Open only a specifically required Lab 3 file.
6. Use short command output limits and narrow path filters.
7. Preserve unrelated working-tree changes. Never stage with `git add .` or `git add -A`.

## Active source set

- Project status and phase decisions: `SOC_PROJECT_PLAN.md`
- Immediate user steps: `soc/docs/NEXT_STEPS.md`
- GEMV HLS kernel: `soc/hw/gemv/gemv.cpp`, `gemv.h`, `tb.cpp`, `build_hls.tcl`
- AXI-Lite RTL register test: `soc/hw/gemv/registers/`
- Vivado integration: `soc/hw/gemv/build_vivado.tcl`
- Software HAL and FPGA adapter: `soc/sw/hal2/`
- QEMU teammate handoff: `soc/docs/QEMU_SYSTEMC_HANDOFF.md`
- QEMU reference code, only when working on QEMU: `soc/lab3_ref/part3/`

## Current design checkpoint

- Sequential Q8_0 GEMV is the selected kernel. Row interleaving and double buffering are deferred.
- HLS C simulation, synthesis, RTL co-simulation and generated AXI-Lite register tests passed.
- Ultra96-V2 Vivado block design passed validation.
- Control registers are mapped at `0xA0000000` with a 64 KiB range.
- Four accelerator memory masters reach the PS HP0 DDR path. The exposed low DDR range is 2 GiB.
- An old f150-named bitstream loaded through PYNQ, but its first control read stalled and USB xHCI failed. That bitstream had an asserted auxiliary-reset bug and must never be loaded again. The USB failure is still unexplained.
- Rebuild entry point: `bash soc/tools/rebuild_lrc.sh software|hls|bd|bitstream`, explicitly selecting 100 MHz. Physical-board validation remains.
- Commits `fa38b6182` and `8bfd25dce` explicitly configure and post-validation-check the reset topology, including Python 3.6-compatible mocked regression tests. The corrected f100 BD and bitstream were regenerated successfully on LRC.
- The corrected final HWH reports `C_AUX_RESET_HIGH=0` and active-low `aux_reset_in` tied to `locked_0_dout=1`. `C_EXT_RESET_HIGH=1` receives the inverted active-low PS `pl_resetn0`. GEMV and both SmartConnect blocks use `reset_0/peripheral_aresetn`.
- The corrected implementation is 100 MHz with WNS `+5.349 ns`, WHS `+0.010 ns`, no failing endpoints, no errors and no critical warnings. It has not yet been loaded on the board.
- Board image: PYNQ 3.0.1. `/dev/uio4` maps fabric at 0xA0000000; uio0 is an AXI performance monitor. No u-dma-buf found. CMA is 128 MiB, insufficient to cache all model weights simultaneously.
- The portable HAL and mocked tests exist. Actual board DMA allocation/access support must still be confirmed.
- QEMU/SystemC integration is assigned to a teammate and is not proof of FPGA-board operation.

## Board bring-up handoff: 2026-11-22

The next task is to determine whether a fresh-boot PYNQ load releases the PS-generated `pl_resetn0` signal, while avoiding all GEMV MMIO and DMA. Do not broaden this into inference or performance work.

### Verified corrected artifacts

- `soc/hw/out/gemv_f100_b64_o8.bit` SHA-256: `c98f594eed9bd64491b7c98693097b802366bfbf0f2a4e7778f40f7a0a2d0a62`
- `soc/hw/out/gemv_f100_b64_o8.hwh` SHA-256: `bfe4c8566fec0fe96b12ee3d932dd27614bda841ec8597569858af77ca4730dd`
- Control: PS `M_AXI_HPM0_FPD` to `gemv_0/s_axi_control`, `0xA0000000..0xA000FFFF`.
- Memory: four GEMV masters through `S_AXI_HP0_FPD`, each mapped to `0x00000000..0x7FFFFFFF`.
- Clock: implemented and HWH metadata both report 100 MHz.

### Fresh-boot preflight result

The board was power-cycled and inspected before loading the corrected overlay. No corrected-bitstream programming has occurred yet.

- Kernel: `5.15.19-xilinx-v2022.1`; PYNQ `3.0.1`.
- Boot device-tree model: `Avnet Ultra96 Rev1`; compatible list includes `avnet,ultra96` and `xlnx,zynqmp`.
- `/dev/uio4` is `fabric` at `0xA0000000`, size `0x10000`, interrupt tuple includes SPI 89. `/dev/uio0..3` are AXI performance monitors.
- `fuser` found no users of UIO or render devices. `zocl` and `uio_pdrv_genirq` are loaded. Jupyter/Python processes exist but did not own those devices.
- CMA total/free was 128 MiB/approximately 122 MiB. No `u-dma-buf` device was present.
- Live `PL0_REF_CTRL` at `0xFF5E00C0` was `0x01010F00`; PYNQ decoded `fclk0` as `99.999 MHz`.
- GPIO bank 5 values before programming were all zero: `DATA5@0xFF0A0054=0`, `DIRM5@0xFF0A0344=0`, `OEN5@0xFF0A0348=0`. With one fabric reset, bank-5 bit 31 represents `pl_resetn0`. This is a consistent asserted/unconfigured preload state, not evidence that the corrected HWH is wrong.
- The first preflight helper incorrectly treated this all-zero preload state as a failure. Future tooling should allow either all three bit-31 values low before programming or all three high, reject mixed state, and require all three high after programming.

### Unresolved question and staged test

PYNQ 3.0.1's `Overlay.download()` applies HWH clock divisors and invokes the ZynqMP FPGA manager. Its Python path does not explicitly manipulate the `pl_resetn0` GPIO; platform firmware may release it during full-bitstream programming. This must be observed, not assumed.

Use a UART console at 115200 and a fresh boot. Preserve `/boot/system.bit` and the boot DTB. Verify artifact hashes and parsed HWH first. Then perform exactly one PYNQ load in that boot. Before and after loading, record the PS clock and the three GPIO bank-5 registers. After loading, wait at least 30 seconds and collect kernel messages and FPGA-manager state. Do not instantiate the GEMV IP object, read `0xA0000000`, write registers, allocate DMA, run the board smoke test, or run llama.cpp.

Pass criteria for the load-only stage:

1. PYNQ reports the matching overlay loaded and discovers `gemv_0` metadata at `0xA0000000/0x10000`.
2. PL0 remains approximately 100 MHz.
3. GPIO bank-5 `DATA5`, `DIRM5`, and `OEN5` all have bit 31 set after programming, meaning `pl_resetn0` is configured as an enabled high output.
4. No new xHCI, SError, external-abort, kernel-panic, or FPGA-manager errors appear, and UART/network access remains healthy for 30 seconds.

If the post-load reset bits remain low, stop without MMIO. The next change should explicitly control/release `pl_resetn0` using an approved board-specific mechanism, with the accelerator's four AXI masters known idle. If the board becomes inaccessible, do not retry in the same boot; preserve UART output, halt if possible, and power-cycle.

## Task routing

- HLS change: read only `soc/hw/gemv/` and the exact referenced numerical helper.
- Register change: read only `soc/hw/gemv/registers/`, `gemv.h` and relevant HAL register definitions.
- Vivado change: read only `build_vivado.tcl`, exported HLS interface metadata and the requested Vivado log.
- HAL or board change: read only `soc/sw/hal2/`, the register map and the board support evidence supplied by the user.
- QEMU change: read `soc/docs/QEMU_SYSTEMC_HANDOFF.md`, then only the required files under `soc/lab3_ref/part3/`.
- llama.cpp call-site integration: identify the exact backend/call site with one targeted `rg` query; do not scan unrelated tools, examples, models or documentation.

Update this file only when the architecture, active paths or validation checkpoint changes.
