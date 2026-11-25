#include "gemm.h"
#include "dot_q8_0_core.h"

#ifdef SOC_GEMM_HW_HOST
#include <queue>
namespace hls {
template <typename T>
class stream {
public:
    void write(const T &value) { values.push(value); }
    T read() {
        T value = values.front();
        values.pop();
        return value;
    }
private:
    std::queue<T> values;
};
}
#else
#include <hls_stream.h>
#endif

static bool finite_half(uint16_t h) {
    return (h & 0x7c00u) != 0x7c00u;
}

#if SOC_GEMM_WEIGHT_BITS == 8
static int32_t dot_beat(soc_gemm_word w, soc_gemm_word x) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS INLINE
#endif
    int32_t products[16];
    int32_t sums8[8];
    int32_t sums4[4];
    int32_t sums2[2];
#ifndef SOC_GEMM_HW_HOST
#pragma HLS ARRAY_PARTITION variable=products complete
#pragma HLS ARRAY_PARTITION variable=sums8 complete
#pragma HLS ARRAY_PARTITION variable=sums4 complete
#pragma HLS ARRAY_PARTITION variable=sums2 complete
#endif

dot_products:
    for (int j = 0; j < 16; ++j) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS UNROLL
#endif
        uint8_t wb = (uint8_t)(w >> (8 * j));
        uint8_t xb = (uint8_t)(x >> (8 * j));
        int32_t wi = wb < 128 ? (int32_t)wb : (int32_t)wb - 256;
        int32_t xi = xb < 128 ? (int32_t)xb : (int32_t)xb - 256;
        products[j] = wi * xi;
    }
reduce_16_to_8:
    for (int j = 0; j < 8; ++j) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS UNROLL
#endif
        sums8[j] = products[2 * j] + products[2 * j + 1];
    }
reduce_8_to_4:
    for (int j = 0; j < 4; ++j) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS UNROLL
#endif
        sums4[j] = sums8[2 * j] + sums8[2 * j + 1];
    }
reduce_4_to_2:
    for (int j = 0; j < 2; ++j) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS UNROLL
#endif
        sums2[j] = sums4[2 * j] + sums4[2 * j + 1];
    }
    return sums2[0] + sums2[1];
}
#endif

// GGML Q4_0 stores q[0..15] in the low nibbles and q[16..31] in
// the high nibbles. Each unsigned nibble is biased by 8.
#if SOC_GEMM_WEIGHT_BITS == 4
static int32_t dot_q4_0_block(soc_gemm_word w,
                              soc_gemm_word x0,
                              soc_gemm_word x1) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS INLINE
#endif
    int32_t products[16];
    int32_t sums8[8];
    int32_t sums4[4];
    int32_t sums2[2];
#ifndef SOC_GEMM_HW_HOST
#pragma HLS ARRAY_PARTITION variable=products complete
#pragma HLS ARRAY_PARTITION variable=sums8 complete
#pragma HLS ARRAY_PARTITION variable=sums4 complete
#pragma HLS ARRAY_PARTITION variable=sums2 complete
#endif

q4_products:
    for (int j = 0; j < 16; ++j) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS UNROLL
#endif
        uint8_t packed = (uint8_t)(w >> (8 * j));
        int32_t w0 = (int32_t)(packed & 0x0fu) - 8;
        int32_t w1 = (int32_t)(packed >> 4) - 8;
        uint8_t x0b = (uint8_t)(x0 >> (8 * j));
        uint8_t x1b = (uint8_t)(x1 >> (8 * j));
        int32_t xv0 = x0b < 128 ? (int32_t)x0b : (int32_t)x0b - 256;
        int32_t xv1 = x1b < 128 ? (int32_t)x1b : (int32_t)x1b - 256;
        products[j] = w0 * xv0 + w1 * xv1;
    }
q4_reduce_16_to_8:
    for (int j = 0; j < 8; ++j) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS UNROLL
#endif
        sums8[j] = products[2 * j] + products[2 * j + 1];
    }
q4_reduce_8_to_4:
    for (int j = 0; j < 4; ++j) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS UNROLL
#endif
        sums4[j] = sums8[2 * j] + sums8[2 * j + 1];
    }
q4_reduce_4_to_2:
    for (int j = 0; j < 2; ++j) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS UNROLL
#endif
        sums2[j] = sums4[2 * j] + sums4[2 * j + 1];
    }
    return sums2[0] + sums2[1];
}
#endif

static void load_shared_quants(
                 const soc_gemm_word *w_quants,
                 hls::stream<soc_gemm_word> q0_fifo[SOC_GEMM_HW_N_TILE],
#if SOC_GEMM_WEIGHT_BITS == 8
                 hls::stream<soc_gemm_word> q1_fifo[SOC_GEMM_HW_N_TILE],
#endif
                 uint32_t m,
                 uint32_t nb,
                 uint32_t n) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS INLINE off
#endif
    const bool row_interleaved = n == 1;
    uint32_t column_tiles =
        row_interleaved ? 1u : (n + SOC_GEMM_HW_N_TILE - 1) /
                                     SOC_GEMM_HW_N_TILE;

load_shared_quant_tiles:
    for (uint32_t tile = 0; tile < column_tiles; ++tile) {
        uint32_t row_lane = 0;
        uint32_t block_in_row = 0;
#if SOC_GEMM_WEIGHT_BITS == 8
        uint32_t beats = 2 * m * nb;
    load_shared_quant_beats:
        for (uint32_t beat = 0; beat < beats; ++beat) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS PIPELINE II=1
#endif
            uint32_t target = row_interleaved ? row_lane : 0u;
            soc_gemm_word value = w_quants[beat];
            if ((beat & 1u) == 0) {
                q0_fifo[target].write(value);
            } else {
                q1_fifo[target].write(value);
                ++block_in_row;
                if (block_in_row == nb) {
                    block_in_row = 0;
                    row_lane = (row_lane + 1) % SOC_GEMM_HW_N_TILE;
                }
            }
        }
#else
        uint32_t blocks = m * nb;
    load_shared_quant_blocks:
        for (uint32_t block = 0; block < blocks; ++block) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS PIPELINE II=1
#endif
            uint32_t target = row_interleaved ? row_lane : 0u;
            q0_fifo[target].write(w_quants[block]);
            ++block_in_row;
            if (block_in_row == nb) {
                block_in_row = 0;
                row_lane = (row_lane + 1) % SOC_GEMM_HW_N_TILE;
            }
        }
#endif
    }
}

static void load_shared_scales(
                 const uint16_t *w_scales,
                 hls::stream<uint16_t> scale_fifo[SOC_GEMM_HW_N_TILE],
                 uint32_t m,
                 uint32_t nb,
                 uint32_t n) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS INLINE off
#endif
    const bool row_interleaved = n == 1;
    uint32_t column_tiles =
        row_interleaved ? 1u : (n + SOC_GEMM_HW_N_TILE - 1) /
                                     SOC_GEMM_HW_N_TILE;

load_shared_scale_tiles:
    for (uint32_t tile = 0; tile < column_tiles; ++tile) {
        uint32_t row_lane = 0;
        uint32_t block_in_row = 0;
        uint32_t blocks = m * nb;
    load_shared_scale_blocks:
        for (uint32_t block = 0; block < blocks; ++block) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS PIPELINE II=1
#endif
            uint32_t target = row_interleaved ? row_lane : 0u;
            scale_fifo[target].write(w_scales[block]);
            ++block_in_row;
            if (block_in_row == nb) {
                block_in_row = 0;
                row_lane = (row_lane + 1) % SOC_GEMM_HW_N_TILE;
            }
        }
    }
}

static void load_shared_activation_tile(
                 const uint8_t *x_native,
                 soc_gemm_word x_words[SOC_GEMM_HW_N_TILE]
                                       [2 * SOC_GEMM_HW_MAX_NB],
                 float x_scales[SOC_GEMM_HW_N_TILE]
                               [SOC_GEMM_HW_MAX_NB],
                 uint32_t col_base,
                 uint32_t active_columns,
                 uint32_t nb,
                 bool &bad_x_scale) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS INLINE
#endif
load_shared_activation_columns:
    for (uint32_t lane = 0; lane < SOC_GEMM_HW_N_TILE; ++lane) {
        if (lane < active_columns) {
            uint32_t x_column_base = (col_base + lane) * nb * 34u;
        load_shared_activation_blocks:
            for (uint32_t b = 0; b < nb; ++b) {
                uint32_t base = x_column_base + b * 34u;
                uint16_t scale = (uint16_t)x_native[base] |
                                 ((uint16_t)x_native[base + 1] << 8);
                bool scale_ok = finite_half(scale);
                if (!scale_ok) {
                    bad_x_scale = true;
                }
                x_scales[lane][b] =
                    scale_ok ? dot_half_to_float(scale) : 0.0f;
                for (uint32_t half = 0; half < 2; ++half) {
                    soc_gemm_word word = 0;
                    // Vitis HLS 2022.2 crashes if this 128-bit OR reduction
                    // is unrolled. Keep the byte pack serial.
                    for (uint32_t j = 0; j < 16; ++j) {
                        word |= soc_gemm_word(
                                    x_native[base + 2 + half * 16 + j])
                                << (8 * j);
                    }
                    x_words[lane][2 * b + half] = word;
                }
            }
        }
    }
}

static void compute_shared_groups(
                 hls::stream<soc_gemm_word> q0_fifo[SOC_GEMM_HW_N_TILE],
#if SOC_GEMM_WEIGHT_BITS == 8
                 hls::stream<soc_gemm_word> q1_fifo[SOC_GEMM_HW_N_TILE],
#endif
                 hls::stream<uint16_t> scale_fifo[SOC_GEMM_HW_N_TILE],
                 const uint8_t *x_native,
                 float *y,
                 uint32_t m,
                 uint32_t nb,
                 uint32_t n,
                 uint32_t &status) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS INLINE off
#endif
    soc_gemm_word x_words[SOC_GEMM_HW_N_TILE][2 * SOC_GEMM_HW_MAX_NB];
    float x_scales[SOC_GEMM_HW_N_TILE][SOC_GEMM_HW_MAX_NB];
#ifndef SOC_GEMM_HW_HOST
#pragma HLS ARRAY_PARTITION variable=x_words complete dim=1
#pragma HLS ARRAY_PARTITION variable=x_scales complete dim=1
#endif

    const bool row_interleaved = n == 1;
    uint32_t column_tiles =
        row_interleaved ? 1u : (n + SOC_GEMM_HW_N_TILE - 1) /
                                     SOC_GEMM_HW_N_TILE;
    uint32_t groups = row_interleaved
                          ? (m + SOC_GEMM_HW_N_TILE - 1) /
                                SOC_GEMM_HW_N_TILE
                          : column_tiles * m;
    bool bad_x_scale = false;
    bool bad_w_scale = false;

shared_groups:
    for (uint32_t group = 0; group < groups; ++group) {
        uint32_t row = 0;
        uint32_t row_base = 0;
        uint32_t col_base = 0;
        uint32_t active_lanes = 0;
        bool load_activation = false;
        uint32_t activation_col_base = 0;
        uint32_t activation_columns = 0;

        if (row_interleaved) {
            row_base = group * SOC_GEMM_HW_N_TILE;
            active_lanes = m - row_base;
            if (active_lanes > SOC_GEMM_HW_N_TILE) {
                active_lanes = SOC_GEMM_HW_N_TILE;
            }
            if (group == 0) {
                load_activation = true;
                activation_columns = 1;
            }
        } else {
            uint32_t tile = group / m;
            row = group - tile * m;
            col_base = tile * SOC_GEMM_HW_N_TILE;
            active_lanes = n - col_base;
            if (active_lanes > SOC_GEMM_HW_N_TILE) {
                active_lanes = SOC_GEMM_HW_N_TILE;
            }
            if (row == 0) {
                load_activation = true;
                activation_col_base = col_base;
                activation_columns = active_lanes;
            }
        }
        if (load_activation) {
            load_shared_activation_tile(x_native, x_words, x_scales,
                                        activation_col_base,
                                        activation_columns, nb,
                                        bad_x_scale);
        }

        float accum[SOC_GEMM_HW_N_TILE];
#ifndef SOC_GEMM_HW_HOST
#pragma HLS ARRAY_PARTITION variable=accum complete
#endif
    init_shared_accumulators:
        for (uint32_t lane = 0; lane < SOC_GEMM_HW_N_TILE; ++lane) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS UNROLL
#endif
            accum[lane] = 0.0f;
        }

        soc_gemm_word held_w0 = 0;
#if SOC_GEMM_WEIGHT_BITS == 8
        soc_gemm_word held_w1 = 0;
#endif
        uint16_t held_scale_bits = 0;
        uint32_t total_slots = nb * SOC_GEMM_HW_N_TILE;

    shared_compute_slots:
        for (uint32_t slot = 0; slot < total_slots; ++slot) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS PIPELINE II=1
#endif
            uint32_t lane = slot % SOC_GEMM_HW_N_TILE;
            uint32_t b = slot / SOC_GEMM_HW_N_TILE;
            bool active = lane < active_lanes;

            soc_gemm_word selected_w0 = held_w0;
#if SOC_GEMM_WEIGHT_BITS == 8
            soc_gemm_word selected_w1 = held_w1;
#endif
            uint16_t selected_scale_bits = held_scale_bits;

            if (row_interleaved) {
                if (active) {
                    selected_w0 = q0_fifo[lane].read();
#if SOC_GEMM_WEIGHT_BITS == 8
                    selected_w1 = q1_fifo[lane].read();
#endif
                    selected_scale_bits = scale_fifo[lane].read();
                }
            } else {
                if (lane == 0) {
                    held_w0 = q0_fifo[0].read();
#if SOC_GEMM_WEIGHT_BITS == 8
                    held_w1 = q1_fifo[0].read();
#endif
                    held_scale_bits = scale_fifo[0].read();
                }
                selected_w0 = held_w0;
#if SOC_GEMM_WEIGHT_BITS == 8
                selected_w1 = held_w1;
#endif
                selected_scale_bits = held_scale_bits;
            }

            bool scale_ok = !active || finite_half(selected_scale_bits);
            if (active && !scale_ok) {
                bad_w_scale = true;
            }

            // This is the only dot-product call site in the runtime-selected
            // N=1/N>1 schedules. The lane interpretation changes, not the
            // physical multiplier/reduction and FP accumulation pipeline.
            float next = accum[0];
            if (active && scale_ok) {
                uint32_t x_lane = row_interleaved ? 0u : lane;
#if SOC_GEMM_WEIGHT_BITS == 8
                int32_t sumi =
                    dot_beat(selected_w0, x_words[x_lane][2 * b]) +
                    dot_beat(selected_w1, x_words[x_lane][2 * b + 1]);
#else
                int32_t sumi =
                    dot_q4_0_block(selected_w0,
                                   x_words[x_lane][2 * b],
                                   x_words[x_lane][2 * b + 1]);
#endif
                float w_scale = dot_half_to_float(selected_scale_bits);
                float scale_product = w_scale * x_scales[x_lane][b];
                float term = (float)sumi * scale_product;
                next = next + term;
            }

            for (uint32_t i = 0; i < SOC_GEMM_HW_N_TILE - 1; ++i) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS UNROLL
#endif
                accum[i] = accum[i + 1];
            }
            accum[SOC_GEMM_HW_N_TILE - 1] = next;
        }

    write_shared_outputs:
        for (uint32_t lane = 0; lane < SOC_GEMM_HW_N_TILE; ++lane) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS PIPELINE II=1
#endif
            if (lane < active_lanes && !bad_x_scale && !bad_w_scale) {
                if (row_interleaved) {
                    y[row_base + lane] = accum[lane];
                } else {
                    y[(col_base + lane) * m + row] = accum[lane];
                }
            }
        }
    }

    if (bad_x_scale) {
        status = SOC_GEMM_HW_BAD_X_SCALE;
    } else if (bad_w_scale) {
        status = SOC_GEMM_HW_BAD_W_SCALE;
    }
}

static void run_shared_dataflow(
                 const soc_gemm_word *w_quants,
                 const uint16_t *w_scales,
                 const uint8_t *x_native,
                 float *y,
                 uint32_t m,
                 uint32_t nb,
                 uint32_t n,
                 uint32_t &status) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS INLINE off
#pragma HLS DATAFLOW
#endif
    // One physical FIFO set feeds both schedules. Each lane can hold two
    // maximum-width rows. This preserves the known-good prefetch capacity
    // while eliminating the second DATAFLOW network; depth reduction is the
    // next isolated area experiment.
    hls::stream<soc_gemm_word> q0_fifo[SOC_GEMM_HW_N_TILE];
#if SOC_GEMM_WEIGHT_BITS == 8
    hls::stream<soc_gemm_word> q1_fifo[SOC_GEMM_HW_N_TILE];
#endif
    hls::stream<uint16_t> scale_fifo[SOC_GEMM_HW_N_TILE];
#ifndef SOC_GEMM_HW_HOST
#pragma HLS ARRAY_PARTITION variable=q0_fifo complete dim=1
#pragma HLS STREAM variable=q0_fifo depth=152
#if SOC_GEMM_WEIGHT_BITS == 8
#pragma HLS ARRAY_PARTITION variable=q1_fifo complete dim=1
#pragma HLS STREAM variable=q1_fifo depth=152
#endif
#pragma HLS ARRAY_PARTITION variable=scale_fifo complete dim=1
#pragma HLS STREAM variable=scale_fifo depth=152
#endif

    load_shared_quants(w_quants, q0_fifo,
#if SOC_GEMM_WEIGHT_BITS == 8
                       q1_fifo,
#endif
                       m, nb, n);
    load_shared_scales(w_scales, scale_fifo, m, nb, n);
    compute_shared_groups(q0_fifo,
#if SOC_GEMM_WEIGHT_BITS == 8
                          q1_fifo,
#endif
                          scale_fifo, x_native, y, m, nb, n, status);
}

void soc_gemm_hw(const soc_gemm_word *w_quants,
                 const uint16_t *w_scales,
                 const uint8_t *x_native,
                 float *y,
                 uint32_t m,
                 uint32_t nb,
                 uint32_t n,
                 uint32_t &status) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS INTERFACE s_axilite port=return bundle=control
#pragma HLS INTERFACE s_axilite port=w_quants bundle=control offset=0x10
#pragma HLS INTERFACE s_axilite port=w_scales bundle=control offset=0x20
#pragma HLS INTERFACE s_axilite port=x_native bundle=control offset=0x30
#pragma HLS INTERFACE s_axilite port=y bundle=control offset=0x40
#pragma HLS INTERFACE s_axilite port=m bundle=control offset=0x50
#pragma HLS INTERFACE s_axilite port=nb bundle=control offset=0x58
#pragma HLS INTERFACE s_axilite port=n bundle=control offset=0x60
#pragma HLS INTERFACE s_axilite port=status bundle=control offset=0x68
#endif

    status = SOC_GEMM_HW_OK;
    if (m > SOC_GEMM_HW_MAX_M || nb > SOC_GEMM_HW_MAX_NB ||
        n > SOC_GEMM_HW_MAX_N || ((m != 0 && n != 0) && nb == 0)) {
        status = SOC_GEMM_HW_BAD_SHAPE;
        return;
    }
    if (m == 0 || n == 0) {
        return;
    }

    // One DATAFLOW network and one flattened dot-product pipeline serve both
    // schedules. For N=1 the eight contexts are rows; for N>1 they are columns.
    run_shared_dataflow(w_quants, w_scales, x_native, y,
                        m, nb, n, status);
}
