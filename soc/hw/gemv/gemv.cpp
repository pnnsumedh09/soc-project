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

#if SOC_GEMV_HW_ROW_INTERLEAVE == 8
// Sixteen blocks make the common Qwen 896-element input (28 blocks) use two
// tiles, so ping-pong buffering can overlap work for QKV and gate/up as well as
// wider down projections. The two banks together retain the old 32-block
// single-tile storage capacity.
enum { R8_ROWS = 8, R8_BLOCKS = 16, R8_BEATS = 2 * R8_BLOCKS };

static uint32_t load_r8_tile(const soc_gemv_word * w_quants, const uint16_t * w_scales,
                             uint32_t nb, uint32_t row_base, uint32_t active_rows,
                             uint32_t block_base, uint32_t blocks,
                             soc_gemv_word quant_tile[R8_ROWS][R8_BEATS],
                             float scale_tile[R8_ROWS][R8_BLOCKS]) {
    uint16_t scale_bits[R8_BLOCKS];
load_r8_rows:
    for (uint32_t lane = 0; lane < active_rows; ++lane) {
        uint32_t base = (row_base + lane) * nb + block_base;
load_r8_scales:
        for (uint32_t b = 0; b < blocks; ++b) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS PIPELINE II=1
#endif
            scale_bits[b] = w_scales[base + b];
        }
convert_r8_scales:
        for (uint32_t b = 0; b < blocks; ++b) {
            if (!finite_half(scale_bits[b])) return SOC_GEMV_HW_BAD_W_SCALE;
            scale_tile[lane][b] = dot_half_to_float(scale_bits[b]);
        }
load_r8_words:
        for (uint32_t beat = 0; beat < 2 * blocks; ++beat) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS PIPELINE II=1
#endif
            quant_tile[lane][beat] = w_quants[2 * base + beat];
        }
    }
    return SOC_GEMV_HW_OK;
}

static void compute_r8_tile(const soc_gemv_word quant_tile[R8_ROWS][R8_BEATS],
                            const float scale_tile[R8_ROWS][R8_BLOCKS],
                            const soc_gemv_word x_words[2 * SOC_GEMV_HW_MAX_NB],
                            const float x_scales[SOC_GEMV_HW_MAX_NB],
                            uint32_t active_rows, uint32_t block_base, uint32_t blocks,
                            float accum[R8_ROWS], int32_t first_half[R8_ROWS]) {
interleave_words:
    for (uint32_t slot = 0; slot < 2 * blocks * R8_ROWS; ++slot) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS PIPELINE II=1
#endif
        uint32_t lane = slot % R8_ROWS;
        uint32_t beat = slot / R8_ROWS;
        float next = accum[0];
        if (lane < active_rows) {
            int32_t part = dot_beat(quant_tile[lane][beat], x_words[2 * block_base + beat]);
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
        for (uint32_t i = 0; i < R8_ROWS - 1; ++i) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS UNROLL
#endif
            accum[i] = accum[i + 1];
        }
        accum[R8_ROWS - 1] = next;
    }
}

#if SOC_GEMV_HW_DOUBLE_BUFFER
static void load_compute_r8(const soc_gemv_word * w_quants, const uint16_t * w_scales,
                            uint32_t nb, uint32_t row_base, uint32_t active_rows,
                            uint32_t next_base, uint32_t next_blocks,
                            soc_gemv_word next_quants[R8_ROWS][R8_BEATS],
                            float next_scales[R8_ROWS][R8_BLOCKS],
                            const soc_gemv_word current_quants[R8_ROWS][R8_BEATS],
                            const float current_scales[R8_ROWS][R8_BLOCKS],
                            const soc_gemv_word x_words[2 * SOC_GEMV_HW_MAX_NB],
                            const float x_scales[SOC_GEMV_HW_MAX_NB],
                            uint32_t block_base, uint32_t blocks,
                            float accum[R8_ROWS], int32_t first_half[R8_ROWS],
                            uint32_t & next_status) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS DATAFLOW
#endif
    next_status = load_r8_tile(w_quants, w_scales, nb, row_base, active_rows,
                               next_base, next_blocks, next_quants, next_scales);
    compute_r8_tile(current_quants, current_scales, x_words, x_scales,
                    active_rows, block_base, blocks, accum, first_half);
}
#endif
#endif

void soc_gemv_hw(const soc_gemv_word * w_quants, const uint16_t * w_scales,
                 const uint8_t * x_native, float * y,
                 uint32_t rows, uint32_t nb, uint32_t & status,
                 uint64_t & read_bytes, uint64_t & write_bytes,
                 uint64_t & macs) {
#ifndef SOC_GEMV_HW_HOST
#pragma HLS INTERFACE s_axilite port=return bundle=control
#pragma HLS INTERFACE s_axilite port=w_quants bundle=control offset=0x10
#pragma HLS INTERFACE s_axilite port=w_scales bundle=control offset=0x20
#pragma HLS INTERFACE s_axilite port=x_native bundle=control offset=0x30
#pragma HLS INTERFACE s_axilite port=y bundle=control offset=0x40
#pragma HLS INTERFACE s_axilite port=rows bundle=control offset=0x50
#pragma HLS INTERFACE s_axilite port=nb bundle=control offset=0x58
#pragma HLS INTERFACE s_axilite port=status bundle=control offset=0x60
#pragma HLS INTERFACE s_axilite port=read_bytes bundle=control offset=0x70
#pragma HLS INTERFACE s_axilite port=write_bytes bundle=control offset=0x80
#pragma HLS INTERFACE s_axilite port=macs bundle=control offset=0x90
#endif
    status = SOC_GEMV_HW_OK;
    read_bytes = 0;
    write_bytes = 0;
    macs = 0;
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
    soc_gemv_word quant_tiles[2][R8_ROWS][R8_BEATS];
    float scale_tiles[2][R8_ROWS][R8_BLOCKS];
    float accum[R8_ROWS];
    int32_t first_half[R8_ROWS];
#ifndef SOC_GEMV_HW_HOST
#pragma HLS ARRAY_PARTITION variable=quant_tiles complete dim=1
#pragma HLS ARRAY_PARTITION variable=scale_tiles complete dim=1
#pragma HLS BIND_STORAGE variable=quant_tiles type=ram_1p impl=bram
#pragma HLS ARRAY_PARTITION variable=accum complete
#pragma HLS ARRAY_PARTITION variable=first_half complete
#endif
row_groups:
    for (uint32_t row_base = 0; row_base < rows; row_base += R8_ROWS) {
        uint32_t active_rows = rows - row_base;
        if (active_rows > R8_ROWS) active_rows = R8_ROWS;
        for (uint32_t lane = 0; lane < R8_ROWS; ++lane) {
            accum[lane] = 0.0f;
            first_half[lane] = 0;
        }
        uint32_t first_blocks = nb > (uint32_t) R8_BLOCKS ? (uint32_t) R8_BLOCKS : nb;
        status = load_r8_tile(w_quants, w_scales, nb, row_base, active_rows, 0,
                              first_blocks, quant_tiles[0], scale_tiles[0]);
        if (status != SOC_GEMV_HW_OK) return;
double_buffered_tiles:
        for (uint32_t block_base = 0, tile = 0; block_base < nb;
             block_base += R8_BLOCKS, ++tile) {
            uint32_t blocks = nb - block_base;
            if (blocks > R8_BLOCKS) blocks = R8_BLOCKS;
            uint32_t current_bank = tile & 1;
            uint32_t next_base = block_base + R8_BLOCKS;
            if (next_base < nb) {
                uint32_t next_blocks = nb - next_base;
                if (next_blocks > R8_BLOCKS) next_blocks = R8_BLOCKS;
                uint32_t next_status = SOC_GEMV_HW_OK;
#if SOC_GEMV_HW_DOUBLE_BUFFER
                if (current_bank == 0) {
                    load_compute_r8(w_quants, w_scales, nb, row_base, active_rows,
                                    next_base, next_blocks, quant_tiles[1], scale_tiles[1],
                                    quant_tiles[0], scale_tiles[0], x_words, x_scales,
                                    block_base, blocks, accum, first_half, next_status);
                } else {
                    load_compute_r8(w_quants, w_scales, nb, row_base, active_rows,
                                    next_base, next_blocks, quant_tiles[0], scale_tiles[0],
                                    quant_tiles[1], scale_tiles[1], x_words, x_scales,
                                    block_base, blocks, accum, first_half, next_status);
                }
#else
                compute_r8_tile(quant_tiles[current_bank], scale_tiles[current_bank],
                                x_words, x_scales, active_rows, block_base, blocks,
                                accum, first_half);
                next_status = load_r8_tile(w_quants, w_scales, nb, row_base, active_rows,
                                           next_base, next_blocks,
                                           quant_tiles[current_bank ^ 1], scale_tiles[current_bank ^ 1]);
#endif
                if (next_status != SOC_GEMV_HW_OK) {
                    status = next_status;
                    return;
                }
            } else {
                compute_r8_tile(quant_tiles[current_bank], scale_tiles[current_bank],
                                x_words, x_scales, active_rows, block_base, blocks,
                                accum, first_half);
            }
        }
write_rows:
        for (uint32_t lane = 0; lane < active_rows; ++lane) {
            y[row_base + lane] = accum[lane];
        }
    }
#endif

    read_bytes = (uint64_t) rows * nb * 34 + (uint64_t) nb * 34;
    write_bytes = (uint64_t) rows * sizeof(float);
    macs = (uint64_t) rows * nb * 32;
}
