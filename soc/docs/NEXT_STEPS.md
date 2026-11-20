# Next steps for Sumedh: LRC and Ultra96 checks

Updated: 2026-11-20. Working checklist for the current handoff. `../../SOC_PROJECT_PLAN.md` remains the master task/status document; this file adds execution order and evidence to return, not a second project plan.

## What is already written

- Q8_0 repack, GEMV hook/cache and emulation backend (P2.1-P2.3): implemented; local tests and prior sandbox end-to-end results exist. Target-board validation remains pending.
- Bandwidth experiment (P1.1): native/Vitis C simulation, synthesis, Verilog co-simulation and IP export pass; separate monitor pass reported. Board integration and bandwidth measurements remain pending.
- GEMV engine (P3.1): provisional kernel, HLS script and starter testbench exist in `../hw/gemv/`. Local golden synthetic/real-slice tests pass; Vitis C simulation passes and synthesis is reviewed; baseline Verilog C/RTL co-simulation passes; the new eight-row candidate is locally tested, with vendor checks pending.
- PRD v0.1 (P0.7): drafted in `PRD.md`; model selection and hardware feasibility are explicitly provisional.

## Current focus: P4.1/P4.2 source written; integration checks next

The default remains the simple sequential GEMV, with existing C/synthesis/RTL PASS and compute II=5. No additional kernel simulation was required to write integration/HAL source. Row interleaving and double buffering remain deferred.

New source and local validation: `results/p4_integration_source_validation.md`, `../hw/vivado/gemv/README.md` and `../sw/hal2/README.md`. HAL/mock-control/arena/fallback/UBSan checks pass; both software-hook build variants retain exact emulation results. Vendor tools, target CMake build and real DMA have NOT been validated locally. The Linux adapter is optional because course-image UIO/allocator support is unknown.

### On LRC now

Pull the new source. If `soc/hw/hls_work/gemv/f150_b64_o8/solution/impl/ip/component.xml` is absent, package the existing sequential synthesis (do not reset/rebuild it):

```bash
vitis_hls -f soc/hw/gemv/export_existing.tcl
```

Then run the non-building preflight:

```bash
mkdir -p soc/docs/results/lrc_p4
SOC_GEMV_BUILD_STAGE=check vivado -mode batch \
    -source soc/hw/vivado/gemv/build_vivado.tcl \
    2>&1 | tee soc/docs/results/lrc_p4/preflight.log
```

Return `preflight.log`. It checks exported target/clock/register ABI and lists board definitions without needing a PS preset. Actual BD/build requires the matching course PS configuration or installed board definition; do not silently switch -e/-i targets. See the P4.1 README for subsequent BD/bitstream stages.

### On the board when available

Read-only discovery, before assuming any allocator or accessing registers:

```bash
python3 soc/tools/board_preflight.py > board_preflight.json
```

Return the JSON. No driver installation or device-tree changes are authorized/required by this discovery. After confirming image support, matching hardware/PS setup and device-visible memory, build/run `soc/sw/hal2/board_smoke` before full inference. The optional adapter's UIO/u-dma-buf, low-DDR addressability and noncached write-combined pool must be confirmed; adapt it if the course image supports another mechanism. Board baseline/memory/bandwidth and ASIC gates remain pending.

### Completed C-simulation instructions (reference)

GEMV v1 source, testbench and HLS script now exist under `soc/hw/gemv/`. Native synthetic tests and eight real Qwen slices pass bit-exactly against the existing golden reference; UndefinedBehaviorSanitizer passes. See `results/p3_gemv_source_validation.md`. The kernel is standalone and not connected to the FPGA software backend yet.

Sync the new directory and existing repack/Lab 3 dependencies to LRC. In the Xilinx 2022.2 environment, from the repository root:

```bash
set -o pipefail
mkdir -p soc/docs/results/lrc_p3
SOC_GEMV_HW_STAGE=csim vitis_hls -f soc/hw/gemv/build_hls.tcl \
    2>&1 | tee soc/docs/results/lrc_p3/csim.log
```

Expected summary: `GEMV HW: PASS`, 54 calls and zero failures. Return this new GEMV C-simulation log before synthesis. It requires no board preset or QEMU. These C-simulation instructions are retained for reference; synthesis review and baseline RTL numerical checks have since completed. Implemented hardware timing remains pending. This new kernel requires its own checks; do not repeat the completed P1.1 tests.

The earlier bandwidth-reader integration instructions below remain available for later board measurements. They are not a prerequisite to this GEMV source-validation step.

## Completed P1.1 and later bandwidth integration (2026-11-20)

The supplied export log confirms Vitis C simulation, synthesis, Verilog C/RTL co-simulation (60 transactions) and IP export completed successfully for f150_b64_o8. Sumedh also confirms the separate cycle-counter simulation passes; its raw log is not yet supplied. Do not rerun completed tests for the unchanged source/configuration. Board bandwidth remains unmeasured.

When resuming the separate bandwidth-board path, its next task is Vivado integration. Copy the updated `soc/hw/bw_test/build_vivado.tcl` to LRC, then run this from the cloned repository root in the Xilinx 2022.2 environment:

```bash
set -o pipefail
mkdir -p soc/docs/results/lrc_p1
SOC_BW_BUILD_STAGE=check vivado -mode batch -source soc/hw/bw_test/build_vivado.tcl \
    2>&1 | tee soc/docs/results/lrc_p1/vivado_preflight.log
```

Return `vivado_preflight.log` first. This stage lists installed Ultra96 definitions with their exact device parts and checks the existing HLS component/report; it creates no design or bitstream. The board definition/course PS configuration must match the intended target. Official current Avnet Ultra96-V2 files name an industrial-grade (-i) part, while our completed HLS run targets the course -e part; resolve the actual target before selecting a preset. Do not retarget or rerun HLS merely because a different board definition is listed.

After the preset/configuration is resolved, run `SOC_BW_BUILD_STAGE=bd` with that selection; inspect block-design validation before running `SOC_BW_BUILD_STAGE=bitstream` with the same settings. These Vivado stages reuse the HLS export. See the bandwidth README for `SOC_BW_BOARD_PART`, `SOC_BW_PS_CONFIG`, optional user-owned `SOC_BW_BOARD_REPO`, and explicit `SOC_BW_PART` settings.

## LRC session instructions (reference)

The initial source transfer and P1.1 simulation/export runs are complete. Sections 1 and 3 retain the original commands for reproducibility, not as requests to repeat them. Sync the latest integration-script changes before the current preflight command above. Keep the validated 150 MHz, burst 64, outstanding 8 configuration until there is evidence to change it.

Use `set -o pipefail` if piping commands through `tee`; otherwise a successful `tee` can hide a failed tool exit. Save C simulation, monitor RTL and export logs separately under `soc/docs/results/lrc_p1/`. These first checks require neither a board nor a PS preset. Bitstream implementation in section 4 still requires the correct board preset/PS configuration.

## 1. Transfer the current sources to LRC

- [x] Initial project source pushed and cloned on LRC (reported by Sumedh). Subsequent local integration-script edits still need to be synced before use.
- [ ] Keep the project branch `soc-gemv-accel`. Do not push to the upstream `origin` remote.
- [ ] Record source revision, local changes, compiler/tool versions and board image. Save logs with their build settings so results can be reproduced.

## 2. Resolve the board setup and submission questions (P0.5)

- [ ] Read the course Ultra96 tutorial and the LRC submission schema/example. Confirm how the bitstream is loaded and how the executable accesses registers and DMA memory.
- [ ] Check whether the board image has PYNQ `Overlay`, `allocate`, `Clocks` and buffer `flush`. The supplied runner assumes these; support has not been confirmed.
- [ ] Supply either an installed, correct Ultra96-V2 board part or an exported PS configuration from a working course design. Follow `../hw/bw_test/README.md`; do not substitute another board's DDR/MIO preset.
- [ ] Inspect the real `benchmark.py --help` and source. Confirm executable-vs-URL arguments, thread count, CPU-clock policy, prompt/context sizes and whether the score includes prefill. Existing profiling commands are templates until this is checked.
- [ ] Confirm whether the project requires a specific QEMU/SystemC submission, beyond its stated HW/SW co-verification requirement. The supplied project handout does not explicitly mandate QEMU.

Return: the tutorial or relevant excerpts, PS preset/configuration, benchmark script/help and submission schema/example. Record resolved questions in `results/autograder_flow.md`.

## 3. Completed P1.1 simulations (commands retained for reference)

From the llama.cpp root:

```bash
module load xilinx/2022
source /usr/local/packages/Xilinx_2022.2/Vivado/2022.2/settings64.sh
make -C soc/hw/bw_test test
SOC_BW_STAGE=csim vitis_hls -f soc/hw/bw_test/build_hls.tcl
vivado -mode batch -source soc/hw/bw_test/test_rtl.tcl
```

- [x] Native and Vitis C simulation pass.
- [x] RTL monitor simulation passes (reported by Sumedh; raw log not yet supplied).

Then synthesize, RTL co-simulate and export the first configuration:

```bash
SOC_BW_MHZ=150 SOC_BW_BURST=64 SOC_BW_OUTSTANDING=8 SOC_BW_STAGE=export \
    vitis_hls -f soc/hw/bw_test/build_hls.tcl
```

- [x] C simulation, Verilog RTL co-simulation (60 transactions) and IP export pass; verified from the supplied export log. No rerun needed for unchanged source/configuration.
- [x] Synthesis reports reviewed: 128-bit interface, read-loop II=1, burst inference and matching register offsets; resource/timing estimates recorded in the validation document.

Return: complete failing logs if any; otherwise the synthesis report and co-simulation summary. Do not label this step a board bandwidth result.

## 4. Build the bandwidth-test bitstream on LRC

Use the exact PS configuration or board part confirmed in step 2. For example:

```bash
SOC_BW_BUILD_STAGE=bd SOC_BW_PS_CONFIG=/absolute/path/ultra96v2_ps.tcl \
SOC_BW_MHZ=150 SOC_BW_BURST=64 SOC_BW_OUTSTANDING=8 \
    vivado -mode batch -source soc/hw/bw_test/build_vivado.tcl
```

- [ ] First run `bd` and verify block-design validation. Then run `bitstream` with the same settings and verify synthesis/implementation complete. Preserve logs before the generated project is recreated.
- [ ] Setup/hold timing pass; review unconstrained paths and DRC warnings.
- [ ] Matching `bw_test_f150_b64_o8.bit` and `.hwh` exist under `soc/hw/out/`.

Return: build errors if any, or timing/utilization reports and the exact preset/configuration used. This is the bandwidth-test bitstream, not the final GEMV accelerator.

## 5. Board baseline and software-hook validation (can run while LRC builds)

Follow `PROFILING_GUIDE.md` and `../sw/gemv/README.md` for cross-build details; use the real benchmark interface from step 2.

- [ ] Build stock Release and `-DGGML_SOC_GEMV=ON` Release executables with the Ultra96 toolchain. Confirm the hook's CMake enable message. Keep profiling instrumentation out of speed builds.
- [ ] Configure one online A53 core at 300 MHz; verify actual frequency and online cores before each measurement.
- [ ] Smoke-test Qwen, then record stock decode/prefill speed, score and full-suite duration. Save `results/baseline_300mhz.md` plus raw logs.
- [ ] Compare deterministic stock and emulation-hook output. Use the same sampling settings, prompt, token count and thread count. Check offload/cache/fallback statistics.
- [ ] Run the hook-enabled server through the official benchmark to check accuracy. Emulation is a correctness reference, not an FPGA speed prediction.
- [ ] Sweep a short model/quant shortlist after the baseline works. Save `results/model_sweep_300mhz.md`.

When setting environment variables with CPU affinity, use `env SOC_GEMV_STATS=1 taskset -c 0 ./cli-soc ...`, not `taskset -c 0 SOC_GEMV_STATS=1 ...`.

## 6. Check DMA-visible memory (P0.6)

- [ ] Record `free -h`, CMA total/free and reserved-memory configuration, first idle and then with llama-server running.
- [ ] Check allocation and device addresses with the allocator approved in step 2. A virtual heap address is not a DMA address.
- [ ] Test the actual per-matrix allocation sequence: about 525 MB total, with the largest matrix about 145 MB. Check the quants/scales allocation layout and alignment. Merely checking free RAM or allocating one small buffer does not pass this gate.
- [ ] Repeat after reboot and record failures, fragmentation, peak resident memory and swap usage. Save `results/board_memory.md`.

Return initial CMA/free-memory data early. A successful small-buffer bandwidth test is useful even if the full allocation test still needs a dedicated script.

## 7. Measure bandwidth and empty-job overhead (P1.2)

After steps 2-4 pass, copy the matching BIT/HWH pair and `run_board.py` to the board. If PYNQ is available:

```bash
sudo python3 run_board.py bw_test_f150_b64_o8.bit --clock-mhz 150 \
    --sizes 16384 1048576 16777216 --repeats 10 --empty-repeats 1000 \
    > pl_read_f150_b64_o8.csv
```

- [ ] Every checksum passes, empty jobs show zero read cycles, and nonempty jobs report valid cycles.
- [ ] Preserve raw samples and record CPU state and PL configuration. Distinguish FPGA read-window GB/s from CPU `job_ns`; the Python empty-job result is not the future C HAL overhead.
- [ ] Send the first working configuration's CSV before running a large sweep. Then compare useful burst/outstanding/clock settings, and CPU-idle versus CPU-active conditions.
- [ ] Save the measured summary in `results/pl_read_bandwidth.md`. A two-HP-port variant is future work, not supported by the initial overlay.

## Verification checkpoints: write in parallel, test at boundaries

Do not wait until every component is written to test. We also do not need to stop all work for every individual file change.

| Checkpoint | Evidence required before advancing that path |
|---|---|
| Algorithm/layout | Local tests agree with the independent/golden reference; already available for repack/emulation |
| HLS source | Vitis C simulation passes before trusting hardware generation |
| Generated RTL | HLS synthesis reports reviewed and RTL co-simulation passes; custom monitor simulated separately |
| FPGA implementation | Block-design validation and implementation timing pass before board loading |
| Hardware protocol | One known-buffer transfer or GEMV matches the reference; repeat across sizes and reuse before full-model inference |
| End-to-end system | Full decode and official benchmark pass with the real backend; measure speed/memory and verify score/suite limits |

On a failure, pause dependent integration and return the first error plus its full log/configuration. Independent documentation, test-vector preparation and analysis can continue.

## QEMU/SystemC decision

Recommendation: defer a new GEMV QEMU model for now. Do the HLS C/RTL checks and the small board test first. Revisit P3.4 if the course requires it, board access is delayed, or software/protocol debugging would benefit from a virtual platform.

The project handout requires HW/SW co-verification and cites Lab 3 and the board tutorial as examples. It does not explicitly prescribe QEMU as a prerequisite to bitstream loading. This is our reading of the provided handout; check any additional tutorial/rubric requirements in step 2.

QEMU runs the ARM software. In Lab 3, a separate SystemC/TLM model represented the accelerator through remote-port. That model does not simulate the generated GEMV RTL. HLS RTL co-simulation tests generated RTL against the testbench, while QEMU/SystemC tests software/model interaction. Neither gives measured physical DDR bandwidth, FPGA timing closure or real cache-coherency behavior.

The old platform's `zynqmp_dot.cc` connects only the control/slave path and calls `zynq.tie_off()` for unused ports. A useful GEMV version needs a new GEMV register/data model, an HP-port memory initiator, DMA-visible guest buffers and a compatible HAL. Re-running the old dot-product simulation verifies the old design, not the new DMA architecture. Use a tiny matrix first, then TinyStories with 1-10 tokens if this path is pursued.

Reference locations: `reference/lab3_handout_text.txt`, `reference/lab3_report_text.txt`, `../lab3_ref/LAB3_README.md`, and the original `../../../ece382m-20-fa26-lab-3-group-18-main/` directory. The copied `soc/lab3_ref/part3/cosim/setup.sh` expects a `Makefile.dot` missing from that reference copy; it is present in the original Lab 3 directory. Keep the reference copy unchanged and use the original working setup if rerunning Lab 3.

## What Codex can work on while these runs happen

- PRD and requirement-to-test traceability: v0.1 now drafted in `PRD.md`.
- Early ASIC area/power envelope, with estimates and uncertainty clearly marked (P1.3).
- GEMV v1 source and native golden tests are now implemented; review the LRC GEMV C/synthesis/RTL results next (P3.1/P3.2/P3.3). Retain the current Q8_0 layout and backend boundary while optimizing from evidence.
- Final lane count, clock, DMA allocation strategy and board HAL depend on the measurements and access mechanism above. Do not mark architecture gates passed or final hardware validated before that evidence arrives.
