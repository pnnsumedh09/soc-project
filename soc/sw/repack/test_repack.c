/*
 * test_repack.c - unit tests for soc_repack (task P2.1)
 *
 *   ./test_repack                       synthetic tests only
 *   ./test_repack tensor1.bin [...]     also test real tensors dumped from the
 *                                       GGUF by soc/tools/dump_q8_tensor.py
 *
 * Checks, for every matrix:
 *   1. pack -> unpack reproduces the original bytes exactly (lossless)
 *   2. layout: block (r, b) is at quants[(r*nb+b)*32] / scales[r*nb+b]
 *   3. 4 KB alignment of both arrays and zeroed padding
 *   4. GEMV on the repacked layout is bit-identical to the same math on the
 *      native layout (so the layout change cannot change model output)
 *   5. GEMV error vs a double-precision reference is small (sanity)
 * Plus argument-validation tests.
 */
#include "soc_repack.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            g_fail++;                                                         \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

/* ---- small deterministic RNG (xorshift32) ---- */
static uint32_t rng_state = 0x12345678u;
static uint32_t rng(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

/* Random FP16 scale: mostly normal values in a llama-like range, some special
 * cases (zero, denormal, negative, large). Never Inf/NaN. */
static uint16_t rand_scale(void)
{
    uint32_t k = rng() % 100;
    uint16_t sign = (rng() & 1) ? 0x8000 : 0;
    if (k < 2)  return sign;                                    /* +/- 0     */
    if (k < 6)  return sign | (uint16_t)(1 + rng() % 0x3FF);    /* denormal  */
    if (k < 8)  return sign | 0x7BFF;                           /* max finite*/
    /* normal, exponent 1..20 (about 6e-5 .. 32) */
    return sign | (uint16_t)(((1 + rng() % 20) << 10) | (rng() & 0x3FF));
}

/* Fill nrows native Q8_0 rows (row stride `stride` bytes; padding bytes 0xAB). */
static void fill_q8_0(unsigned char *buf, uint32_t rows, uint32_t nb, size_t stride)
{
    memset(buf, 0xAB, (size_t)rows * stride);
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t b = 0; b < nb; b++) {
            unsigned char *blk = buf + (size_t)r * stride + (size_t)b * SOC_Q8_0_BLOCK_BYTES;
            uint16_t d = rand_scale();
            memcpy(blk, &d, 2);
            for (int j = 0; j < SOC_QK; j++) {
                uint32_t k = rng() % 50;
                int8_t q = (k == 0) ? -128 : (k == 1) ? 127 : (int8_t)(rng() & 0xFF);
                blk[2 + j] = (unsigned char)q;
            }
        }
    }
}

/* Double-precision reference for one native row (for the error sanity check). */
static double row_dot_double(const unsigned char *w, const unsigned char *x, uint32_t nb)
{
    double s = 0.0;
    for (uint32_t b = 0; b < nb; b++) {
        const unsigned char *wb = w + (size_t)b * SOC_Q8_0_BLOCK_BYTES;
        const unsigned char *xb = x + (size_t)b * SOC_Q8_0_BLOCK_BYTES;
        uint16_t wd, xd;
        memcpy(&wd, wb, 2);
        memcpy(&xd, xb, 2);
        long sumi = 0;
        for (int j = 0; j < SOC_QK; j++)
            sumi += (long)(int8_t)wb[2 + j] * (int8_t)xb[2 + j];
        s += (double)sumi * (double)soc_fp16_to_fp32(wd) * (double)soc_fp16_to_fp32(xd);
    }
    return s;
}

/*
 * Run all per-matrix checks. src = native rows with stride `stride`.
 * name is for printing. Returns nothing; failures go to g_fail.
 */
static void check_matrix(const char *name, const unsigned char *src, uint32_t rows,
                         uint32_t nb, size_t stride)
{
    int fail_before = g_fail;
    soc_q8r_mat m;

    CHECK(soc_q8r_alloc_host(&m, rows, nb) == 0, "%s: alloc_host failed", name);
    if (g_fail != fail_before)
        return;

    /* 3. alignment, sizes */
    CHECK((uintptr_t)m.quants % SOC_REPACK_ALIGN == 0, "%s: quants not 4K aligned", name);
    CHECK((uintptr_t)m.scales % SOC_REPACK_ALIGN == 0, "%s: scales not 4K aligned", name);
    CHECK(m.quants_bytes % SOC_REPACK_ALIGN == 0 && m.quants_bytes >= (size_t)rows * nb * 32,
          "%s: quants_bytes %zu", name, m.quants_bytes);
    CHECK(m.scales_bytes % SOC_REPACK_ALIGN == 0 && m.scales_bytes >= (size_t)rows * nb * 2,
          "%s: scales_bytes %zu", name, m.scales_bytes);

    CHECK(soc_q8r_from_q8_0(&m, src, stride) == 0, "%s: pack failed", name);

    /* 3b. padding zero */
    size_t qused = (size_t)rows * nb * 32, sused = (size_t)rows * nb * 2;
    int pad_ok = 1;
    for (size_t i = qused; i < m.quants_bytes; i++)
        if (((unsigned char *)m.quants)[i]) { pad_ok = 0; break; }
    for (size_t i = sused; i < m.scales_bytes; i++)
        if (((unsigned char *)m.scales)[i]) { pad_ok = 0; break; }
    CHECK(pad_ok, "%s: padding not zero", name);

    /* 2. layout spot checks (first, last, and random blocks) */
    for (int t = 0; t < 64; t++) {
        uint32_t r = (t == 0) ? 0 : (t == 1) ? rows - 1 : rng() % rows;
        uint32_t b = (t == 0) ? 0 : (t == 1) ? nb - 1 : rng() % nb;
        const unsigned char *blk = src + (size_t)r * stride + (size_t)b * SOC_Q8_0_BLOCK_BYTES;
        uint16_t d;
        memcpy(&d, blk, 2);
        size_t idx = (size_t)r * nb + b;
        CHECK(m.scales[idx] == d, "%s: scale (%u,%u) mismatch", name, r, b);
        CHECK(memcmp(&m.quants[idx * 32], blk + 2, 32) == 0, "%s: quants (%u,%u) mismatch", name, r, b);
    }

    /* 1. lossless round trip (compare only the block bytes, not row padding) */
    size_t row_bytes = (size_t)nb * SOC_Q8_0_BLOCK_BYTES;
    unsigned char *back = malloc((size_t)rows * row_bytes);
    CHECK(back != NULL, "%s: malloc", name);
    if (back) {
        CHECK(soc_q8r_unpack(m.quants, m.scales, rows, nb, back, row_bytes) == 0, "%s: unpack failed", name);
        int rt_ok = 1;
        for (uint32_t r = 0; r < rows && rt_ok; r++)
            if (memcmp(back + (size_t)r * row_bytes, src + (size_t)r * stride, row_bytes))
                rt_ok = 0;
        CHECK(rt_ok, "%s: round trip not identical", name);
        free(back);
    }

    /* 4 + 5. GEMV: repacked vs native (bit-exact) and vs double (sanity) */
    unsigned char *x = malloc(row_bytes);
    float *y = malloc(sizeof(float) * rows);
    CHECK(x && y, "%s: malloc", name);
    if (x && y) {
        fill_q8_0(x, 1, nb, row_bytes);
        soc_q8r_gemv_ref(&m, x, y);
        uint32_t mismatches = 0;
        double max_rel = 0.0, max_abs = 0.0;
        for (uint32_t r = 0; r < rows; r++) {
            const unsigned char *wrow = src + (size_t)r * stride;
            float yn = soc_q8_0_row_dot_native(wrow, x, nb);
            if (memcmp(&yn, &y[r], sizeof(float)))
                mismatches++;
            double yd = row_dot_double(wrow, x, nb);
            double ae = fabs((double)y[r] - yd);
            /* relative error against the sum of magnitudes avoids blow-up on cancellation */
            double mag = 0.0;
            for (uint32_t b = 0; b < nb; b++) {
                const unsigned char *wb = wrow + (size_t)b * SOC_Q8_0_BLOCK_BYTES;
                const unsigned char *xb = x + (size_t)b * SOC_Q8_0_BLOCK_BYTES;
                uint16_t wd, xd; memcpy(&wd, wb, 2); memcpy(&xd, xb, 2);
                long sa = 0;
                for (int j = 0; j < SOC_QK; j++) sa += labs((long)(int8_t)wb[2 + j] * (int8_t)xb[2 + j]);
                mag += (double)sa * fabs((double)soc_fp16_to_fp32(wd) * (double)soc_fp16_to_fp32(xd));
            }
            double re = mag > 0 ? ae / mag : 0.0;
            if (ae > max_abs) max_abs = ae;
            if (re > max_rel) max_rel = re;
        }
        CHECK(mismatches == 0, "%s: %u/%u rows differ between repacked and native GEMV", name, mismatches, rows);
        /* FP32 accumulation over <= 152 blocks: error bound ~ nb * 2^-24 relative to |terms| */
        CHECK(max_rel < 1e-5, "%s: max relative error %.3g vs double", name, max_rel);
        printf("  %-34s rows=%-6u nb=%-4u quants=%zu B scales=%zu B  max|err|=%.3g  rel=%.3g  %s\n",
               name, rows, nb, m.quants_bytes, m.scales_bytes, max_abs, max_rel,
               g_fail == fail_before ? "OK" : "FAIL");
    }
    free(x);
    free(y);
    soc_q8r_free_host(&m);
}

static void test_synthetic(void)
{
    struct { uint32_t rows, nb; size_t extra_stride; const char *name; } cases[] = {
        {   1,   1,  0, "1x32"                     },
        {   3,  28,  0, "3x896"                    },
        { 128,  28,  0, "attn_k shape 128x896"     },
        { 896,  28,  0, "attn_q shape 896x896"     },
        { 896, 152,  0, "ffn_down shape 896x4864"  },
        {  17, 152,  0, "17x4864"                  },
        {  40,  28, 13, "40x896, padded row stride"},
        { 129,   5,  0, "129x160 (odd sizes)"      },
    };
    printf("Synthetic matrices:\n");
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        size_t stride = (size_t)cases[i].nb * SOC_Q8_0_BLOCK_BYTES + cases[i].extra_stride;
        unsigned char *src = malloc((size_t)cases[i].rows * stride);
        fill_q8_0(src, cases[i].rows, cases[i].nb, stride);
        check_matrix(cases[i].name, src, cases[i].rows, cases[i].nb, stride);
        free(src);
    }
}

static void test_fp16(void)
{
    printf("FP16 conversion:\n");
    struct { uint16_t h; float f; } v[] = {
        { 0x0000, 0.0f }, { 0x3C00, 1.0f }, { 0xC000, -2.0f }, { 0x7BFF, 65504.0f },
        { 0x0400, 6.103515625e-05f },             /* smallest normal    */
        { 0x0001, 5.9604644775390625e-08f },      /* smallest denormal  */
        { 0x03FF, 6.09755516052246094e-05f },     /* largest denormal   */
        { 0x8001, -5.9604644775390625e-08f },
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        float got = soc_fp16_to_fp32(v[i].h);
        if (got != v[i].f) {
            ok = 0;
            CHECK(0, "fp16 0x%04x -> %.9g, expected %.9g", v[i].h, got, v[i].f);
        }
    }
    /* Inf and NaN */
    CHECK(isinf(soc_fp16_to_fp32(0x7C00)) && soc_fp16_to_fp32(0x7C00) > 0, "fp16 +inf");
    CHECK(isnan(soc_fp16_to_fp32(0x7E00)), "fp16 nan");
    /* exhaustive check against an independent formula for all finite values */
    for (uint32_t h = 0; h < 0x10000; h++) {
        uint32_t e = (h >> 10) & 0x1F, mn = h & 0x3FF;
        if (e == 0x1F) continue;
        double ref = e ? ldexp(1.0 + mn / 1024.0, (int)e - 15) : ldexp(mn / 1024.0, -14);
        if (h & 0x8000) ref = -ref;
        if ((double)soc_fp16_to_fp32((uint16_t)h) != ref) { ok = 0; CHECK(0, "fp16 0x%04x", h); break; }
    }
    printf("  all 63488 finite FP16 values exact: %s\n", ok ? "OK" : "FAIL");
}

static void test_bad_args(void)
{
    printf("Argument validation:\n");
    int8_t q[64];
    uint16_t s[2];
    unsigned char src[68] = {0};
    soc_q8r_mat m;
    int before = g_fail;

    errno = 0;
    CHECK(soc_q8r_pack(NULL, 34, 1, 1, q, s) == -1 && errno == EINVAL, "NULL src accepted");
    CHECK(soc_q8r_pack(src, 34, 0, 1, q, s) == -1, "rows=0 accepted");
    CHECK(soc_q8r_pack(src, 34, 1, 0, q, s) == -1, "nb=0 accepted");
    CHECK(soc_q8r_pack(src, 33, 1, 1, q, s) == -1, "stride too small accepted");
    CHECK(soc_q8r_pack(src, 34, 2, 1, q, s) == 0, "valid pack rejected");
    CHECK(soc_q8r_unpack(q, s, 1, 2, src, 67) == -1, "unpack stride too small accepted");
    CHECK(soc_q8r_quants_bytes(0, 5) == 0 && soc_q8r_scales_bytes(5, 0) == 0, "zero size not 0");
    CHECK(soc_q8r_quants_bytes(1, 1) == 4096 && soc_q8r_scales_bytes(1, 1) == 4096, "rounding");
    CHECK(soc_q8r_quants_bytes(128, 1) == 4096 && soc_q8r_quants_bytes(129, 1) == 8192, "rounding 2");

    /* init with misaligned / short buffers */
    void *buf = aligned_alloc(SOC_REPACK_ALIGN, 3 * SOC_REPACK_ALIGN);
    CHECK(soc_q8r_init(&m, 1, 1, (char *)buf + 16, 4096, (char *)buf + 8192, 4096) == -1,
          "misaligned quants accepted");
    CHECK(soc_q8r_init(&m, 200, 1, buf, 4096, (char *)buf + 8192, 4096) == -1,
          "short quants buffer accepted");
    CHECK(soc_q8r_init(&m, 1, 1, buf, 4096, (char *)buf + 8192, 4096) == 0,
          "valid init rejected");
    free(buf);
    printf("  %s\n", g_fail == before ? "OK" : "FAIL");
}

/* Real tensor file written by soc/tools/dump_q8_tensor.py:
 *   char magic[8] = "SOCQ8T1\0"; uint32 ne0 (in); uint32 rows; uint64 row_stride;
 *   char name[64]; then rows * row_stride bytes of native Q8_0 data. */
static void test_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { CHECK(0, "cannot open %s", path); return; }
    char magic[8], name[65] = {0};
    uint32_t ne0, rows;
    uint64_t stride;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "SOCQ8T1", 8) ||
        fread(&ne0, 4, 1, f) != 1 || fread(&rows, 4, 1, f) != 1 ||
        fread(&stride, 8, 1, f) != 1 || fread(name, 1, 64, f) != 64 ||
        ne0 % SOC_QK || stride < (uint64_t)(ne0 / SOC_QK) * SOC_Q8_0_BLOCK_BYTES) {
        CHECK(0, "%s: bad header", path);
        fclose(f);
        return;
    }
    unsigned char *src = malloc((size_t)rows * stride);
    if (!src || fread(src, 1, (size_t)rows * stride, f) != (size_t)rows * stride) {
        CHECK(0, "%s: short read", path);
        free(src);
        fclose(f);
        return;
    }
    fclose(f);
    check_matrix(name, src, rows, ne0 / SOC_QK, (size_t)stride);
    free(src);
}

int main(int argc, char **argv)
{
    test_fp16();
    test_bad_args();
    test_synthetic();
    if (argc > 1) {
        printf("Real tensors:\n");
        for (int i = 1; i < argc; i++)
            test_file(argv[i]);
    }
    printf("\n%s (%d failure%s)\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
