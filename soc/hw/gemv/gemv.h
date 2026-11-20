#ifndef SOC_HW_GEMV_H
#define SOC_HW_GEMV_H

#include <stdint.h>

#ifdef SOC_GEMV_HW_HOST
typedef unsigned __int128 soc_gemv_word;
#else
#include <ap_int.h>
typedef ap_uint<128> soc_gemv_word;
#endif

// 1 retains the verified sequential baseline; 8 enables row interleaving.
#ifndef SOC_GEMV_HW_ROW_INTERLEAVE
#define SOC_GEMV_HW_ROW_INTERLEAVE 8
#endif
static_assert(SOC_GEMV_HW_ROW_INTERLEAVE == 1 || SOC_GEMV_HW_ROW_INTERLEAVE == 8,
              "Select the baseline (1) or eight-row interleaving (8)");

enum {
    SOC_GEMV_HW_MAX_NB = 152,
    SOC_GEMV_HW_MAX_ROWS = 151936,
    SOC_GEMV_HW_OK = 0,
    SOC_GEMV_HW_BAD_SHAPE = 1,
    SOC_GEMV_HW_BAD_X_SCALE = 2,
    SOC_GEMV_HW_BAD_W_SCALE = 3,
};

void soc_gemv_hw(const soc_gemv_word * w_quants, const uint16_t * w_scales,
                 const uint8_t * x_native, float * y,
                 uint32_t rows, uint32_t nb, uint32_t & status);

#endif
