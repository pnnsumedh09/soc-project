#include "soc_gemv.h"
#include "soc_repack.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

static void store_u16(uint8_t * destination, uint16_t value) {
    destination[0] = (uint8_t) value;
    destination[1] = (uint8_t) (value >> 8);
}

static void reference_dot(int          n,
                          float *      output,
                          size_t       output_stride,
                          const void * weights,
                          size_t       weight_stride,
                          const void * input,
                          size_t       input_stride,
                          int          rows) {
    (void) output_stride;
    (void) weight_stride;
    (void) input_stride;
    (void) rows;
    *output = soc_q4_0_row_dot_native(weights, input, (uint32_t) n / SOC_QK4_0);
}

int main(void) {
    enum { ROWS = 4, NB = 5 };

    uint8_t weights[ROWS * NB * SOC_Q4_0_BLOCK_BYTES];
    uint8_t activation[NB * SOC_Q8_0_BLOCK_BYTES];
    for (unsigned block = 0; block < ROWS * NB; ++block) {
        uint8_t * weight = weights + block * SOC_Q4_0_BLOCK_BYTES;
        store_u16(weight, (uint16_t) (0x2800 + (block & 3) * 0x0400));
        for (unsigned i = 0; i < SOC_Q4_0_QUANT_BYTES; ++i) {
            weight[SOC_Q4_0_SCALE_BYTES + i] = (uint8_t) (17 * block + 23 * i + 5);
        }
    }
    for (unsigned block = 0; block < NB; ++block) {
        uint8_t * input = activation + block * SOC_Q8_0_BLOCK_BYTES;
        store_u16(input, (uint16_t) (0x3000 + (block & 3) * 0x0400));
        int8_t * quants = (int8_t *) (input + SOC_Q4_0_SCALE_BYTES);
        for (unsigned i = 0; i < SOC_QK4_0; ++i) {
            quants[i] = (int8_t) ((37 * i + 19 * block) % 255 - 127);
        }
    }

    float actual[ROWS];
    float expected[ROWS];
    for (unsigned row = 0; row < ROWS; ++row) {
        expected[row] = soc_q4_0_row_dot_native(weights + row * NB * SOC_Q4_0_BLOCK_BYTES, activation, NB);
    }
    for (unsigned pass = 0; pass < 2; ++pass) {
        soc_gemv_q4_0(weights, NB * SOC_Q4_0_BLOCK_BYTES, ROWS, NB * SOC_QK4_0, activation, actual, reference_dot);
        for (unsigned row = 0; row < ROWS; ++row) {
            if (actual[row] != expected[row]) {
                fprintf(stderr, "Q4_0 dispatch mismatch at row %u: actual=%a expected=%a\n", row, actual[row],
                        expected[row]);
                return 1;
            }
        }
    }

    enum { LARGE_NB = SOC_Q4_0_MAX_NB + 1 };

    uint8_t large_weights[LARGE_NB * SOC_Q4_0_BLOCK_BYTES];
    uint8_t large_activation[LARGE_NB * SOC_Q8_0_BLOCK_BYTES];
    for (unsigned block = 0; block < LARGE_NB; ++block) {
        uint8_t * weight = large_weights + block * SOC_Q4_0_BLOCK_BYTES;
        uint8_t * input  = large_activation + block * SOC_Q8_0_BLOCK_BYTES;
        store_u16(weight, 0x3000);
        store_u16(input, 0x3000);
        for (unsigned i = 0; i < SOC_Q4_0_QUANT_BYTES; ++i) {
            weight[SOC_Q4_0_SCALE_BYTES + i] = (uint8_t) (13 * block + i);
        }
        for (unsigned i = 0; i < SOC_QK4_0; ++i) {
            input[SOC_Q4_0_SCALE_BYTES + i] = (uint8_t) (7 * block + i);
        }
    }
    float large_expected = soc_q4_0_row_dot_native(large_weights, large_activation, LARGE_NB);
    float large_actual   = 0.0f;
    soc_gemv_q4_0(large_weights, sizeof(large_weights), 1, LARGE_NB * SOC_QK4_0, large_activation, &large_actual,
                  reference_dot);
    if (large_actual != large_expected) {
        fprintf(stderr, "Q4_0 unsupported-shape fallback mismatch\n");
        return 1;
    }

    soc_gemv_stats stats;
    soc_gemv_get_stats(&stats);
    if (stats.calls != 2 || stats.tensors != 1 || stats.verified != 2 || stats.fallback_calls != 1) {
        fprintf(stderr, "Q4_0 dispatch stats mismatch\n");
        return 1;
    }
    puts("Q4_0 dispatcher and cache: PASS");
    return 0;
}
