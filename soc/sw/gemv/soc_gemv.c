/*
 * soc_gemv.c - weight cache, backend dispatch, fallback, verification, stats
 * (plan tasks P2.2 / P2.3). See soc_gemv.h.
 */
#define _POSIX_C_SOURCE 200809L

#include "soc_gemv.h"
#include "soc_gemv_backend.h"
#include "soc_repack.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------------- */
/* Weight cache                                                               */
/* ------------------------------------------------------------------------- */

/* Qwen2.5-0.5B has 169 offloadable matrices; leave plenty of headroom.
 * Must be a power of two. When full, extra matrices run on the CPU. */
#define CACHE_SLOTS 1024

enum { SLOT_EMPTY = 0, SLOT_READY, SLOT_CPU_ONLY };

typedef struct {
    int           state;
    const void   *w;                         /* key: weight pointer ...       */
    size_t        stride;                    /* ... plus shape                */
    uint32_t      rows, nb;
    unsigned char head[SOC_Q8_0_BLOCK_BYTES];/* first and last block, to detect */
    unsigned char tail[SOC_Q8_0_BLOCK_BYTES];/* reuse of the address for new data */
    soc_q8r_mat   mat;
    void         *q_handle, *s_handle;
    uint64_t      q_dev, s_dev;
} slot_t;

static slot_t g_slots[CACHE_SLOTS];

/* ------------------------------------------------------------------------- */
/* Global state                                                               */
/* ------------------------------------------------------------------------- */

static pthread_once_t  g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int g_enabled, g_verify, g_print_stats, g_strict;
static const soc_gemv_backend *g_be;
static soc_gemv_stats g_st;

static float  *g_ref_buf;          /* scratch for verification */
static size_t  g_ref_cap;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int env_flag(const char *name, int dflt)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return dflt;
    return !(v[0] == '0' && v[1] == '\0');
}

static void at_exit_handler(void)
{
    if (g_print_stats)
        soc_gemv_print_stats();
    soc_gemv_shutdown();
    if (g_enabled && g_be)
        g_be->shutdown();
}

static void init_once(void)
{
    g_enabled     = env_flag("SOC_GEMV", 1);
    g_verify      = env_flag("SOC_GEMV_VERIFY", 0);
    g_print_stats = env_flag("SOC_GEMV_STATS", 0);
    g_strict      = env_flag("SOC_GEMV_STRICT", 0);

    const char *name = getenv("SOC_GEMV_BACKEND");
    if (!name || !*name)
        name = "emul";

    if (strcmp(name, soc_gemv_backend_emul.name) == 0) {
        g_be = &soc_gemv_backend_emul;
#ifdef SOC_GEMV_WITH_FPGA
    } else if (strcmp(name, soc_gemv_backend_fpga.name) == 0) {
        g_be = &soc_gemv_backend_fpga;
#endif
    } else {
        fprintf(stderr, "soc_gemv: unknown backend '%s'\n", name);
        if (g_strict)
            abort();
        g_enabled = 0;
    }

    if (g_enabled && g_be->init() != 0) {
        fprintf(stderr, "soc_gemv: backend '%s' init failed\n", g_be->name);
        if (g_strict)
            abort();
        g_enabled = 0;
    }

    fprintf(stderr, "soc_gemv: %s (backend=%s verify=%d stats=%d strict=%d)\n",
            g_enabled ? "enabled" : "disabled", g_be ? g_be->name : "none",
            g_verify, g_print_stats, g_strict);

    atexit(at_exit_handler);
}

int soc_gemv_enabled(void)
{
    pthread_once(&g_once, init_once);
    return g_enabled;
}

/* ------------------------------------------------------------------------- */
/* CPU path (fallback and verification reference)                             */
/* ------------------------------------------------------------------------- */

static void cpu_gemv(const void *w, size_t stride, uint32_t rows, uint32_t nb,
                     const void *x, float *y, soc_vec_dot_fn ref)
{
    const unsigned char *wb = (const unsigned char *)w;
    for (uint32_t r = 0; r < rows; r++) {
        const void *row = wb + (size_t)r * stride;
        if (ref)
            ref((int)(nb * SOC_QK), &y[r], 0, row, 0, x, 0, 1);
        else
            y[r] = soc_q8_0_row_dot_native(row, x, nb);
    }
}

/* ------------------------------------------------------------------------- */
/* Cache management (caller holds g_lock)                                     */
/* ------------------------------------------------------------------------- */

static size_t hash_ptr(const void *p)
{
    uint64_t h = (uint64_t)(uintptr_t)p;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return (size_t)h & (CACHE_SLOTS - 1);
}

static void fingerprint(const void *w, size_t stride, uint32_t rows, uint32_t nb,
                        unsigned char *head, unsigned char *tail)
{
    const unsigned char *wb = (const unsigned char *)w;
    memcpy(head, wb, SOC_Q8_0_BLOCK_BYTES);
    memcpy(tail, wb + (size_t)(rows - 1) * stride + (size_t)(nb - 1) * SOC_Q8_0_BLOCK_BYTES,
           SOC_Q8_0_BLOCK_BYTES);
}

static void free_slot(slot_t *s)
{
    if (s->q_handle)
        g_be->free(s->q_handle);
    if (s->s_handle)
        g_be->free(s->s_handle);
    memset(s, 0, sizeof(*s));
}

static int repack_into(slot_t *s)
{
    uint64_t t0 = now_ns();
    int rc = soc_q8r_from_q8_0(&s->mat, s->w, s->stride);
    g_st.repack_ns += now_ns() - t0;
    if (rc == 0)
        fingerprint(s->w, s->stride, s->rows, s->nb, s->head, s->tail);
    return rc;
}

/* Allocate engine memory for a new matrix and repack it. On failure the slot
 * is marked CPU-only so we do not retry every token. */
static void fill_slot(slot_t *s)
{
    size_t qb = soc_q8r_quants_bytes(s->rows, s->nb);
    size_t sb = soc_q8r_scales_bytes(s->rows, s->nb);
    void *qcpu = NULL, *scpu = NULL;

    if (!qb || !sb ||
        g_be->alloc(qb, &qcpu, &s->q_dev, &s->q_handle) != 0 ||
        g_be->alloc(sb, &scpu, &s->s_dev, &s->s_handle) != 0 ||
        soc_q8r_init(&s->mat, s->rows, s->nb, qcpu, qb, scpu, sb) != 0 ||
        repack_into(s) != 0) {
        fprintf(stderr, "soc_gemv: could not cache %u x %u matrix at %p (%zu + %zu B); using CPU for it\n",
                s->rows, s->nb * SOC_QK, s->w, qb, sb);
        if (s->q_handle) { g_be->free(s->q_handle); s->q_handle = NULL; }
        if (s->s_handle) { g_be->free(s->s_handle); s->s_handle = NULL; }
        s->state = SLOT_CPU_ONLY;
        return;
    }
    s->state = SLOT_READY;
    g_st.tensors++;
    g_st.repack_bytes += qb + sb;
}

/* Find or create the slot for (w, stride, rows, nb). NULL if the table is full. */
static slot_t *lookup(const void *w, size_t stride, uint32_t rows, uint32_t nb)
{
    size_t i = hash_ptr(w);
    for (size_t probe = 0; probe < CACHE_SLOTS; probe++, i = (i + 1) & (CACHE_SLOTS - 1)) {
        slot_t *s = &g_slots[i];
        if (s->state == SLOT_EMPTY) {
            s->w = w;
            s->stride = stride;
            s->rows = rows;
            s->nb = nb;
            fill_slot(s);
            return s;
        }
        if (s->w == w && s->stride == stride && s->rows == rows && s->nb == nb) {
            if (s->state == SLOT_READY) {
                unsigned char head[SOC_Q8_0_BLOCK_BYTES], tail[SOC_Q8_0_BLOCK_BYTES];
                fingerprint(w, stride, rows, nb, head, tail);
                if (memcmp(head, s->head, sizeof head) || memcmp(tail, s->tail, sizeof tail)) {
                    /* Same address, different contents: repack in place. */
                    g_st.stale_repacks++;
                    if (repack_into(s) != 0)
                        s->state = SLOT_CPU_ONLY;
                }
            }
            return s;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Verification (caller holds g_lock)                                         */
/* ------------------------------------------------------------------------- */

static void verify(const void *w, size_t stride, uint32_t rows, uint32_t nb,
                   const void *x, const float *y, soc_vec_dot_fn ref)
{
    if (g_ref_cap < rows) {
        float *nb_buf = realloc(g_ref_buf, sizeof(float) * rows);
        if (!nb_buf)
            return;
        g_ref_buf = nb_buf;
        g_ref_cap = rows;
    }
    cpu_gemv(w, stride, rows, nb, x, g_ref_buf, ref);

    double max_diff = 0.0, max_ref = 0.0;
    uint64_t exact = 0;
    for (uint32_t r = 0; r < rows; r++) {
        double d = fabs((double)y[r] - (double)g_ref_buf[r]);
        double a = fabs((double)g_ref_buf[r]);
        if (d > max_diff) max_diff = d;
        if (a > max_ref) max_ref = a;
        if (memcmp(&y[r], &g_ref_buf[r], sizeof(float)) == 0)
            exact++;
    }
    double norm = max_ref > 0 ? max_diff / max_ref : max_diff;

    g_st.verified++;
    g_st.verified_rows += rows;
    g_st.exact_rows += exact;
    if (max_diff > g_st.max_abs_diff) g_st.max_abs_diff = max_diff;
    if (norm > g_st.max_norm_diff) {
        if (norm > 1e-3 && g_st.max_norm_diff <= 1e-3)
            fprintf(stderr, "soc_gemv: WARNING verify: %u x %u GEMV differs by %.3g of max|y|\n",
                    rows, nb * SOC_QK, norm);
        g_st.max_norm_diff = norm;
    }
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                                */
/* ------------------------------------------------------------------------- */

void soc_gemv_q8_0(const void *w, size_t w_row_stride, int64_t rows64, int64_t n64,
                   const void *x, float *y, soc_vec_dot_fn ref_vec_dot)
{
    pthread_once(&g_once, init_once);

    int shape_ok = w && x && y && rows64 > 0 && rows64 <= (int64_t)UINT32_MAX &&
                   n64 > 0 && n64 % SOC_QK == 0 && n64 / SOC_QK <= (int64_t)UINT32_MAX &&
                   w_row_stride >= (size_t)(n64 / SOC_QK) * SOC_Q8_0_BLOCK_BYTES;
    uint32_t rows = shape_ok ? (uint32_t)rows64 : 0;
    uint32_t nb   = shape_ok ? (uint32_t)(n64 / SOC_QK) : 0;

    if (!shape_ok || !g_enabled) {
        if (shape_ok)
            cpu_gemv(w, w_row_stride, rows, nb, x, y, ref_vec_dot);
        pthread_mutex_lock(&g_lock);
        g_st.fallback_calls++;
        pthread_mutex_unlock(&g_lock);
        return;
    }

    /* One engine, one request at a time: hold the lock across run(). */
    pthread_mutex_lock(&g_lock);

    slot_t *s = lookup(w, w_row_stride, rows, nb);
    int rc = -1;
    if (s && s->state == SLOT_READY) {
        uint64_t t0 = now_ns();
        rc = g_be->run(&s->mat, s->q_dev, s->s_dev, x, y);
        uint64_t dt = now_ns() - t0;
        if (rc == 0) {
            g_st.calls++;
            g_st.rows += rows;
            g_st.weight_bytes += (uint64_t)rows * nb * (SOC_QK + 2);
            g_st.backend_ns += dt;
        } else {
            fprintf(stderr, "soc_gemv: backend run failed (%d) for %u x %u\n", rc, rows, nb * SOC_QK);
            if (g_strict)
                abort();
        }
    }

    if (rc != 0) {
        cpu_gemv(w, w_row_stride, rows, nb, x, y, ref_vec_dot);
        g_st.fallback_calls++;
    } else if (g_verify) {
        verify(w, w_row_stride, rows, nb, x, y, ref_vec_dot);
    }

    pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------------------- */
/* Stats and shutdown                                                         */
/* ------------------------------------------------------------------------- */

void soc_gemv_get_stats(soc_gemv_stats *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_st;
    pthread_mutex_unlock(&g_lock);
}

void soc_gemv_print_stats(void)
{
    soc_gemv_stats s;
    soc_gemv_get_stats(&s);
    double bt = s.backend_ns * 1e-9;
    fprintf(stderr,
            "soc_gemv stats: backend=%s\n"
            "  offloaded GEMVs   %llu (%llu rows), CPU fallbacks %llu\n"
            "  weights streamed  %.1f MB in %.3f s backend time (%.2f GB/s)\n"
            "  weight cache      %llu matrices, %.1f MB, repack %.3f s, stale repacks %llu\n",
            g_be ? g_be->name : "none",
            (unsigned long long)s.calls, (unsigned long long)s.rows, (unsigned long long)s.fallback_calls,
            s.weight_bytes / 1e6, bt, bt > 0 ? s.weight_bytes / bt / 1e9 : 0.0,
            (unsigned long long)s.tensors, s.repack_bytes / 1e6, s.repack_ns * 1e-9,
            (unsigned long long)s.stale_repacks);
    if (s.verified)
        fprintf(stderr,
                "  verify            %llu GEMVs, %llu rows, bit-exact rows %.2f%%, "
                "max|diff| %.3g, max diff/max|y| %.3g\n",
                (unsigned long long)s.verified, (unsigned long long)s.verified_rows,
                s.verified_rows ? 100.0 * s.exact_rows / s.verified_rows : 0.0,
                s.max_abs_diff, s.max_norm_diff);
}

void soc_gemv_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_be) {
        for (size_t i = 0; i < CACHE_SLOTS; i++)
            if (g_slots[i].state != SLOT_EMPTY)
                free_slot(&g_slots[i]);
    }
    free(g_ref_buf);
    g_ref_buf = NULL;
    g_ref_cap = 0;
    pthread_mutex_unlock(&g_lock);
}
