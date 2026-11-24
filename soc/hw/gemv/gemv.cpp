#include "gemv.h"

enum {
    QK4_0            = 32,
    Q4_0_QUANT_BYTES = 16,
    Q8_0_BLOCK_BYTES = 34,
};

static bool finite_half(uint16_t value) {
    return (value & 0x7c00u) != 0x7c00u;
}

static float bits_to_float(uint32_t bits) {
    union {
        uint32_t u;
        float    f;
    } value;

    value.u = bits;
    return value.f;
}

static float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t) (h & 0x8000u) << 16;
    uint32_t exp  = (h >> 10) & 0x1fu;
    uint32_t man  = h & 0x3ffu;
    uint32_t out;
    if (exp == 0x1f) {
        out = sign | 0x7f800000u | (man << 13);
    } else if (exp) {
        out = sign | ((exp + 112) << 23) | (man << 13);
    } else if (!man) {
        out = sign;
    } else {
        uint32_t word  = man << 6;
        uint32_t shift = 0;
        if (!(word & 0xff00u)) {
            shift += 8;
            word <<= 8;
        }
        if (!(word & 0xf000u)) {
            shift += 4;
            word <<= 4;
        }
        if (!(word & 0xc000u)) {
            shift += 2;
            word <<= 2;
        }
        if (!(word & 0x8000u)) {
            shift += 1;
            word <<= 1;
        }
        out = sign | ((112 - shift) << 23) | (((word >> 5) & 0x3ffu) << 13);
    }
    return bits_to_float(out);
}

static uint8_t word_byte(soc_gemv_word word, unsigned index) {
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS INLINE
#endif
    return (uint8_t) (word >> (8 * index));
}

void soc_gemv_hw(const soc_gemv_word * w_quants,
                 const uint16_t *      w_scales,
                 const uint8_t *       x_native,
                 float *               y,
                 uint32_t              rows,
                 uint32_t              nb,
                 uint32_t &            status) {
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS INTERFACE s_axilite port = return bundle = control
#    pragma HLS INTERFACE s_axilite port = w_quants bundle = control offset = 0x10
#    pragma HLS INTERFACE s_axilite port = w_scales bundle = control offset = 0x20
#    pragma HLS INTERFACE s_axilite port = x_native bundle = control offset = 0x30
#    pragma HLS INTERFACE s_axilite port = y bundle = control offset = 0x40
#    pragma HLS INTERFACE s_axilite port = rows bundle = control offset = 0x50
#    pragma HLS INTERFACE s_axilite port = nb bundle = control offset = 0x58
#    pragma HLS INTERFACE s_axilite port = status bundle = control offset = 0x60
#endif
    status = SOC_GEMV_HW_OK;
    if (rows > SOC_GEMV_HW_MAX_ROWS || nb > SOC_GEMV_HW_MAX_NB || (rows && !nb)) {
        status = SOC_GEMV_HW_BAD_SHAPE;
        return;
    }
    if (!rows) {
        return;
    }

    int8_t   x_quants[SOC_GEMV_HW_MAX_NB][QK4_0];
    uint16_t x_scale_bits[SOC_GEMV_HW_MAX_NB];
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS BIND_STORAGE variable = x_quants type = ram_2p impl = bram
#endif
#ifndef SOC_GEMV_HW_HOST
load_x_blocks:
#endif
    for (uint32_t block = 0; block < nb; ++block) {
        uint32_t base  = block * Q8_0_BLOCK_BYTES;
        uint16_t scale = (uint16_t) x_native[base] | (uint16_t) x_native[base + 1] << 8;
        if (!finite_half(scale)) {
            status = SOC_GEMV_HW_BAD_X_SCALE;
            return;
        }
        x_scale_bits[block] = scale;
#ifndef SOC_GEMV_HW_HOST
load_x_quants:
#endif
        for (unsigned i = 0; i < QK4_0; ++i) {
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS PIPELINE II = 1
#endif
            uint8_t value      = x_native[base + 2 + i];
            x_quants[block][i] = value < 128 ? (int8_t) value : (int16_t) value - 256;
        }
    }

#ifndef SOC_GEMV_HW_HOST
rows_loop:
#endif
    for (uint32_t row = 0; row < rows; ++row) {
        float sum = 0.0f;
#ifndef SOC_GEMV_HW_HOST
blocks_loop:
#endif
        for (uint32_t block = 0; block < nb; ++block) {
            uint32_t matrix_block = row * nb + block;
            uint16_t weight_scale = w_scales[matrix_block];
            if (!finite_half(weight_scale)) {
                status = SOC_GEMV_HW_BAD_W_SCALE;
                return;
            }
            soc_gemv_word quant_word = w_quants[matrix_block];
            int32_t       sum_low    = 0;
            int32_t       sum_high   = 0;
#ifndef SOC_GEMV_HW_HOST
dot_values:
#endif
            for (unsigned i = 0; i < Q4_0_QUANT_BYTES; ++i) {
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS PIPELINE II = 1
#endif
                uint8_t packed = word_byte(quant_word, i);
                sum_low += ((int32_t) (packed & 0x0f) - 8) * x_quants[block][i];
                sum_high += ((int32_t) (packed >> 4) - 8) * x_quants[block][i + Q4_0_QUANT_BYTES];
            }
            int32_t dot = sum_low + sum_high;
            sum += (float) dot * half_to_float(weight_scale) * half_to_float(x_scale_bits[block]);
        }
        y[row] = sum;
    }
}
