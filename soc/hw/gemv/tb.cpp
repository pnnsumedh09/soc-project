#include "gemv.h"
#include "soc_repack.h"

#include <cstdio>
#include <cstring>

enum { ROWS = 3, NB = 2 };

static void store_u16(uint8_t * p, uint16_t value) {
    p[0] = (uint8_t) value;
    p[1] = (uint8_t) (value >> 8);
}

static void store_f32(uint8_t * p, float value) {
    std::memcpy(p, &value, sizeof(value));
}

static void store_u32(uint8_t * p, uint32_t value) {
    std::memcpy(p, &value, sizeof(value));
}

static void make_weights(uint8_t * weights) {
    for (unsigned block = 0; block < ROWS * NB; ++block) {
        uint8_t * w = weights + block * SOC_Q4_K_BLOCK_BYTES;
        store_u16(w, 0x3000 + (block & 3) * 0x0400);
        store_u16(w + 2, 0x2800 + (block & 1) * 0x0400);
        for (unsigned i = 0; i < 12; ++i) {
            w[4 + i] = (uint8_t) (17 * block + 23 * i + 5);
        }
        for (unsigned i = 0; i < SOC_Q4_K_QUANT_BYTES; ++i) {
            w[SOC_Q4_K_META_BYTES + i] = (uint8_t) (29 * block + 11 * i + 3);
        }
    }
}

static void make_activation(uint8_t * activation) {
    for (unsigned block = 0; block < NB; ++block) {
        uint8_t * x = activation + block * SOC_Q8_K_BLOCK_BYTES;
        store_f32(x, block ? -0.03125f : 0.0625f);
        int8_t * quants = (int8_t *) (x + 4);
        for (unsigned i = 0; i < SOC_QK_K; ++i) {
            quants[i] = (int8_t) ((37 * i + 19 * block) % 255 - 127);
        }
        for (unsigned group = 0; group < 16; ++group) {
            int16_t sum = 0;
            for (unsigned i = 0; i < 16; ++i) {
                sum += quants[group * 16 + i];
            }
            store_u16(x + 4 + SOC_QK_K + 2 * group, (uint16_t) sum);
        }
    }
}

int main() {
    alignas(16) uint8_t weights[ROWS * NB * SOC_Q4_K_BLOCK_BYTES];
    alignas(16) uint8_t quants[ROWS * NB * SOC_Q4_K_QUANT_BYTES];
    alignas(16) uint8_t metadata[ROWS * NB * SOC_Q4_K_META_BYTES];
    alignas(16) uint8_t activation[NB * SOC_Q8_K_BLOCK_BYTES];
    uint8_t             unpacked[sizeof(weights)];
    float               actual[ROWS]   = { 0 };
    float               expected[ROWS] = { 0 };
    make_weights(weights);
    make_activation(activation);

    if (soc_q4kr_pack(weights, NB * SOC_Q4_K_BLOCK_BYTES, ROWS, NB, quants, metadata) ||
        soc_q4kr_unpack(quants, metadata, ROWS, NB, unpacked, NB * SOC_Q4_K_BLOCK_BYTES) ||
        std::memcmp(weights, unpacked, sizeof(weights))) {
        std::fprintf(stderr, "Q4_K repack round trip failed\n");
        return 1;
    }

    soc_q4kr_mat matrix = { ROWS, NB, quants, metadata, sizeof(quants), sizeof(metadata), NULL };
    soc_q4kr_gemv_ref(&matrix, activation, expected);
    uint32_t status = 99;
    soc_gemv_hw((const soc_gemv_word *) quants, (const soc_gemv_word *) metadata, activation, actual, ROWS, NB, status);
    if (status != SOC_GEMV_HW_OK || std::memcmp(actual, expected, sizeof(actual))) {
        std::fprintf(stderr, "Q4_K GEMV mismatch: status=%u\n", status);
        for (unsigned row = 0; row < ROWS; ++row) {
            std::fprintf(stderr, "  row %u actual=%a expected=%a\n", row, actual[row], expected[row]);
        }
        return 1;
    }

    soc_gemv_hw((const soc_gemv_word *) quants, (const soc_gemv_word *) metadata, activation, actual, 1,
                SOC_GEMV_HW_MAX_NB + 1, status);
    if (status != SOC_GEMV_HW_BAD_SHAPE) {
        std::fprintf(stderr, "Q4_K shape validation failed\n");
        return 1;
    }

    store_u32(activation, 0x7f800000u);
    soc_gemv_hw((const soc_gemv_word *) quants, (const soc_gemv_word *) metadata, activation, actual, ROWS, NB, status);
    if (status != SOC_GEMV_HW_BAD_X_SCALE) {
        std::fprintf(stderr, "Q8_K scale validation failed\n");
        return 1;
    }

    std::puts("Q4_K GEMV: PASS");
    return 0;
}
