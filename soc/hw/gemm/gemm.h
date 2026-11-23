#ifndef SOC_HW_GEMM_H
#define SOC_HW_GEMM_H

#include <stdint.h>

#ifdef SOC_GEMM_HW_HOST
typedef unsigned __int128 soc_gemm_word;
#else
#include <ap_int.h>
typedef ap_uint<128> soc_gemm_word;
#endif

enum {
    SOC_GEMM_HW_MAX_NB = 152,
    SOC_GEMM_HW_MAX_M = 151936,
    // Provisional until the benchmark shape logger gives the real maximum N.
    SOC_GEMM_HW_MAX_N = 4096,
    SOC_GEMM_HW_OK = 0,
    SOC_GEMM_HW_BAD_SHAPE = 1,
    SOC_GEMM_HW_BAD_X_SCALE = 2,
    SOC_GEMM_HW_BAD_W_SCALE = 3,
    SOC_GEMM_HW_BAD_WEIGHT_TYPE = 4,
    SOC_GEMM_WEIGHT_Q4_0 = 4,
    SOC_GEMM_WEIGHT_Q8_0 = 8,
};

// W is M x K in repacked Q4_0 or Q8_0 form. K = 32 * nb.
// Both formats keep one FP16 scale per 32-value block in w_scales.
// Q4_0: one 128-bit quant word/block; Q8_0: two words/block.
// X contains N native Q8_0 columns, each nb * 34 bytes.
// Y is column-major by token: Y[n * M + m].
void soc_gemm_hw(const soc_gemm_word *w_quants,
                 const uint16_t *w_scales,
                 const uint8_t *x_native,
                 float *y,
                 uint32_t m,
                 uint32_t nb,
                 uint32_t n,
                 uint32_t weight_type,
                 uint32_t &status);

#endif
