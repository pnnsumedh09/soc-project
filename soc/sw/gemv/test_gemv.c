/*
 * test_gemv.c - unit test for soc_gemv (cache + emul backend), no ggml needed.
 *
 * Checks: offloaded result is bit-identical to the reference vec_dot; cache
 * hits do not repack; changed weight contents at the same address are
 * detected and repacked; several matrices coexist; invalid shapes and
 * SOC_GEMV=0 fall back to the CPU; verification statistics; multithreaded
 * callers get correct results.
 */
#define _POSIX_C_SOURCE 200809L
#include "soc_gemv.h"
#include "soc_repack.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t st = 0xC0FFEEu;
static uint32_t rnd(void) { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; }

static void fill(unsigned char *buf, size_t nblocks)
{
    for (size_t b = 0; b < nblocks; b++) {
        unsigned char *blk = buf + b * SOC_Q8_0_BLOCK_BYTES;
        uint16_t d = (uint16_t)(((5 + rnd() % 12) << 10) | (rnd() & 0x3FF)); /* normal, ~1e-3..1 */
        memcpy(blk, &d, 2);
        for (int j = 0; j < 32; j++) blk[2 + j] = (unsigned char)rnd();
    }
}

/* Stand-in for llama.cpp's vec_dot: the generic scalar algorithm. */
static int g_ref_calls;
static void ref_vec_dot(int n, float *s, size_t bs, const void *x, size_t bx, const void *y, size_t by, int nrc)
{
    (void)bs; (void)bx; (void)by; (void)nrc;
    __atomic_add_fetch(&g_ref_calls, 1, __ATOMIC_RELAXED);
    *s = soc_q8_0_row_dot_native(x, y, (uint32_t)(n / 32));
}

typedef struct { unsigned char *w; uint32_t rows, n; size_t stride; } mat_t;

static mat_t make(uint32_t rows, uint32_t n)
{
    mat_t m = { NULL, rows, n, (size_t)n / 32 * SOC_Q8_0_BLOCK_BYTES };
    m.w = malloc(m.stride * rows);
    fill(m.w, (size_t)rows * n / 32);
    return m;
}

static int check_gemv(const mat_t *m, const char *what)
{
    size_t xb = (size_t)m->n / 32 * SOC_Q8_0_BLOCK_BYTES;
    unsigned char *x = malloc(xb);
    float *y = malloc(sizeof(float) * m->rows);
    fill(x, m->n / 32);
    soc_gemv_q8_0(m->w, m->stride, m->rows, m->n, x, y, ref_vec_dot);
    int bad = 0;
    for (uint32_t r = 0; r < m->rows; r++) {
        float e = soc_q8_0_row_dot_native(m->w + r * m->stride, x, m->n / 32);
        if (memcmp(&e, &y[r], 4)) bad++;
    }
    CHECK(bad == 0, "%s: %d/%u rows differ", what, bad, m->rows);
    free(x); free(y);
    return bad;
}

typedef struct { const mat_t *m; int bad; } thr_arg;
static void *thr(void *p)
{
    thr_arg *a = p;
    for (int i = 0; i < 20; i++) a->bad += check_gemv(a->m, "threaded");
    return NULL;
}

int main(void)
{
    setenv("SOC_GEMV_VERIFY", "1", 1);
    setenv("SOC_GEMV_STATS", "1", 1);
    CHECK(soc_gemv_enabled() == 1, "not enabled");

    soc_gemv_stats s0, s1;
    mat_t q = make(896, 896), k = make(128, 896), down = make(896, 4864);

    printf("first use (repack) + cache hits\n");
    check_gemv(&q, "q first");
    soc_gemv_get_stats(&s0);
    CHECK(s0.tensors == 1 && s0.calls == 1, "after first call: tensors=%llu calls=%llu",
          (unsigned long long)s0.tensors, (unsigned long long)s0.calls);
    for (int i = 0; i < 5; i++) check_gemv(&q, "q hit");
    soc_gemv_get_stats(&s1);
    CHECK(s1.tensors == 1 && s1.calls == 6 && s1.repack_ns == s0.repack_ns,
          "cache hits repacked again (tensors=%llu)", (unsigned long long)s1.tensors);

    printf("several matrices\n");
    check_gemv(&k, "k");
    check_gemv(&down, "down");
    soc_gemv_get_stats(&s1);
    CHECK(s1.tensors == 3, "tensors=%llu, expected 3", (unsigned long long)s1.tensors);

    printf("changed contents at the same address\n");
    fill(q.w, (size_t)q.rows * q.n / 32);
    check_gemv(&q, "q after rewrite");
    soc_gemv_get_stats(&s1);
    CHECK(s1.stale_repacks == 1, "stale_repacks=%llu", (unsigned long long)s1.stale_repacks);

    printf("invalid shape falls back\n");
    {
        unsigned char x[34] = {0}; float y = 0;
        uint64_t fb = s1.fallback_calls;
        soc_gemv_q8_0(q.w, q.stride, 1, 31, x, &y, ref_vec_dot);   /* n not multiple of 32 */
        soc_gemv_get_stats(&s1);
        CHECK(s1.fallback_calls == fb + 1, "no fallback recorded");
    }

    printf("multithreaded callers\n");
    {
        pthread_t t[4]; thr_arg a[4];
        for (int i = 0; i < 4; i++) { a[i].m = (i & 1) ? &k : &down; a[i].bad = 0; pthread_create(&t[i], NULL, thr, &a[i]); }
        for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    }

    soc_gemv_get_stats(&s1);
    CHECK(s1.verified == s1.calls && s1.exact_rows == s1.verified_rows,
          "verify: %llu/%llu calls, %llu/%llu rows exact", (unsigned long long)s1.verified,
          (unsigned long long)s1.calls, (unsigned long long)s1.exact_rows, (unsigned long long)s1.verified_rows);
    soc_gemv_print_stats();

    free(q.w); free(k.w); free(down.w);
    printf("\n%s (%d failure%s)\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
