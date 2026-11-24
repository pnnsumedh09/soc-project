#include "gemv.h"
#include "soc_repack.h"

#include <cstdio>
#include <cstring>

enum { ROWS = 3, NB = 5 };

static void store_u16(uint8_t * destination, uint16_t value) {
    destination[0] = (uint8_t) value;
    destination[1] = (uint8_t) (value >> 8);
}

static void make_weights(uint8_t * weights) {
    for (unsigned block = 0; block < ROWS * NB; ++block) {
        uint8_t * weight = weights + block * SOC_Q4_0_BLOCK_BYTES;
        store_u16(weight, (uint16_t) (0x2800 + (block & 3) * 0x0400));
        for (unsigned i = 0; i < SOC_Q4_0_QUANT_BYTES; ++i) {
            weight[SOC_Q4_0_SCALE_BYTES + i] = (uint8_t) (29 * block + 11 * i + 3);
        }
    }
}

static void make_activation(uint8_t * activation) {
    for (unsigned block = 0; block < NB; ++block) {
        uint8_t * input = activation + block * SOC_Q8_0_BLOCK_BYTES;
        store_u16(input, (uint16_t) (0x3000 + (block & 3) * 0x0400));
        int8_t * quants = (int8_t *) (input + SOC_Q4_0_SCALE_BYTES);
        for (unsigned i = 0; i < SOC_QK4_0; ++i) {
            quants[i] = (int8_t) ((37 * i + 19 * block) % 255 - 127);
        }
    }
}

static void native_reference(const uint8_t * weights, const uint8_t * activation, float * output) {
    for (unsigned row = 0; row < ROWS; ++row) {
        float sum = 0.0f;
        for (unsigned block = 0; block < NB; ++block) {
            const uint8_t * weight = weights + (row * NB + block) * SOC_Q4_0_BLOCK_BYTES;
            const uint8_t * input  = activation + block * SOC_Q8_0_BLOCK_BYTES;
            const int8_t *  x      = (const int8_t *) (input + SOC_Q4_0_SCALE_BYTES);
            int32_t         low    = 0;
            int32_t         high   = 0;
            for (unsigned i = 0; i < SOC_Q4_0_QUANT_BYTES; ++i) {
                uint8_t packed = weight[SOC_Q4_0_SCALE_BYTES + i];
                low += ((int32_t) (packed & 0x0f) - 8) * x[i];
                high += ((int32_t) (packed >> 4) - 8) * x[i + SOC_Q4_0_QUANT_BYTES];
            }
            uint16_t weight_scale = (uint16_t) weight[0] | (uint16_t) weight[1] << 8;
            uint16_t input_scale  = (uint16_t) input[0] | (uint16_t) input[1] << 8;
            sum += (float) (low + high) * soc_fp16_to_fp32(weight_scale) * soc_fp16_to_fp32(input_scale);
        }
        output[row] = sum;
    }
}

int main() {
    alignas(16) uint8_t  weights[ROWS * NB * SOC_Q4_0_BLOCK_BYTES];
    alignas(16) uint8_t  quants[ROWS * NB * SOC_Q4_0_QUANT_BYTES];
    alignas(16) uint16_t scales[ROWS * NB];
    alignas(16) uint8_t  activation[NB * SOC_Q8_0_BLOCK_BYTES];
    uint8_t              unpacked[sizeof(weights)];
    float                actual[ROWS]   = { 0 };
    float                repacked[ROWS] = { 0 };
    float                expected[ROWS] = { 0 };
    make_weights(weights);
    make_activation(activation);

    if (soc_q4r_pack(weights, NB * SOC_Q4_0_BLOCK_BYTES, ROWS, NB, quants, scales) ||
        soc_q4r_unpack(quants, scales, ROWS, NB, unpacked, NB * SOC_Q4_0_BLOCK_BYTES) ||
        std::memcmp(weights, unpacked, sizeof(weights))) {
        std::fprintf(stderr, "Q4_0 repack round trip failed\n");
        return 1;
    }

    soc_q4r_mat matrix = { ROWS, NB, quants, scales, sizeof(quants), sizeof(scales), NULL };
    native_reference(weights, activation, expected);
    soc_q4r_gemv_ref(&matrix, activation, repacked);
    uint32_t status = 99;
    soc_gemv_hw((const soc_gemv_word *) quants, scales, activation, actual, ROWS, NB, status);
    if (status != SOC_GEMV_HW_OK || std::memcmp(repacked, expected, sizeof(expected)) ||
        std::memcmp(actual, expected, sizeof(actual))) {
        std::fprintf(stderr, "Q4_0 GEMV mismatch: status=%u\n", status);
        for (unsigned row = 0; row < ROWS; ++row) {
            std::fprintf(stderr, "  row %u actual=%a repacked=%a expected=%a\n", row, actual[row], repacked[row],
                         expected[row]);
        }
        return 1;
    }

    soc_gemv_hw((const soc_gemv_word *) quants, scales, activation, actual, 1, SOC_GEMV_HW_MAX_NB + 1, status);
    if (status != SOC_GEMV_HW_BAD_SHAPE) {
        std::fprintf(stderr, "Q4_0 shape validation failed\n");
        return 1;
    }

    store_u16(activation, 0x7c00u);
    soc_gemv_hw((const soc_gemv_word *) quants, scales, activation, actual, ROWS, NB, status);
    if (status != SOC_GEMV_HW_BAD_X_SCALE) {
        std::fprintf(stderr, "Q8_0 scale validation failed\n");
        return 1;
    }

    std::puts("Q4_0 GEMV: PASS");
    return 0;
}
