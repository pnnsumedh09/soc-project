# Q4_0 GEMV accelerator

This branch adapts `gemv-q4` to accelerate decode-time `Q4_0 x Q8_0` matrix-vector operations. It does not add a GGUF type. Only native Q4_0 weight tensors enter the accelerator; every other tensor type remains on llama.cpp's CPU path.

The hardware limit is 152 Q4_0 blocks, or 4864 columns, and 151936 output rows. Unsupported shapes fall back to llama.cpp's supplied vector-dot function.

## Data layout

Each native 18-byte Q4_0 block is repacked once into:

- `w_quants`: one 128-bit word containing the 16 packed nibble bytes.
- `w_scales`: one FP16 scale per block.

The activation stays in llama.cpp's native 34-byte Q8_0 layout: one FP16 scale followed by 32 signed bytes. The kernel preserves the generic Q4_0 vector-dot accumulation order.

## Software and profiling

`soc/sw/repack` owns the layout and reference arithmetic. `soc/sw/gemv` owns dispatch, fallback, verification, and the persistent weight cache. `soc/sw/hal2` owns register programming and the UIO/XRT adapters.

The golden profiling behavior remains available. `SOC_GEMV_PROFILE=1` reports the XRT copy, synchronization, ownership, HAL, and output phases. `SOC_GEMV_STATS=1` reports aggregate cache, repack, offload, fallback, and verification data.

Do not use the inherited Q8_0 or Q4_K hardware files with this software. The default Q4_0 build produces `gemv_q4_0_f100_b64_o8.bit`, `.hwh`, and `.xsa`.

## Validation state

The standalone kernel/repacker, mock HAL test, dispatcher/cache test, and warning-as-error Linux checks for the HAL, generic UIO backend, and XRT backend pass. Vitis HLS and Vivado remain user-run steps in [Q4_0_VITIS_VIVADO_HANDOFF.md](../Q4_0_VITIS_VIVADO_HANDOFF.md).
