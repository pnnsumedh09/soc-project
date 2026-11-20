#include "gemv.h"
#include "dot_q8_0_core.h"

static_assert(SOC_GEMV_HW_MAX_NB == DOT_MAX_NB, "Activation storage must match the supported dot length");

static bool finite_half(uint16_t h) {
    return (h & 0x7c00u) != 0x7c00u;
}

static int32_t dot_beat(soc_gemv_word w, soc_gemv_word x) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS INLINE
#endif
    int32_t products[16];
    int32_t sums8[8], sums4[4], sums2[2];
#ifndef SOC_GEMV_HW_HOST
#pragma HLS ARRAY_PARTITION variable=products complete
#pragma HLS ARRAY_PARTITION variable=sums8 complete
#pragma HLS ARRAY_PARTITION variable=sums4 complete
#pragma HLS ARRAY_PARTITION variable=sums2 complete
#endif
    for (int j = 0; j < 16; ++j) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS UNROLL
#endif
        uint8_t wb = (uint8_t) (w >> (8 * j));
        uint8_t xb = (uint8_t) (x >> (8 * j));
        int32_t wi = wb < 128 ? (int32_t) wb : (int32_t) wb - 256;
        int32_t xi = xb < 128 ? (int32_t) xb : (int32_t) xb - 256;
        products[j] = wi * xi;
    }
    for (int j = 0; j < 8; ++j) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS UNROLL
#endif
        sums8[j] = products[2 * j] + products[2 * j + 1];
    }
    for (int j = 0; j < 4; ++j) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS UNROLL
#endif
        sums4[j] = sums8[2 * j] + sums8[2 * j + 1];
    }
    for (int j = 0; j < 2; ++j) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS UNROLL
#endif
        sums2[j] = sums4[2 * j] + sums4[2 * j + 1];
    }
    return sums2[0] + sums2[1];
}

void soc_gemv_hw(const soc_gemv_word * w_quants, const uint16_t * w_scales,
                 const uint8_t * x_native, float * y,
                 uint32_t rows, uint32_t nb, uint32_t & status) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS INTERFACE s_axilite port=return bundle=control
#pragma HLS INTERFACE s_axilite port=w_quants bundle=control offset=0x10
#pragma HLS INTERFACE s_axilite port=w_scales bundle=control offset=0x20
#pragma HLS INTERFACE s_axilite port=x_native bundle=control offset=0x30
#pragma HLS INTERFACE s_axilite port=y bundle=control offset=0x40
#pragma HLS INTERFACE s_axilite port=rows bundle=control offset=0x50
#pragma HLS INTERFACE s_axilite port=nb bundle=control offset=0x58
#pragma HLS INTERFACE s_axilite port=status bundle=control offset=0x60
#endif
    status = SOC_GEMV_HW_OK;
    if (rows > SOC_GEMV_HW_MAX_ROWS || nb > SOC_GEMV_HW_MAX_NB || (rows != 0 && nb == 0)) {
        status = SOC_GEMV_HW_BAD_SHAPE;
        return;
    }
    if (rows == 0) {
        return;
    }

    soc_gemv_word x_words[2 * SOC_GEMV_HW_MAX_NB];
    float x_scales[SOC_GEMV_HW_MAX_NB];
#ifndef SOC_GEMV_HW_HOST
#pragma HLS BIND_STORAGE variable=x_words type=ram_1p impl=bram
#endif
load_x:
    for (uint32_t b = 0; b < nb; ++b) {
        uint32_t base = b * 34;
        uint16_t scale = (uint16_t) x_native[base] | ((uint16_t) x_native[base + 1] << 8);
        if (!finite_half(scale)) {
            status = SOC_GEMV_HW_BAD_X_SCALE;
            return;
        }
        x_scales[b] = dot_half_to_float(scale);
        for (uint32_t half = 0; half < 2; ++half) {
            soc_gemv_word word = 0;
            for (uint32_t j = 0; j < 16; ++j) {
                word |= soc_gemv_word(x_native[base + 2 + half * 16 + j]) << (8 * j);
            }
            x_words[2 * b + half] = word;
        }
    }

#if SOC_GEMV_HW_ROW_INTERLEAVE == 1
    uint16_t row_scale_bits[SOC_GEMV_HW_MAX_NB];
    float row_scales[SOC_GEMV_HW_MAX_NB];
rows_loop:
    for (uint32_t row = 0; row < rows; ++row) {
        uint32_t block_base = row * nb;
read_scales:
        for (uint32_t b = 0; b < nb; ++b) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS PIPELINE II=1
#endif
            row_scale_bits[b] = w_scales[block_base + b];
        }
convert_scales:
        for (uint32_t b = 0; b < nb; ++b) {
            if (!finite_half(row_scale_bits[b])) {
                status = SOC_GEMV_HW_BAD_W_SCALE;
                return;
            }
            row_scales[b] = dot_half_to_float(row_scale_bits[b]);
        }

        float sumf = 0.0f;
        int32_t first_half = 0;
        uint32_t word_base = block_base * 2;
#ifndef SOC_GEMV_HW_HOST
#pragma HLS BIND_OP variable=sumf op=fadd
#endif
read_words:
        for (uint32_t beat = 0; beat < nb * 2; ++beat) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS PIPELINE II=1
#endif
            int32_t part = dot_beat(w_quants[word_base + beat], x_words[beat]);
            if ((beat & 1) == 0) {
                first_half = part;
            } else {
                int32_t sumi = first_half + part;
                uint32_t b = beat / 2;
                float scale_product = row_scales[b] * x_scales[b];
                float term = (float) sumi * scale_product;
#ifndef SOC_GEMV_HW_HOST
#pragma HLS BIND_OP variable=scale_product op=fmul
#pragma HLS BIND_OP variable=term op=fmul
#endif
                // Preserve the golden block order; no partial FP sums or FMA.
                sumf = sumf + term;
            }
        }
        y[row] = sumf;
    }
#else
    // Eight independent rows share one 16-product datapath. Buffer contiguous
    // row segments first, then consume them in round-robin order locally.
    const uint32_t ROWS = SOC_GEMV_HW_ROW_INTERLEAVE;
    const uint32_t BLOCKS = 32; // 64 quant beats per row, matching the default burst.
    soc_gemv_word quant_tile[ROWS][2 * BLOCKS];
    uint16_t scale_bits[ROWS][BLOCKS];
    float scale_tile[ROWS][BLOCKS];
    float accum[ROWS];
    int32_t first_half[ROWS];
#ifndef SOC_GEMV_HW_HOST
#pragma HLS BIND_STORAGE variable=quant_tile type=ram_1p impl=bram
#pragma HLS ARRAY_PARTITION variable=accum complete
#pragma HLS ARRAY_PARTITION variable=first_half complete
#endif
row_groups:
    for (uint32_t row_base = 0; row_base < rows; row_base += ROWS) {
        uint32_t active_rows = rows - row_base;
        if (active_rows > ROWS) active_rows = ROWS;
        for (uint32_t lane = 0; lane < ROWS; ++lane) {
            accum[lane] = 0.0f;
            first_half[lane] = 0;
        }
block_tiles:
        for (uint32_t block_base = 0; block_base < nb; block_base += BLOCKS) {
            uint32_t blocks = nb - block_base;
            if (blocks > BLOCKS) blocks = BLOCKS;
load_tile_rows:
            for (uint32_t lane = 0; lane < active_rows; ++lane) {
                uint32_t base = (row_base + lane) * nb + block_base;
load_tile_scales:
                for (uint32_t b = 0; b < blocks; ++b) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS PIPELINE II=1
#endif
                    scale_bits[lane][b] = w_scales[base + b];
                }
convert_tile_scales:
                for (uint32_t b = 0; b < blocks; ++b) {
                    uint16_t bits = scale_bits[lane][b];
                    if (!finite_half(bits)) {
                        status = SOC_GEMV_HW_BAD_W_SCALE;
                        return;
                    }
                    scale_tile[lane][b] = dot_half_to_float(bits);
                }
load_tile_words:
                for (uint32_t beat = 0; beat < 2 * blocks; ++beat) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS PIPELINE II=1
#endif
                    quant_tile[lane][beat] = w_quants[2 * base + beat];
                }
            }
interleave_words:
            for (uint32_t slot = 0; slot < 2 * blocks * ROWS; ++slot) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS PIPELINE II=1
#endif
                uint32_t lane = slot % ROWS;
                uint32_t beat = slot / ROWS;
                // Rotate on every slot, including inactive lanes in a tail group.
                // After eight rotations each row returns to its original slot.
                // The FP feedback travels through eight distinct registers; no
                // false-dependence directive hides the real accumulator feedback.
                float next = accum[0];
                if (lane < active_rows) {
                    int32_t part = dot_beat(quant_tile[lane][beat],
                                           x_words[2 * block_base + beat]);
                    if ((beat & 1) == 0) {
                        first_half[lane] = part;
                    } else {
                        int32_t sumi = first_half[lane] + part;
                        uint32_t b = beat / 2;
                        float scale_product = scale_tile[lane][b] * x_scales[block_base + b];
                        float term = (float) sumi * scale_product;
#ifndef SOC_GEMV_HW_HOST
#pragma HLS BIND_OP variable=scale_product op=fmul
#pragma HLS BIND_OP variable=term op=fmul
#pragma HLS BIND_OP variable=next op=fadd
#endif
                        next = next + term;
                    }
                }
                for (uint32_t i = 0; i < ROWS - 1; ++i) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS UNROLL
#endif
                    accum[i] = accum[i + 1];
                }
                accum[ROWS - 1] = next;
            }
        }
write_rows:
        for (uint32_t lane = 0; lane < active_rows; ++lane) {
            y[row_base + lane] = accum[lane];
        }
    }
#endif

}
