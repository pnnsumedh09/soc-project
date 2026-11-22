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
- A bitstream was generated; its timing report showed 100 MHz despite the f150 filename. PYNQ loaded it, then the first control read stalled and USB xHCI failed. Root cause remains unresolved. Do not repeat deployment until metadata and live PS clock/reset compatibility are reviewed after recovery.
- Rebuild entry point: `bash soc/tools/rebuild_lrc.sh software|hls|bd|bitstream`, explicitly selecting 100 MHz. Physical-board validation remains.
- The portable HAL and mocked tests exist. Actual board DMA allocation/access support must still be confirmed.
- QEMU/SystemC integration is assigned to a teammate and is not proof of FPGA-board operation.

## Task routing

- HLS change: read only `soc/hw/gemv/` and the exact referenced numerical helper.
- Register change: read only `soc/hw/gemv/registers/`, `gemv.h` and relevant HAL register definitions.
- Vivado change: read only `build_vivado.tcl`, exported HLS interface metadata and the requested Vivado log.
- HAL or board change: read only `soc/sw/hal2/`, the register map and the board support evidence supplied by the user.
- QEMU change: read `soc/docs/QEMU_SYSTEMC_HANDOFF.md`, then only the required files under `soc/lab3_ref/part3/`.
- llama.cpp call-site integration: identify the exact backend/call site with one targeted `rg` query; do not scan unrelated tools, examples, models or documentation.

Update this file only when the architecture, active paths or validation checkpoint changes.
