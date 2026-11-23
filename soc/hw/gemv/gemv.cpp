#include "gemv.h"

enum {
    QK_K             = 256,
    Q4_K_QUANT_WORDS = 8,
    Q8_K_BLOCK_BYTES = 292,
};

static bool finite_half(uint16_t h) {
    return (h & 0x7c00u) != 0x7c00u;
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
        uint32_t w = man << 6;
        uint32_t n = 0;
        if (!(w & 0xff00u)) {
            n += 8;
            w <<= 8;
        }
        if (!(w & 0xf000u)) {
            n += 4;
            w <<= 4;
        }
        if (!(w & 0xc000u)) {
            n += 2;
            w <<= 2;
        }
        if (!(w & 0x8000u)) {
            n += 1;
            w <<= 1;
        }
        out = sign | ((112 - n) << 23) | (((w >> 5) & 0x3ffu) << 13);
    }
    return bits_to_float(out);
}

static uint8_t word_byte(soc_gemv_word word, unsigned index) {
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS INLINE
#endif
    return (uint8_t) (word >> (8 * index));
}

static void scale_min(unsigned group, const uint8_t packed[12], uint8_t & scale, uint8_t & min) {
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS INLINE
#endif
    if (group < 4) {
        scale = packed[group] & 63;
        min   = packed[group + 4] & 63;
    } else {
        scale = (packed[group + 4] & 0x0f) | ((packed[group - 4] >> 6) << 4);
        min   = (packed[group + 4] >> 4) | ((packed[group] >> 6) << 4);
    }
}

void soc_gemv_hw(const soc_gemv_word * w_quants,
                 const soc_gemv_word * w_metadata,
                 const uint8_t *       x_native,
                 float *               y,
                 uint32_t              rows,
                 uint32_t              nb,
                 uint32_t &            status) {
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS INTERFACE s_axilite port = return bundle = control
#    pragma HLS INTERFACE s_axilite port = w_quants bundle = control offset = 0x10
#    pragma HLS INTERFACE s_axilite port = w_metadata bundle = control offset = 0x20
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

    int8_t  x_quants[SOC_GEMV_HW_MAX_NB][QK_K];
    int16_t x_bsums[SOC_GEMV_HW_MAX_NB][QK_K / 16];
    float   x_scales[SOC_GEMV_HW_MAX_NB];
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS BIND_STORAGE variable = x_quants type = ram_2p impl = bram
#    pragma HLS BIND_STORAGE variable = x_bsums type = ram_1p impl = bram
#endif
load_x_blocks:
    for (uint32_t block = 0; block < nb; ++block) {
        uint32_t base       = block * Q8_K_BLOCK_BYTES;
        uint32_t scale_bits = (uint32_t) x_native[base] | (uint32_t) x_native[base + 1] << 8 |
                              (uint32_t) x_native[base + 2] << 16 | (uint32_t) x_native[base + 3] << 24;
        float scale = bits_to_float(scale_bits);
        if (!(scale == scale) || scale > 3.402823466e+38F || scale < -3.402823466e+38F) {
            status = SOC_GEMV_HW_BAD_X_SCALE;
            return;
        }
        x_scales[block] = scale;
load_x_quants:
        for (unsigned i = 0; i < QK_K; ++i) {
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS PIPELINE II = 1
#endif
            uint8_t value      = x_native[base + 4 + i];
            x_quants[block][i] = value < 128 ? (int8_t) value : (int16_t) value - 256;
        }
load_x_bsums:
        for (unsigned i = 0; i < QK_K / 16; ++i) {
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS PIPELINE II = 1
#endif
            uint32_t offset   = base + 4 + QK_K + 2 * i;
            x_bsums[block][i] = (int16_t) ((uint16_t) x_native[offset] | (uint16_t) x_native[offset + 1] << 8);
        }
    }

rows_loop:
    for (uint32_t row = 0; row < rows; ++row) {
        float sums[8] = { 0 };
        float sumf    = 0.0f;
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS ARRAY_PARTITION variable = sums complete
#endif
blocks_loop:
        for (uint32_t block = 0; block < nb; ++block) {
            uint32_t      matrix_block = row * nb + block;
            soc_gemv_word metadata     = w_metadata[matrix_block];
            uint16_t      d_bits       = (uint16_t) word_byte(metadata, 0) | (uint16_t) word_byte(metadata, 1) << 8;
            uint16_t      dmin_bits    = (uint16_t) word_byte(metadata, 2) | (uint16_t) word_byte(metadata, 3) << 8;
            if (!finite_half(d_bits) || !finite_half(dmin_bits)) {
                status = SOC_GEMV_HW_BAD_W_SCALE;
                return;
            }
            uint8_t packed_scales[12];
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS ARRAY_PARTITION variable = packed_scales complete
#endif
            for (unsigned i = 0; i < 12; ++i) {
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS UNROLL
#endif
                packed_scales[i] = word_byte(metadata, i + 4);
            }
            soc_gemv_word qwords[Q4_K_QUANT_WORDS];
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS ARRAY_PARTITION variable = qwords complete
#endif
read_quant_words:
            for (unsigned word = 0; word < Q4_K_QUANT_WORDS; ++word) {
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS PIPELINE II = 1
#endif
                qwords[word] = w_quants[matrix_block * Q4_K_QUANT_WORDS + word];
            }
            int32_t lanes[8] = { 0 };
            int32_t min_sum  = 0;
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS ARRAY_PARTITION variable = lanes complete
#endif
groups_loop:
            for (unsigned group = 0; group < 8; ++group) {
                uint8_t scale, min;
                scale_min(group, packed_scales, scale, min);
                unsigned chunk = group / 2;
group_values:
                for (unsigned i = 0; i < 32; ++i) {
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS PIPELINE II = 1
#endif
                    soc_gemv_word word   = qwords[chunk * 2 + i / 16];
                    uint8_t       packed = word_byte(word, i & 15);
                    int32_t       weight = group & 1 ? packed >> 4 : packed & 0x0f;
                    lanes[i & 7] += (int32_t) scale * weight * x_quants[block][group * 32 + i];
                }
                min_sum += min * (x_bsums[block][2 * group] + x_bsums[block][2 * group + 1]);
            }
            float d = half_to_float(d_bits) * x_scales[block];
accumulate_lanes:
            for (unsigned lane = 0; lane < 8; ++lane) {
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS UNROLL
#endif
                sums[lane] += d * lanes[lane];
            }
            float dmin = half_to_float(dmin_bits) * x_scales[block];
            sumf -= dmin * min_sum;
        }
finish_lanes:
        for (unsigned lane = 0; lane < 8; ++lane) {
#ifndef SOC_GEMV_HW_HOST
#    pragma HLS PIPELINE II = 1
#endif
            sumf += sums[lane];
        }
        y[row] = sumf;
    }
}
