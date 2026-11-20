# P4.1: sequential GEMV Vivado integration

Source written; local Tcl guard/topology checks use mocked vendor commands. No actual Vivado block-design validation, implementation, routed timing or board execution has been performed here. No Ultra96 PS/DDR preset is invented. The previously observed course target is `xczu3eg-sbva484-1-e`; installed Avnet definitions may select `-i`, which must be resolved before retargeting.

## Topology

- PS `M_AXI_HPM0_FPD` → 1×1 control SmartConnect → GEMV `s_axi_control`.
- GEMV `m_axi_weights`, `m_axi_scales`, `m_axi_activation`, `m_axi_output_r` → 4×1 memory SmartConnect → PS noncoherent `S_AXI_HP0_FPD` at 128 bits.
- PL clock drives IP, interconnect and PS AXI clock inputs; `proc_sys_reset` distributes synchronous active-low resets. Polling completion; no IRQ connection.
- Control bank explicitly assigned 64 KiB at `0xA0000000` by default. Automatic memory address assignment, `validate_bd_design`, saved BD Tcl and exported address map.
- Bitstream stage runs implementation, requires both setup/hold timing checks to pass and exports matching BIT/HWH/XSA under `soc/hw/out/`.

## Before writing versus before running

Existing sequential kernel C/synthesis/RTL PASS evidence is sufficient to write this integration. No kernel rerun is required merely because scripts/HAL were added. The next boundaries are export/preflight, vendor BD validation, implemented timing, then board memory/MMIO/DMA smoke testing. Board setup and autograder permission remain unresolved; source completion is not a hardware result.

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

Set exactly one of:

- `SOC_GEMV_BOARD_PART`: an installed board identifier selecting the same FPGA part as HLS, or
- `SOC_GEMV_PS_CONFIG`: a course-provided Tcl file configuring the existing `$ps` cell (DDR, MIO and PS settings), not creating a second PS or replacing the whole design.

Then set `SOC_GEMV_BUILD_STAGE=bd` on the same Vivado command. Optional `SOC_GEMV_BOARD_REPO` registers board files. The preset's part must match; do not silently switch HLS targets. No valid choice is known in this checkout yet.

Keep the validated design's address map. Only then use `SOC_GEMV_BUILD_STAGE=bitstream` with the same settings. Work is `soc/hw/vivado/gemv_f150_b64_o8/`; this script recreates that generated project for each BD/build stage. Source scripts live separately in `soc/hw/vivado/gemv/`.

Shared HLS knobs: `SOC_GEMV_HW_MHZ=100/150/200`, `SOC_GEMV_HW_BURST=1/4/16/64/256`, `SOC_GEMV_HW_OUTSTANDING=1..32`, `SOC_GEMV_HW_PART` (-e/-i). Only `SOC_GEMV_HW_ROW_INTERLEAVE=1` is supported by this integration. Additional knobs: `SOC_GEMV_JOBS=8`, `SOC_GEMV_CONTROL_BASE` (64-KiB aligned in 0xA0000000..0xAFFF0000). Update HAL/UIO expectations if changing the base.

## Local script checks

```sh
python3 soc/hw/vivado/gemv/test_scripts.py
```

Checks stage selection, four AXI masters, fixed control mapping, PS requirement, target/clock/register mismatch rejection and packaging without reruns, using temporary fixtures and mocked Vitis/Vivado commands. This is not vendor validation. Next software/board steps are in `soc/sw/hal2/README.md`.

Reference: [AMD Vitis HLS 2022.2 export_design](https://docs.amd.com/r/2022.2-English/ug1399-vitis-hls/export_design).
