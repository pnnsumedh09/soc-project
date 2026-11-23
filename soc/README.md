# GEMV golden checkpoint

This checkout contains the verified sequential Q8_0 GEMV design and its
llama.cpp integration. It is independent of the original experiment checkout.
Branch: `gemv-golden`. Create new branches for subsequent design changes.

## Selected hardware

- Ultra96v2; 100 MHz PL clock; sequential rows, 16 signed int8 products per beat.
- Weight AXI interface: 128 bits, maximum 64-beat bursts, eight outstanding reads.
- Current HLS weight-loop II: 3. No FIFO profiling instrumentation.
- AXI-Lite control: 0xA0000000, 64 KiB; DDR accessed through PS HP0.
- `soc/hw/out/gemv_f100_b64_o8.bit` and matching `.hwh` are the previously
  board-tested HLS-generated design, with corrected reset wiring.
- `soc/hardware.sha256` records their known hashes.

## Source map

- `sw/repack`: existing Q8_0 weight layout conversion and scalar reference.
- `sw/gemv`: existing llama.cpp dispatch, persistent weight cache and emulation.
- `sw/hal2`: control HAL, XRT/ZOCL DMA backend, register tests and optional older
  UIO/u-dma-buf adapter sources. The selected board default is XRT, not u-dma-buf.
- `hw/gemv`: HLS C++, testbench and build scripts. The source retains the existing
  optional eight-row path, but the selected build and included bitstream use 1.
- `hw/vivado/gemv`: Vivado integration and reset/address validation.
- `lab3_ref/part3/dot_q8_0_core.{h,cpp}`: numerical helpers directly required by
  the HLS build. Historical Lab 3 drivers and SystemC experiments are excluded.
- `third_party/xrt`: board-matched XRT headers, with original notices retained.
  These are declarations for cross compilation; the runtime library stays on
  the board. No host XRT binaries or toolchain sysroot are included.

## Build and runtime defaults

Normal CMake configuration for Linux/AArch64 enables the GEMV FPGA/XRT path.
Explicit CMake options may override this. Other host platforms retain the
optional offload default. The existing CPU call-site hook remains in
`ggml/src/ggml-cpu/ggml-cpu.c`.

For an FPGA-enabled build, the runtime backend defaults to `fpga`.
`SOC_GEMV_BACKEND=emul` and `SOC_GEMV=0` remain available for diagnostics.
Verification and detailed profiling remain off by default. Startup logs must
show `soc_gemv: enabled (backend=fpga ...)` in the board validation.

Example cross build on mario (toolchain path belongs to the course environment):

```sh
/usr/bin/cmake -S . -B build-ultra96 \
  -DCMAKE_TOOLCHAIN_FILE=/home/projects/gerstl/ece382m/ultra96_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release -DLLAMA_BUILD_TESTS=OFF
/usr/bin/cmake --build build-ultra96 --target llama-cli llama-server -j8
```

Generated projects, caches, logs, SDK staging, binaries and FIFO profiling
patches are excluded. Do not copy generated profiling RTL into this checkout.

## Deployment prerequisites and limits

The bitstream must be loaded by PYNQ before starting the application. This HAL
does not program the FPGA. Current board tests used PYNQ 3.0.1, XRT/ZOCL,
generic UIO at `/dev/uio4`, root permissions, and Linux 5.15.19-xilinx-v2022.1.
Control is polled. No new kernel module or DTBO is supplied by this checkpoint.
Device mapping and installed runtime dependencies need confirmation on the grader.

The current backend has no artificial aggregate weight allocation cap.
Qwen's full repacked cache used 525.2 MB; the verified board was configured with
640 MiB CMA. The grader's CMA pool is unconfirmed. Allocation failure can cause
CPU fallback; this must be checked in the grader environment. This checkout
does not change boot settings or reboot the board.

`submission.json` is intentionally not generated until the exact model download
URL and SHA-256 are known. No submission is pushed or triggered by these scripts.
The clean checkout passed cross-build of both executables, board CLI numerical
verification, a matching noverify run, and a local HTTP server request using the
default FPGA backend. See `soc/validation/golden.json` and its server evidence.
Full course benchmarks and the grading environment remain untested. This is a
functional baseline checkpoint, not a claim that the final grading requirements
are met. Create a new branch from `gemv-golden` for subsequent design changes.
