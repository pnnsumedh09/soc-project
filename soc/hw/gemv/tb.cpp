#include "gemv.h"
#include "soc_repack.h"
#include "dot_q8_0_core.h"

#include <algorithm>
#include <cmath>
#include <stdio.h>
#include <string.h>
#include <vector>

static const uint32_t TB_ROWS = 64;
static const float CANARY = -12345.5f;
static uint32_t rng_state = 1;
static int failures = 0;
static unsigned calls = 0;
static uint64_t checked_rows = 0;
static double max_abs_error = 0;

static uint32_t random_u32() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void put_half(uint8_t * dst, uint16_t value) {
    dst[0] = (uint8_t) value;
    dst[1] = (uint8_t) (value >> 8);
}

static uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void fill_blocks(std::vector<uint8_t> & data, uint32_t blocks, unsigned pattern, bool activation) {
    const uint16_t scales[] = {0x0001, 0x03ff, 0x0400, 0x3555, 0x3bff, 0x3c00, 0x4000, 0x7bff, 0x8001, 0xbc00};
    for (uint32_t b = 0; b < blocks; ++b) {
        uint8_t * p = data.data() + b * 34;
        uint16_t scale = scales[random_u32() % (sizeof(scales) / sizeof(scales[0]))];
        if (pattern == 0) scale = (b & 1) ? 0x8000 : 0;
        if (pattern == 1 || pattern == 4 || pattern == 5) scale = 0x3c00;
        if (pattern == 3) scale = activation ? 0x0001 : 0x03ff;
        put_half(p, scale);
        for (int j = 0; j < 32; ++j) {
            uint8_t q = (uint8_t) random_u32();
            if (pattern == 1) q = (b & 1) ? 127 : 129;
            if (pattern == 4) q = 128;
            if (pattern == 5) q = activation ? 127 : 128;
            p[2 + j] = q;
        }
    }
}

static void check_matrix(const char * name, const std::vector<uint8_t> & native,
                         const std::vector<uint8_t> & x, uint32_t rows, uint32_t nb,
                         uint32_t expected_status = SOC_GEMV_HW_OK) {
    soc_q8r_mat matrix = {};
    if (soc_q8r_alloc_host(&matrix, rows, nb) || soc_q8r_from_q8_0(&matrix, native.data(), nb * 34)) {
        fprintf(stderr, "FAIL %s: reference allocation/repack\n", name);
        ++failures;
        soc_q8r_free_host(&matrix);
        return;
    }
    // Fixed small-test capacity matches the HLS co-simulation adapter depths.
    size_t capacity_blocks = rows <= TB_ROWS ? TB_ROWS * SOC_GEMV_HW_MAX_NB : (size_t) rows * nb;
    std::vector<soc_gemv_word> quants(capacity_blocks * 2, 0);
    std::vector<uint16_t> scales(capacity_blocks, 0);
    std::vector<uint8_t> activation(SOC_GEMV_HW_MAX_NB * 34, 0);
    std::vector<float> output(std::max(rows, TB_ROWS) + 2, CANARY);
    std::vector<float> expected(rows);
    memcpy(activation.data(), x.data(), nb * 34);
    memcpy(scales.data(), matrix.scales, (size_t) rows * nb * 2);
    for (size_t i = 0; i < (size_t) rows * nb * 2; ++i) {
        soc_gemv_word word = 0;
        for (int j = 0; j < 16; ++j) {
            word |= soc_gemv_word((uint8_t) matrix.quants[i * 16 + j]) << (8 * j);
        }
        quants[i] = word;
    }
    if (expected_status == SOC_GEMV_HW_OK) {
        soc_q8r_gemv_ref(&matrix, activation.data(), expected.data());
    }
    uint32_t status = UINT32_MAX;
    soc_gemv_hw(quants.data(), scales.data(), activation.data(), output.data() + 1, rows, nb, status);
    ++calls;
    int before = failures;
    if (status != expected_status) {
        fprintf(stderr, "FAIL %s: status %u expected %u\n", name, status, expected_status);
        ++failures;
    }
    if (expected_status == SOC_GEMV_HW_OK && status == SOC_GEMV_HW_OK) {
        for (uint32_t row = 0; row < rows; ++row) {
            float got = output[row + 1];
            double error = std::fabs((double) got - expected[row]);
            max_abs_error = std::max(max_abs_error, error);
            if (!std::isfinite(got) || float_bits(got) != float_bits(expected[row])) {
                if (failures - before < 4) {
                    fprintf(stderr, "FAIL %s row=%u got=%08x expected=%08x\n",
                            name, row, float_bits(got), float_bits(expected[row]));
                }
                ++failures;
            }
        }
        checked_rows += rows;
    }
#ifdef SOC_GEMV_HW_HOST
    if (output[0] != CANARY) ++failures;
    for (size_t i = rows + 1; i < output.size(); ++i) {
        if (output[i] != CANARY) ++failures;
    }
    if (expected_status == SOC_GEMV_HW_BAD_X_SCALE) {
        for (size_t i = 1; i <= rows; ++i) {
            if (output[i] != CANARY) ++failures;
        }
    }
#endif
    printf("  %-32s rows=%u nb=%u status=%u %s\n", name, rows, nb, status, before == failures ? "PASS" : "FAIL");
    soc_q8r_free_host(&matrix);
}

static void synthetic(uint32_t rows, uint32_t nb, unsigned pattern) {
    std::vector<uint8_t> w((size_t) rows * nb * 34);
    std::vector<uint8_t> x(nb * 34);
    fill_blocks(w, rows * nb, pattern, false);
    fill_blocks(x, nb, pattern, true);
    char name[64];
    snprintf(name, sizeof(name), "synthetic pattern %u", pattern);
    check_matrix(name, w, x, rows, nb);
}

static void bad_shapes() {
    std::vector<soc_gemv_word> w(TB_ROWS * SOC_GEMV_HW_MAX_NB * 2, 0);
    std::vector<uint16_t> s(TB_ROWS * SOC_GEMV_HW_MAX_NB, 0);
    std::vector<uint8_t> x(SOC_GEMV_HW_MAX_NB * 34, 0);
    std::vector<float> y(TB_ROWS, CANARY);
    const uint32_t cases[][3] = {
        {0, 0, SOC_GEMV_HW_OK}, {0, 152, SOC_GEMV_HW_OK},
        {1, 0, SOC_GEMV_HW_BAD_SHAPE}, {1, 153, SOC_GEMV_HW_BAD_SHAPE},
        {151937, 1, SOC_GEMV_HW_BAD_SHAPE}, {UINT32_MAX, UINT32_MAX, SOC_GEMV_HW_BAD_SHAPE},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint32_t status = UINT32_MAX;
        soc_gemv_hw(w.data(), s.data(), x.data(), y.data(), cases[i][0], cases[i][1], status);
        ++calls;
        if (status != cases[i][2]) ++failures;
#ifdef SOC_GEMV_HW_HOST
        for (size_t j = 0; j < y.size(); ++j) if (y[j] != CANARY) ++failures;
#endif
    }
}

static void nonfinite_scales() {
    std::vector<uint8_t> w(3 * 28 * 34), x(28 * 34);
    fill_blocks(w, 3 * 28, 2, false);
    fill_blocks(x, 28, 2, true);
    const uint16_t bad[] = {0x7c00, 0xfc00, 0x7e00};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        std::vector<uint8_t> changed_x = x, changed_w = w;
        put_half(changed_x.data() + 27 * 34, bad[i]);
        check_matrix("nonfinite activation scale", w, changed_x, 3, 28, SOC_GEMV_HW_BAD_X_SCALE);
        put_half(changed_w.data() + (2 * 28 + 27) * 34, bad[i]);
        check_matrix("nonfinite late weight scale", changed_w, x, 3, 28, SOC_GEMV_HW_BAD_W_SCALE);
    }
}

static uint64_t little_endian(const uint8_t * p, unsigned bytes) {
    uint64_t result = 0;
    for (unsigned i = 0; i < bytes; ++i) result |= (uint64_t) p[i] << (8 * i);
    return result;
}

static void tensor_file(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "FAIL cannot open %s\n", path); ++failures; return; }
    uint8_t header[88];
    bool valid = fread(header, 1, sizeof(header), f) == sizeof(header);
    uint32_t in = valid ? (uint32_t) little_endian(header + 8, 4) : 0;
    uint32_t rows = valid ? (uint32_t) little_endian(header + 12, 4) : 0;
    uint64_t stride = valid ? little_endian(header + 16, 8) : 0;
    valid = valid && !memcmp(header, "SOCQ8T1", 8) && in && in % 32 == 0 &&
            in / 32 <= SOC_GEMV_HW_MAX_NB && rows && rows <= SOC_GEMV_HW_MAX_ROWS && stride >= (in / 32) * 34;
    long length = -1;
    if (valid && !fseek(f, 0, SEEK_END)) length = ftell(f);
    valid = valid && length >= (long) sizeof(header) && stride <= ((uint64_t) length - sizeof(header)) / rows;
    if (!valid) {
        fprintf(stderr, "FAIL invalid/short tensor file %s\n", path);
        ++failures;
        fclose(f);
        return;
    }
    rows = std::min(rows, TB_ROWS);
    uint32_t nb = in / 32;
    std::vector<uint8_t> w((size_t) rows * nb * 34), x(nb * 34);
    for (uint32_t row = 0; row < rows; ++row) {
        long offset = (long) (sizeof(header) + row * stride);
        if (fseek(f, offset, SEEK_SET) || fread(w.data() + row * nb * 34, 1, nb * 34, f) != nb * 34) {
            fprintf(stderr, "FAIL short tensor row in %s\n", path);
            ++failures;
            fclose(f);
            return;
        }
    }
    fclose(f);
    fill_blocks(x, nb, 2, true);
    check_matrix(path, w, x, rows, nb);
}

int main(int argc, char ** argv) {
    for (uint32_t h = 0; h <= 0xffff; ++h) {
        if ((h & 0x7c00) != 0x7c00 && float_bits(dot_half_to_float((uint16_t) h)) != float_bits(soc_fp16_to_fp32((uint16_t) h))) {
            ++failures;
        }
    }
    printf("Finite FP16 conversion comparison complete\n");
    // Exercise full/tail row groups and both sides of the 32-block tile boundary.
    const uint32_t shapes[][2] = {{1, 1}, {3, 1}, {4, 3}, {7, 28}, {32, 28}, {9, 152}, {64, 152},
                                  {8, 32}, {9, 33}, {17, 65}, {16, 64}, {15, 31}};
    for (unsigned p = 0; p < 6; ++p) {
        for (unsigned i = 0; i < sizeof(shapes) / sizeof(shapes[0]); ++i) {
            synthetic(shapes[i][0], shapes[i][1], p);
        }
    }
    bad_shapes();
    nonfinite_scales();
    // Reject an invalid scale after both a row-group and block-tile boundary.
    {
        std::vector<uint8_t> w(9 * 33 * 34), x(33 * 34);
        fill_blocks(w, 9 * 33, 2, false);
        fill_blocks(x, 33, 2, true);
        put_half(w.data() + (8 * 33 + 32) * 34, 0x7e00);
        check_matrix("nonfinite tail tile scale", w, x, 9, 33, SOC_GEMV_HW_BAD_W_SCALE);
    }
#ifdef SOC_GEMV_HW_HOST
    synthetic(SOC_GEMV_HW_MAX_ROWS, 1, 2);
#endif
    for (int i = 1; i < argc; ++i) tensor_file(argv[i]);
    printf("GEMV HW: %s; calls=%u rows=%llu max_abs_error=%.9g failures=%d\n",
           failures ? "FAIL" : "PASS", calls, (unsigned long long) checked_rows, max_abs_error, failures);
    return failures ? 1 : 0;
}
