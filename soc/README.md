# Q4_K GEMV accelerator

This branch extends `gemv-golden` with a sequential Q4_K matrix-vector accelerator for Q4_K_M GGUF models. The ggml hook offloads decode-time `Q4_K x Q8_K` matrix-vector operations. Q6_K, Q4_0, and other tensor types in a Q4_K_M model continue through llama.cpp's CPU implementation.

The hardware input limit is 19 Q4_K superblocks, or 4864 columns, with at most 151936 output rows. Unsupported shapes fall back to the supplied llama.cpp vector-dot function. The branch does not add or change a GGUF quantization type.

## Data layout

Each native 144-byte Q4_K superblock is repacked once into two aligned streams:

- `w_quants`: 128 bytes of packed 4-bit values, or eight 128-bit AXI beats.
- `w_metadata`: one 128-bit beat containing FP16 `d`, FP16 `dmin`, and 12 packed scale/min bytes.

The activation remains llama.cpp's native 292-byte Q8_K block: one FP32 scale, 256 signed bytes, and sixteen signed 16-bit partial sums. The kernel follows llama.cpp's eight-lane accumulation order.

## Software and profiling

`soc/sw/repack` supplies the repacker and reference arithmetic. `soc/sw/gemv` owns the persistent matrix cache, fallback, verification, and global statistics. The default Linux/AArch64 build selects the XRT/ZOCL allocator and generic UIO register control.

Set `SOC_GEMV_PROFILE=1` to retain the golden XRT phase profile. It reports activation copy, weight-quant sync, weight-metadata sync, activation sync, ownership checks, HAL submit/wait, output sync, and output copy. `SOC_GEMV_STATS=1` prints aggregate offload, cache, repack, and verification statistics.

The Q8_0 `.bit` and `.hwh` files inherited from `gemv-golden` are incompatible reference artifacts. Do not load or package them with this software. Rebuild the Q4_K hardware; its generated stem is `gemv_q4k_f100_b64_o8` for the default 100 MHz configuration.

## Validation state

Host C simulation, the full llama.cpp emulation build, the cache/dispatcher, and Linux HAL/backend syntax checks pass. Vitis HLS synthesis/co-simulation, Vivado timing closure, and Ultra96 execution require the LRC and board workflow in [GEMV_Q4_K_M_BUILD_AND_BOARD_RUN.md](../GEMV_Q4_K_M_BUILD_AND_BOARD_RUN.md).
