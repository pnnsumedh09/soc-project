/*
 * soc_repack.h - Q8_0 weight repacking for the GEMV accelerator (plan task P2.1)
 *
 * llama.cpp stores a Q8_0 weight matrix as rows of 34-byte blocks:
 *
 *     block_q8_0 = { uint16 d (FP16 scale), int8 qs[32] }      34 bytes
 *     row r      = blocks [r*nb .. r*nb+nb-1]                  nb = in/32
 *
 * 34 bytes does not line up with 16-byte (128-bit) AXI beats, so the hardware
 * would have to realign every block. We repack each matrix ONCE, in memory,
 * into two separate arrays that stream cleanly:
 *
 *     quants: rows * nb * 32 bytes   int8, row-major, block-major within a row
 *     scales: rows * nb * 2 bytes    FP16 raw bits, same order as quants
 *
 * Block (r, b) lives at quants[(r*nb + b)*32 .. +31] and scales[r*nb + b].
 * Every block is exactly two 16-byte beats. Each array starts on a
 * SOC_REPACK_ALIGN (4 KB) boundary and its size is rounded up to 4 KB, with
 * zero padding, so AXI bursts never cross a 4 KB boundary at an array start.
 *
 * Nothing here touches the .gguf file on disk. The FP16 scales are copied
 * bit-for-bit (no conversion), so pack/unpack is lossless.
 *
 * Memory ownership: soc_q8r_pack() writes into buffers the caller provides,
 * so the HAL can later hand in physically contiguous, PL-visible memory
 * (CMA / reserved memory). soc_q8r_alloc_host() is a plain aligned_alloc
 * helper for the CPU-only emulation path and for tests.
 *
 * Pure C99 plus aligned_alloc (C11). No dependency on ggml headers.
 */
#ifndef SOC_REPACK_H
#define SOC_REPACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOC_QK                32      /* elements per Q8_0 block (QK8_0)          */
#define SOC_Q8_0_BLOCK_BYTES  34      /* activation block: 2 B scale + 32 int8     */
#define SOC_Q4_0_BLOCK_BYTES  18      /* weight block: 2 B scale + 16 packed nibbles */
#define SOC_REPACK_ALIGN      4096    /* AXI 4 KB boundary; base and size rounding */

/* The HLS bitstream is specialized for one weight format. Compile the
 * software with the same value; activations remain native Q8_0 in both cases. */
#ifndef SOC_GEMM_WEIGHT_BITS
#define SOC_GEMM_WEIGHT_BITS 8
#endif
#if SOC_GEMM_WEIGHT_BITS == 4
#define SOC_WEIGHT_BLOCK_BYTES SOC_Q4_0_BLOCK_BYTES
#define SOC_WEIGHT_QUANT_BYTES 16
#elif SOC_GEMM_WEIGHT_BITS == 8
#define SOC_WEIGHT_BLOCK_BYTES SOC_Q8_0_BLOCK_BYTES
#define SOC_WEIGHT_QUANT_BYTES 32
#else
#error "SOC_GEMM_WEIGHT_BITS must be 4 or 8"
#endif

/* A repacked Q8_0 matrix (out = rows, in = nb * 32). */
typedef struct soc_q8r_mat {
    uint32_t  rows;          /* output rows (ggml ne01)                        */
    uint32_t  nb;            /* blocks per row (ggml ne00 / 32)                */
    int8_t   *quants;        /* rows*nb*32 bytes used, quants_bytes allocated  */
    uint16_t *scales;        /* rows*nb entries used, scales_bytes allocated   */
    size_t    quants_bytes;  /* rounded up to SOC_REPACK_ALIGN                 */
    size_t    scales_bytes;  /* rounded up to SOC_REPACK_ALIGN                 */
    void     *host_mem;      /* owning pointer if allocated by alloc_host      */
} soc_q8r_mat;

/* Sizes of the two arrays, each rounded up to SOC_REPACK_ALIGN. 0 on overflow. */
size_t soc_q8r_quants_bytes(uint32_t rows, uint32_t nb);
size_t soc_q8r_scales_bytes(uint32_t rows, uint32_t nb);

/*
 * Repack native Q8_0 rows into (quants, scales).
 *   src            first block of row 0 (ggml tensor->data)
 *   src_row_stride bytes between rows (ggml nb01), >= nb * 34
 * quants / scales must hold at least rows*nb*32 bytes / rows*nb entries.
 * Returns 0, or -1 with errno = EINVAL on bad arguments.
 */
int soc_q8r_pack(const void *src, size_t src_row_stride, uint32_t rows, uint32_t nb,
                 int8_t *quants, uint16_t *scales);

/* Inverse of soc_q8r_pack (used by tests). dst_row_stride >= nb * 34. */
int soc_q8r_unpack(const int8_t *quants, const uint16_t *scales, uint32_t rows, uint32_t nb,
                   void *dst, size_t dst_row_stride);

/*
 * Fill m->rows/nb and point m at caller-provided buffers (e.g. from the HAL).
 * Both buffers must be SOC_REPACK_ALIGN-aligned and at least
 * soc_q8r_quants_bytes / soc_q8r_scales_bytes long. Padding is zeroed.
 * Returns 0, or -1 with errno = EINVAL.
 */
int soc_q8r_init(soc_q8r_mat *m, uint32_t rows, uint32_t nb,
                 void *quants_buf, size_t quants_buf_bytes,
                 void *scales_buf, size_t scales_buf_bytes);

/* Allocate both arrays from the heap (one 4 KB-aligned block, zeroed). */
int  soc_q8r_alloc_host(soc_q8r_mat *m, uint32_t rows, uint32_t nb);
void soc_q8r_free_host(soc_q8r_mat *m);

/* Convenience: soc_q8r_pack into an initialized matrix. */
int soc_q8r_from_q8_0(soc_q8r_mat *m, const void *src, size_t src_row_stride);

/* ---------------------------------------------------------------------------
 * Reference arithmetic (golden model for the hardware; used by tests and by
 * the CPU emulation backend in P2.3).
 *
 * The order of operations matches ggml_vec_dot_q8_0_q8_0_generic exactly:
 *     for each block b (in order):
 *         sumi  = sum_j w.qs[j] * x.qs[j]                  (int32)
 *         sumf += (float)sumi * (fp32(w.d) * fp32(x.d))     (FP32)
 * so results are bit-identical to llama.cpp's scalar path. (The NEON path
 * sums in a different order and can differ in the last bits.)
 * ------------------------------------------------------------------------- */

/* Exact FP16 -> FP32 conversion with denormal support (same as Lab 2/3). */
float soc_fp16_to_fp32(uint16_t h);

/* One row of the repacked matrix dotted with x (native Q8_0 blocks, nb of them). */
float soc_q8r_row_dot(const soc_q8r_mat *m, uint32_t row, const void *x_q8_0);

/* y[r] = row_dot(r) for all rows. */
void soc_q8r_gemv_ref(const soc_q8r_mat *m, const void *x_q8_0, float *y);

/* Same arithmetic on one native (un-repacked) Q8_0 row, for cross-checking. */
float soc_q8_0_row_dot_native(const void *w_row_q8_0, const void *x_q8_0, uint32_t nb);

#ifdef __cplusplus
}
#endif

#endif /* SOC_REPACK_H */
