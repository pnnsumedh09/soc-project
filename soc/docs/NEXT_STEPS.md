# Next steps for Sumedh: LRC and Ultra96 checks

Updated: 2026-11-19. Working checklist for the current handoff. `../../SOC_PROJECT_PLAN.md` remains the master task/status document; this file adds execution order and evidence to return, not a second project plan.

## What is already written

- Q8_0 repack, GEMV hook/cache and emulation backend (P2.1-P2.3): implemented; local tests and prior sandbox end-to-end results exist. Target-board validation remains pending.
- Bandwidth experiment (P1.1): kernel, functional testbench, RTL cycle monitor/testbench, HLS/Vivado scripts and optional PYNQ runner exist in `../hw/bw_test/`. Native tests pass. Vendor-tool execution and board results remain pending.
- PRD v0.1 (P0.7): drafted in `PRD.md`; model selection and hardware feasibility are explicitly provisional.

## 1. Transfer the current sources to LRC

- [ ] Copy the current working tree, including the untracked `soc/` directory and the modified `ggml/src/ggml-cpu/ggml-cpu.c` and `CMakeLists.txt`. A fresh upstream clone alone does not contain this work.
- [ ] Keep the project branch `soc-gemv-accel`. Do not push to the upstream `origin` remote.
- [ ] Record source revision, local changes, compiler/tool versions and board image. Save logs with their build settings so results can be reproduced.

## 2. Resolve the board setup and submission questions (P0.5)

- [ ] Read the course Ultra96 tutorial and the LRC submission schema/example. Confirm how the bitstream is loaded and how the executable accesses registers and DMA memory.
- [ ] Check whether the board image has PYNQ `Overlay`, `allocate`, `Clocks` and buffer `flush`. The supplied runner assumes these; support has not been confirmed.
- [ ] Supply either an installed, correct Ultra96-V2 board part or an exported PS configuration from a working course design. Follow `../hw/bw_test/README.md`; do not substitute another board's DDR/MIO preset.
- [ ] Inspect the real `benchmark.py --help` and source. Confirm executable-vs-URL arguments, thread count, CPU-clock policy, prompt/context sizes and whether the score includes prefill. Existing profiling commands are templates until this is checked.
- [ ] Confirm whether the project requires a specific QEMU/SystemC submission, beyond its stated HW/SW co-verification requirement. The supplied project handout does not explicitly mandate QEMU.

Return: the tutorial or relevant excerpts, PS preset/configuration, benchmark script/help and submission schema/example. Record resolved questions in `results/autograder_flow.md`.

## 3. Run the P1.1 simulations on LRC before building its bitstream

From the llama.cpp root:

```bash
module load xilinx/2022
source /usr/local/packages/Xilinx_2022.2/Vivado/2022.2/settings64.sh
make -C soc/hw/bw_test test
SOC_BW_STAGE=csim vitis_hls -f soc/hw/bw_test/build_hls.tcl
vivado -mode batch -source soc/hw/bw_test/test_rtl.tcl
```

- [ ] Native and Vitis C simulation pass.
- [ ] The RTL monitor simulation prints `read window: PASS` with no fatal errors.

Then synthesize, RTL co-simulate and export the first configuration:

```bash
SOC_BW_MHZ=150 SOC_BW_BURST=64 SOC_BW_OUTSTANDING=8 SOC_BW_STAGE=export \
    vitis_hls -f soc/hw/bw_test/build_hls.tcl
```

- [ ] C simulation and RTL co-simulation pass. The export stage reruns earlier checks and resets the generated project for this configuration; preserve any earlier logs you need first.
- [ ] Check that the memory interface is 128 bits, the read loop achieves II=1, and bursts are inferred. Inspect warnings, latency and DSP/LUT/FF/BRAM reports. Check the generated register offsets against the README/runner.

Return: complete failing logs if any; otherwise the synthesis report and co-simulation summary. Do not label this step a board bandwidth result.

## 4. Build the bandwidth-test bitstream on LRC

Use the exact PS configuration or board part confirmed in step 2. For example:

```bash
SOC_BW_PS_CONFIG=/absolute/path/ultra96v2_ps.tcl \
SOC_BW_MHZ=150 SOC_BW_BURST=64 SOC_BW_OUTSTANDING=8 \
    vivado -mode batch -source soc/hw/bw_test/build_vivado.tcl
```

- [ ] Block-design validation, synthesis and implementation complete.
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
- Provisional GEMV arithmetic/kernel and golden-reference testbench (P3.1/P3.2), retaining the current Q8_0 layout and backend boundary. Native tests should accompany that source work.
- Final lane count, clock, DMA allocation strategy and board HAL depend on the measurements and access mechanism above. Do not mark architecture gates passed or final hardware validated before that evidence arrives.
