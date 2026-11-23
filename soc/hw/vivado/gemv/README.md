# P4.1: GEMV Vivado integration

The Combination A default is the row-interleave-8, double-buffered export. The r8-only and sequential exports remain selectable for reference. The authoritative r8, double-buffer and counter workflow is `../../../docs/COMBINATION_A_R8_COUNTERS_END_TO_END.md`.

Source written; local Tcl guard/topology checks use mocked vendor commands. No actual Vivado block-design validation, implementation, routed timing or board execution has been performed here. The course tutorial and actual LRC discovery establish the Avnet Ultra96-V2 preset: HLS remains `xczu3eg-sbva484-1-e`, while Vivado creates the board project for `xczu3eg-sbva484-1-i`. The script permits only this documented Avnet Ultra96-V2 mismatch; it does not retarget or rebuild the HLS IP.

## Topology

- PS `M_AXI_HPM0_FPD` → 1×1 control SmartConnect → GEMV `s_axi_control`.
- GEMV `m_axi_weights`, `m_axi_scales`, `m_axi_activation`, `m_axi_output_r` → 4×1 memory SmartConnect → PS noncoherent `S_AXI_HP0_FPD` at 128 bits.
- PL clock drives IP, interconnect and PS AXI clock inputs; `proc_sys_reset` distributes synchronous active-low resets. GEMV `interrupt` connects to PS `pl_ps_irq0`; polling remains usable for initial bring-up.
- GEMV control occupies 4 KiB at `0xA0000000`; the PL-clock profiling timer occupies 4 KiB at `0xA0001000`. The enclosing UIO map remains at least 64 KiB. Automatic memory address assignment, `validate_bd_design`, saved BD Tcl and exported address map remain enabled.
- Bitstream stage runs implementation, requires both setup/hold timing checks to pass and exports matching BIT/HWH/XSA under `soc/hw/out/`.

## Before writing versus before running

The counter ABI and timer topology require a new r8 HLS export, Vivado implementation, paired BIT/HWH and matching software. Old scalar artifacts are not compatible with the counter-valid checks.

## 1. Package existing sequential HLS synthesis if not already exported

Preserve existing reports. If `soc/hw/hls_work/gemv/f150_b64_o8/solution/impl/ip/component.xml` already exists with matching generated register header, skip packaging.

```sh
vitis_hls -f soc/hw/gemv/export_existing.tcl
```

This helper opens the existing sequential project/solution without reset and calls `export_design`; it does NOT run C simulation, synthesis or RTL co-simulation. It checks the existing target, clock, sequential schedule, synthesized RTL and generated register ABI. Restore the existing project if unavailable; do not substitute r8 or the old bandwidth-reader export.

## 2. Run read-only Vivado preflight

From the LRC repository root:

```sh
mkdir -p soc/docs/results/lrc_p4
SOC_GEMV_BUILD_STAGE=check vivado -mode batch \
    -source soc/hw/vivado/gemv/build_vivado.tcl \
    2>&1 | tee soc/docs/results/lrc_p4/preflight.log
```

Enable your shell's pipeline-failure handling if supported (`set -o pipefail` in bash). Default stage is `check`: lists installed Ultra96 definitions, verifies export/report target/clock and generated register offsets, and creates no project. This stage needs no board preset. Return its log first.

## 3. Validate the block design using verified PS settings

The verified LRC board-store selection is:

```sh
SOC_GEMV_BOARD_REPO=/misc/scratch/np26753/project/XilinxBoardStore/boards/Avnet
SOC_GEMV_BOARD_PART=avnet.com:ultra96v2:part0:1.2
```

Set exactly one of:

- `SOC_GEMV_BOARD_PART`: an installed board identifier. Exact part equality is required except for the tutorial-approved Avnet Ultra96-V2 HLS `-e` / board `-i` pairing, or
- `SOC_GEMV_PS_CONFIG`: a course-provided Tcl file configuring the existing `$ps` cell (DDR, MIO and PS settings), not creating a second PS or replacing the whole design.

Then set `SOC_GEMV_BUILD_STAGE=bd` on the same Vivado command. Optional `SOC_GEMV_BOARD_REPO` registers board files. Run `bd` and review the log, generated `address_map.csv` and `soc_gemv_bd.tcl` before requesting `bitstream`. Do not set `SOC_GEMV_HW_PART=-i`; the exported IP and report remain `-e`, as instructed by the course tutorial.

Keep the validated design's address map. Only then use `SOC_GEMV_BUILD_STAGE=bitstream` with the same settings. Work is `soc/hw/vivado/gemv_f150_b64_o8/`; this script recreates that generated project for each BD/build stage. Source scripts live separately in `soc/hw/vivado/gemv/`.

Shared HLS knobs: `SOC_GEMV_HW_MHZ=100/150/200`, `SOC_GEMV_HW_BURST=1/4/16/64/256`, `SOC_GEMV_HW_OUTSTANDING=1..32`, `SOC_GEMV_HW_PART` (-e/-i), `SOC_GEMV_HW_ROW_INTERLEAVE=1/8`, and `SOC_GEMV_HW_DOUBLE_BUFFER=0/1`. Defaults are r8 and double buffer 1; row interleave 1 requires double buffer 0. Additional knobs: `SOC_GEMV_JOBS=8`, `SOC_GEMV_CONTROL_BASE` (64-KiB aligned in 0xA0000000..0xAFFF0000). Update HAL/UIO expectations if changing the base.

## Local script checks

```sh
python3 soc/hw/vivado/gemv/test_scripts.py
```

Checks stage selection, four AXI masters, fixed control mapping, PS requirement, target/clock/register mismatch rejection and packaging without reruns, using temporary fixtures and mocked Vitis/Vivado commands. This is not vendor validation. Next software/board steps are in `soc/sw/hal2/README.md`.

Reference: [AMD Vitis HLS 2022.2 export_design](https://docs.amd.com/r/2022.2-English/ug1399-vitis-hls/export_design).
