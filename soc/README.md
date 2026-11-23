# GEMV production checkpoint

The selected design is the verified sequential Q8_0 GEMV accelerator at
100 MHz, with 128-bit weight reads, 64-beat maximum bursts and eight outstanding
reads. Its current weight-processing loop achieves II=3.

## Files

- `soc/hw/gemv/gemv.cpp`, `gemv.h`: HLS design source.
- `soc/hw/common/dot_q8_0_core.cpp`, `.h`: required numerical helpers.
- `soc/hw/gemv/build_hls.tcl`: synthesis/export only; no testbench required.
- `soc/hw/vivado/gemv/build_vivado.tcl`: PS/PL integration, reset validation,
  implementation, timing checks and hardware handoff.
- `soc/hw/out/gemv_f100_b64_o8.bit`, `.hwh`: verified uninstrumented hardware.
- `soc/hardware.sha256`: hashes of the selected hardware files.
- `soc/sw/repack`: existing weight layout conversion and reference arithmetic.
- `soc/sw/gemv`: llama.cpp integration, persistent cache and backend selection.
- `soc/sw/hal2`: register-control HAL and FPGA buffer backends. XRT is selected
  by default; the older UIO/u-dma-buf adapter remains an optional source path.
- `soc/third_party/xrt`: board-matched compile-time headers and original notices.
- `soc/submission_defaults.cmake`: ARM/Linux FPGA defaults; tests disabled.

Upstream test directories, SoC tests, testbenches, validation logs, cleanup
manifests and generated experiment projects are excluded from this checkout.
Earlier verification evidence remains outside the production tree and in the
preceding local checkpoint. No handwritten FIFO profiling RTL is included.

## Software build

On mario:

```sh
/usr/bin/cmake -S . -B build-ultra96-production \
  -DCMAKE_TOOLCHAIN_FILE=/home/projects/gerstl/ece382m/ultra96_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
/usr/bin/cmake --build build-ultra96-production --target llama-cli llama-server -j8
```

Linux/AArch64 defaults select FPGA/XRT. FPGA-enabled builds default to the
`fpga` runtime backend. `SOC_GEMV=0` disables offload and
`SOC_GEMV_BACKEND=emul` selects the retained software reference backend.
The existing llama.cpp call-site hook is in `ggml/src/ggml-cpu/ggml-cpu.c`.

## Hardware rebuild

With Vivado/Vitis HLS 2022.2 loaded, run from the repository root:

```sh
env SOC_GEMV_HW_MHZ=100 SOC_GEMV_HW_ROW_INTERLEAVE=1 \
  SOC_GEMV_HW_STAGE=export vitis_hls -f soc/hw/gemv/build_hls.tcl
env SOC_GEMV_HW_MHZ=100 SOC_GEMV_HW_ROW_INTERLEAVE=1 \
  SOC_GEMV_BUILD_STAGE=bitstream \
  SOC_GEMV_BOARD_PART=avnet.com:ultra96v2:part0:1.2 \
  SOC_GEMV_BOARD_REPO=/misc/scratch/np26753/project/XilinxBoardStore/boards/Avnet \
  vivado -mode batch -source soc/hw/vivado/gemv/build_vivado.tcl
```

HLS regenerates RTL from C++. Do not manually edit the generated design RTL.
Rebuilding hardware is a separate operation and replaces generated outputs;
the production cleanup does not run either hardware command.

## Deployment and submission

Load the matching bitstream/HWH through PYNQ before starting the application.
The runtime uses generic UIO at `/dev/uio4` for 0xA0000000 control and the existing
XRT/ZOCL allocator for DDR. It polls completion. No custom module/DTBO is supplied.
The verified board used PYNQ 3.0.1, root permissions and 640 MiB CMA.
Full Qwen repacked weights occupied about 525.2 MB. The grader's CMA capacity,
UIO mapping and runtime dependencies still require confirmation.

Cross-build and short board CLI/server checks passed before source cleanup.
The production cleanup rebuilds both executables but does not claim a new
hardware run or full benchmark validation. Accuracy/performance grading,
model download URL/hash and `submission.json` remain pending.

Use `gemv-golden` as the baseline and create separate branches for new designs.
No remote is configured and nothing is pushed by the local cleanup scripts.
