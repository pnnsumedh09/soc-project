/*
 * soc_gemv.h - GEMV offload entry point called from llama.cpp (plan task P2.2)
 *
 * The hook in ggml/src/ggml-cpu/ggml-cpu.c (ggml_compute_forward_mul_mat)
 * calls soc_gemv_q8_0() for every decode matrix-vector product whose weights
 * are a Q8_0 model tensor. This layer:
 *   - keeps a cache of repacked weight matrices, keyed by the weight pointer
 *     (repack happens on first use; the HAL/backend provides the memory),
 *   - sends the work to the selected backend ("emul" now, FPGA later),
 *   - falls back to llama.cpp's own vec_dot on any failure,
 *   - optionally verifies results against vec_dot and prints statistics.
 *
 * It does not include any ggml header; the hook passes plain pointers.
 *
 * Runtime environment variables (read once):
 *   SOC_GEMV=0            disable offload (everything runs on llama.cpp's CPU path)
 *   SOC_GEMV_BACKEND=emul backend name (default emul)
 *   SOC_GEMV_VERIFY=1     recompute every offloaded GEMV with llama.cpp's vec_dot and
 *                         record the difference (slow; for testing)
 *   SOC_GEMV_STATS=1      print statistics to stderr at exit
 *   SOC_GEMV_PROFILE=PATH write one CSV row per GEMV (profiling overhead applies)
 *   SOC_GEMV_STRICT=1     abort instead of falling back to the CPU on backend errors
 */
#ifndef SOC_GEMV_H
#define SOC_GEMV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same signature as ggml_vec_dot_t (ggml/include/ggml-cpu.h). */
typedef void (*soc_vec_dot_fn)(int n, float *s, size_t bs, const void *x, size_t bx,
                               const void *y, size_t by, int nrc);

/* 1 if offload is compiled in, enabled by the environment and the backend
 * initialized. Thread-safe; the answer never changes after the first call,
 * so every compute thread takes the same branch. */
int soc_gemv_enabled(void);

/*
 * y = W x for one Q8_0 weight matrix. Always produces y (offloaded, or on the
 * CPU via ref_vec_dot if anything fails).
 *   w            first byte of row 0 (tensor->data), native Q8_0 blocks
 *   w_row_stride bytes between rows (tensor->nb[1])
 *   rows         output rows (tensor->ne[1])
 *   n            input length (tensor->ne[0]), multiple of 32
 *   x            n/32 native Q8_0 blocks (the quantized activation row)
 *   y            rows floats
 *   ref_vec_dot  llama.cpp's Q8_0 vec_dot (fallback and verification); may be NULL
 */
void soc_gemv_q8_0(const void *w, size_t w_row_stride, int64_t rows, int64_t n,
                   const void *x, float *y, soc_vec_dot_fn ref_vec_dot);

typedef struct soc_gemv_stats {
    uint64_t calls;            /* offloaded GEMVs (backend run succeeded) */
    uint64_t rows;             /* rows computed by the backend            */
    uint64_t weight_bytes;     /* repacked weight bytes the backend read  */
    uint64_t activation_bytes; /* native Q8_0 activation bytes read       */
    uint64_t output_bytes;     /* FP32 output bytes written               */
    uint64_t macs;             /* logical multiply-accumulate operations  */
    uint64_t backend_ns;       /* time inside backend run()               */
    uint64_t backend_ns_min;
    uint64_t backend_ns_max;
    uint64_t fallback_calls;   /* GEMVs computed on the CPU instead        */
    uint64_t fallback_ns;      /* time in the measured CPU fallback       */
    uint64_t cache_hits;       /* calls using an existing weight entry    */
    uint64_t tensors;          /* weight matrices in the cache             */
    uint64_t repack_bytes;     /* bytes allocated for repacked weights     */
    uint64_t repack_ns;        /* time spent repacking                     */
    uint64_t stale_repacks;    /* cache hits whose weight bytes had changed */
    uint64_t verified;         /* GEMVs checked against ref_vec_dot        */
    uint64_t verified_rows;
    uint64_t exact_rows;       /* rows bit-identical to ref_vec_dot        */
    double   max_abs_diff;     /* max |y - y_ref|                          */
    double   max_norm_diff;    /* max over GEMVs of max|y-y_ref| / max|y_ref| */
} soc_gemv_stats;

void soc_gemv_get_stats(soc_gemv_stats *out);
void soc_gemv_print_stats(void);

/* Free all cached matrices (also called at exit). */
void soc_gemv_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SOC_GEMV_H */
