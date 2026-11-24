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

static void load_quant_rows(const soc_gemm_word *w_quants,
                            hls::stream<soc_gemm_word> &q0_fifo,
#if SOC_GEMM_WEIGHT_BITS == 8
                            hls::stream<soc_gemm_word> &q1_fifo,
#endif
                            uint32_t m,
                            uint32_t nb,
                            uint32_t n) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS INLINE off
#endif
    uint32_t blocks = m * nb;
load_quant_tiles:
    for (uint32_t col_base = 0; col_base < n;
         col_base += SOC_GEMM_HW_N_TILE) {
#if SOC_GEMM_WEIGHT_BITS == 8
load_quant_beats:
        for (uint32_t beat = 0; beat < 2 * blocks; ++beat) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS PIPELINE II=1
#endif
            soc_gemm_word value = w_quants[beat];
            if ((beat & 1u) == 0) {
                q0_fifo.write(value);
            } else {
                q1_fifo.write(value);
            }
        }
#else
load_quant_blocks:
        for (uint32_t block = 0; block < blocks; ++block) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS PIPELINE II=1
#endif
            q0_fifo.write(w_quants[block]);
        }
#endif
    }
}

static void load_scale_rows(const uint16_t *w_scales,
                            hls::stream<uint16_t> &scale_fifo,
                            uint32_t m,
                            uint32_t nb,
                            uint32_t n) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS INLINE off
#endif
    uint32_t blocks = m * nb;
load_scale_tiles:
    for (uint32_t col_base = 0; col_base < n;
         col_base += SOC_GEMM_HW_N_TILE) {
load_scale_blocks:
        for (uint32_t block = 0; block < blocks; ++block) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS PIPELINE II=1
#endif
            scale_fifo.write(w_scales[block]);
        }
    }
}

static void compute_all_tiles(
                 hls::stream<soc_gemm_word> &q0_fifo,
#if SOC_GEMM_WEIGHT_BITS == 8
                 hls::stream<soc_gemm_word> &q1_fifo,
#endif
                 hls::stream<uint16_t> &scale_fifo,
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
    bool bad_x_scale = false;
    bool bad_w_scale = false;

compute_column_tiles:
    for (uint32_t col_base = 0; col_base < n;
         col_base += SOC_GEMM_HW_N_TILE) {
        uint32_t active_columns = n - col_base;
        if (active_columns > SOC_GEMM_HW_N_TILE) {
            active_columns = SOC_GEMM_HW_N_TILE;
        }

    compute_load_tile_columns:
        for (uint32_t lane = 0; lane < SOC_GEMM_HW_N_TILE; ++lane) {
            if (lane < active_columns) {
                uint32_t x_column_base = (col_base + lane) * nb * 34u;
            compute_load_tile_blocks:
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
                        // Keep this loop serial. Vitis HLS 2022.2 crashes when
                        // unrolling the loop-carried 128-bit OR reduction.
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

compute_rows:
        for (uint32_t row = 0; row < m; ++row) {
            float accum[SOC_GEMM_HW_N_TILE];
#ifndef SOC_GEMM_HW_HOST
#pragma HLS ARRAY_PARTITION variable=accum complete
#endif
        init_compute_accumulators:
            for (uint32_t lane = 0; lane < SOC_GEMM_HW_N_TILE; ++lane) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS UNROLL
#endif
                accum[lane] = 0.0f;
            }

        compute_blocks:
            for (uint32_t b = 0; b < nb; ++b) {
                soc_gemm_word w0 = q0_fifo.read();
#if SOC_GEMM_WEIGHT_BITS == 8
                soc_gemm_word w1 = q1_fifo.read();
#endif
                uint16_t w_scale_bits = scale_fifo.read();
                bool scale_ok = finite_half(w_scale_bits);
                if (!scale_ok) {
                    bad_w_scale = true;
                }
                float w_scale =
                    scale_ok ? dot_half_to_float(w_scale_bits) : 0.0f;

            compute_lanes:
                for (uint32_t lane = 0; lane < SOC_GEMM_HW_N_TILE; ++lane) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS PIPELINE II=1
#endif
                    // Eight rotating contexts hide FP accumulator latency.
                    float next = accum[0];
                    if (lane < active_columns && scale_ok) {
#if SOC_GEMM_WEIGHT_BITS == 8
                        int32_t sumi =
                            dot_beat(w0, x_words[lane][2 * b]) +
                            dot_beat(w1, x_words[lane][2 * b + 1]);
#else
                        int32_t sumi =
                            dot_q4_0_block(w0,
                                           x_words[lane][2 * b],
                                           x_words[lane][2 * b + 1]);
#endif
                        float scale_product = w_scale * x_scales[lane][b];
                        float term = (float)sumi * scale_product;
                        next = next + term;
                    }
                    for (uint32_t i = 0;
                         i < SOC_GEMM_HW_N_TILE - 1; ++i) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS UNROLL
#endif
                        accum[i] = accum[i + 1];
                    }
                    accum[SOC_GEMM_HW_N_TILE - 1] = next;
                }
            }

        write_compute_tile:
            for (uint32_t lane = 0; lane < SOC_GEMM_HW_N_TILE; ++lane) {
#ifndef SOC_GEMM_HW_HOST
#pragma HLS PIPELINE II=1
#endif
                if (lane < active_columns &&
                    !bad_x_scale && !bad_w_scale) {
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

static void run_weight_dataflow(
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
    // At the maximum row width, 2432 entries hold sixteen complete rows.
    hls::stream<soc_gemm_word> q0_fifo;
#if SOC_GEMM_WEIGHT_BITS == 8
    hls::stream<soc_gemm_word> q1_fifo;
#endif
    hls::stream<uint16_t> scale_fifo;
#ifndef SOC_GEMM_HW_HOST
#pragma HLS STREAM variable=q0_fifo depth=2432
#if SOC_GEMM_WEIGHT_BITS == 8
#pragma HLS STREAM variable=q1_fifo depth=2432
#endif
#pragma HLS STREAM variable=scale_fifo depth=2432
#endif

    load_quant_rows(w_quants, q0_fifo,
#if SOC_GEMM_WEIGHT_BITS == 8
                    q1_fifo,
#endif
                    m, nb, n);
    load_scale_rows(w_scales, scale_fifo, m, nb, n);
    compute_all_tiles(q0_fifo,
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

    // Start the dataflow network once. Each process loops over every
    // eight-column tile internally, so the generated hardware does not need
    // to restart a dataflow submodule between tiles.
    run_weight_dataflow(w_quants, w_scales, x_native, y,
                        m, nb, n, status);
}
