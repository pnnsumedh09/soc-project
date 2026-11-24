#include "gemm.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static uint32_t rng_state = 0x12345678u;
static int failures = 0;

static uint32_t random_u32() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static float half_to_float_ref(uint16_t h) {
    uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    uint32_t exp = ((uint32_t)h >> 10) & 0x1fu;
    uint32_t frac = (uint32_t)h & 0x03ffu;
    uint32_t bits;
    if (exp == 0) {
        if (frac == 0) {
            bits = sign;
        } else {
            int shift = 0;
            while ((frac & 0x0400u) == 0) {
                frac <<= 1;
                ++shift;
            }
            frac &= 0x03ffu;
            bits = sign | ((uint32_t)(113 - shift) << 23) | (frac << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7f800000u | (frac << 13);
    } else {
        bits = sign | ((exp + 112u) << 23) | (frac << 13);
    }
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t float_bits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static int32_t signed_byte(uint8_t value) {
    return value < 128 ? (int32_t)value : (int32_t)value - 256;
}

static soc_gemm_word pack_16(const uint8_t *bytes) {
    soc_gemm_word word = 0;
    for (int j = 0; j < 16; ++j) {
        word |= soc_gemm_word(bytes[j]) << (8 * j);
    }
    return word;
}

static soc_gemm_word pack_q4_0(const uint8_t *values) {
    uint8_t packed[16];
    for (int j = 0; j < 16; ++j) {
        packed[j] = (uint8_t)((values[j] & 0x0fu) |
                              ((values[j + 16] & 0x0fu) << 4));
    }
    return pack_16(packed);
}

static void put_half(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void run_shape(uint32_t m, uint32_t nb, uint32_t n) {
    const uint32_t weight_type = SOC_GEMM_WEIGHT_BITS;
    static const uint16_t scales[] = {
        0x0001, 0x0400, 0x3000, 0x3800, 0x3c00, 0x4000, 0xb800
    };
    const float canary = -9876.5f;
    size_t blocks = (size_t)m * nb;
    std::vector<soc_gemm_word> w_quants(blocks * 2);
    std::vector<uint16_t> w_scales(blocks);
    // Q8_0 stores raw signed bytes. Q4_0 stores decoded unsigned nibbles here;
    // pack_q4_0 applies the GGML low-half/high-half nibble layout.
    std::vector<uint8_t> w_values(blocks * 32);
    std::vector<uint8_t> x_native((size_t)n * nb * 34);
    std::vector<float> expected((size_t)n * m);
    std::vector<float> output((size_t)n * m + 2, canary);

    for (size_t block = 0; block < blocks; ++block) {
        w_scales[block] = scales[random_u32() %
                                 (sizeof(scales) / sizeof(scales[0]))];
        for (int j = 0; j < 32; ++j) {
            w_values[block * 32 + j] = weight_type == 4
                ? (uint8_t)(random_u32() & 0x0fu)
                : (uint8_t)random_u32();
        }
        if (weight_type == 4) {
            w_quants[block] = pack_q4_0(&w_values[block * 32]);
        } else {
            w_quants[2 * block] = pack_16(&w_values[block * 32]);
            w_quants[2 * block + 1] = pack_16(&w_values[block * 32 + 16]);
        }
    }

    for (uint32_t col = 0; col < n; ++col) {
        for (uint32_t b = 0; b < nb; ++b) {
            uint8_t *block = &x_native[((size_t)col * nb + b) * 34];
            uint16_t scale = scales[random_u32() %
                                    (sizeof(scales) / sizeof(scales[0]))];
            put_half(block, scale);
            for (int j = 0; j < 32; ++j) {
                block[2 + j] = (uint8_t)random_u32();
            }
        }
    }

    for (uint32_t col = 0; col < n; ++col) {
        for (uint32_t row = 0; row < m; ++row) {
            float sum = 0.0f;
            for (uint32_t b = 0; b < nb; ++b) {
                const uint8_t *wb = &w_values[((size_t)row * nb + b) * 32];
                const uint8_t *xb = &x_native[((size_t)col * nb + b) * 34 + 2];
                int32_t sumi = 0;
                for (int j = 0; j < 32; ++j) {
                    int32_t wv = weight_type == 4
                        ? (int32_t)wb[j] - 8
                        : signed_byte(wb[j]);
                    sumi += wv * signed_byte(xb[j]);
                }
                float scale = half_to_float_ref(w_scales[(size_t)row * nb + b]) *
                              half_to_float_ref((uint16_t)x_native[((size_t)col * nb + b) * 34] |
                                                ((uint16_t)x_native[((size_t)col * nb + b) * 34 + 1] << 8));
                float term = (float)sumi * scale;
                sum = sum + term;
            }
            expected[(size_t)col * m + row] = sum;
        }
    }

    uint32_t status = 0xffffffffu;
    soc_gemm_hw(w_quants.data(), w_scales.data(), x_native.data(),
                output.data() + 1, m, nb, n, status);

    int before = failures;
    if (status != SOC_GEMM_HW_OK) {
        std::fprintf(stderr, "FAIL Q%u_0 shape M=%u K=%u N=%u status=%u\n",
                     weight_type, m, 32 * nb, n, status);
        ++failures;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (float_bits(output[i + 1]) != float_bits(expected[i])) {
            if (failures - before < 5) {
                std::fprintf(stderr,
                             "FAIL Q%u_0 M=%u K=%u N=%u index=%zu got=%08x expected=%08x\n",
                             weight_type, m, 32 * nb, n, i, float_bits(output[i + 1]),
                             float_bits(expected[i]));
            }
            ++failures;
        }
    }
    if (output.front() != canary || output.back() != canary) {
        std::fprintf(stderr, "FAIL Q%u_0 output canary M=%u K=%u N=%u\n",
                     weight_type, m, 32 * nb, n);
        ++failures;
    }
    std::printf("Q%u_0 M=%-3u K=%-5u N=%-3u %s\n",
                weight_type, m, 32 * nb, n,
                failures == before ? "PASS" : "FAIL");
}

static void bad_shapes() {
    std::vector<soc_gemm_word> w(2, 0);
    std::vector<uint16_t> s(1, 0x3c00);
    std::vector<uint8_t> x(34, 0);
    std::vector<float> y(1, 123.0f);
    uint32_t status = 0;
    soc_gemm_hw(w.data(), s.data(), x.data(), y.data(),
                1, 0, 1, status);
    if (status != SOC_GEMM_HW_BAD_SHAPE || y[0] != 123.0f) {
        std::fprintf(stderr, "FAIL bad-shape rejection\n");
        ++failures;
    }
}

int main() {
    const uint32_t shapes[][3] = {
        {1, 1, 1}, {3, 1, 2}, {4, 3, 5},
        {7, 28, 3}, {8, 33, 4}, {5, 152, 2},
        {2, 7, 8}, {3, 5, 9}, {2, 2, 17}
    };
    for (unsigned i = 0; i < sizeof(shapes) / sizeof(shapes[0]); ++i) {
        run_shape(shapes[i][0], shapes[i][1], shapes[i][2]);
    }
    bad_shapes();
    std::printf("Q%u_0 GEMM C-SIM %s; failures=%d\n",
                SOC_GEMM_WEIGHT_BITS,
                failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
