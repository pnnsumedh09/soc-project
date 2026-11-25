/*
 * soc_repack.c - compile-time Q4_0/Q8_0 weight repacking for the GEMM
 * accelerator. Activations are Q8_0 for either weight format.
 * See soc_repack.h for the layout and the rationale.
 */
#include "soc_repack.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The FP16 scale is copied as raw little-endian bits. Both the ARM target and
 * typical hosts are little-endian; refuse to build elsewhere. */
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "soc_repack assumes a little-endian host"
#endif

static int fail_einval(void)
{
    errno = EINVAL;
    return -1;
}

/* Round up to SOC_REPACK_ALIGN; returns 0 on overflow. */
static size_t align_up(size_t x)
{
    if (x > SIZE_MAX - (SOC_REPACK_ALIGN - 1))
        return 0;
    return (x + SOC_REPACK_ALIGN - 1) / SOC_REPACK_ALIGN * SOC_REPACK_ALIGN;
}

/* rows*nb*unit with overflow check; 0 on overflow or zero size. */
static size_t mul3(uint32_t rows, uint32_t nb, size_t unit)
{
    size_t n = (size_t)rows * (size_t)nb;           /* fits: 32b*32b in 64b size_t */
    if (rows == 0 || nb == 0 || n > SIZE_MAX / unit)
        return 0;
    return n * unit;
}

size_t soc_q8r_quants_bytes(uint32_t rows, uint32_t nb)
{
    size_t n = mul3(rows, nb, SOC_WEIGHT_QUANT_BYTES);
    return n ? align_up(n) : 0;
}

size_t soc_q8r_scales_bytes(uint32_t rows, uint32_t nb)
{
    size_t n = mul3(rows, nb, sizeof(uint16_t));
    return n ? align_up(n) : 0;
}

int soc_q8r_pack(const void *src, size_t src_row_stride, uint32_t rows, uint32_t nb,
                 int8_t *quants, uint16_t *scales)
{
    if (!src || !quants || !scales || rows == 0 || nb == 0)
        return fail_einval();
    if (src_row_stride < (size_t)nb * SOC_WEIGHT_BLOCK_BYTES)
        return fail_einval();

    const unsigned char *base = (const unsigned char *)src;
    for (uint32_t r = 0; r < rows; r++) {
        const unsigned char *row = base + (size_t)r * src_row_stride;
        size_t out = (size_t)r * nb;                     /* block index of (r, 0) */
        for (uint32_t b = 0; b < nb; b++) {
            const unsigned char *blk = row + (size_t)b * SOC_WEIGHT_BLOCK_BYTES;
            memcpy(&scales[out + b], blk, sizeof(uint16_t));            /* d  */
            memcpy(&quants[(out + b) * SOC_WEIGHT_QUANT_BYTES], blk + 2,
                   SOC_WEIGHT_QUANT_BYTES);                              /* qs */
        }
    }
    return 0;
}

int soc_q8r_unpack(const int8_t *quants, const uint16_t *scales, uint32_t rows, uint32_t nb,
                   void *dst, size_t dst_row_stride)
{
    if (!quants || !scales || !dst || rows == 0 || nb == 0)
        return fail_einval();
    if (dst_row_stride < (size_t)nb * SOC_WEIGHT_BLOCK_BYTES)
        return fail_einval();

    unsigned char *base = (unsigned char *)dst;
    for (uint32_t r = 0; r < rows; r++) {
        unsigned char *row = base + (size_t)r * dst_row_stride;
        size_t in = (size_t)r * nb;
        for (uint32_t b = 0; b < nb; b++) {
            unsigned char *blk = row + (size_t)b * SOC_WEIGHT_BLOCK_BYTES;
            memcpy(blk, &scales[in + b], sizeof(uint16_t));
            memcpy(blk + 2, &quants[(in + b) * SOC_WEIGHT_QUANT_BYTES],
                   SOC_WEIGHT_QUANT_BYTES);
        }
    }
    return 0;
}

int soc_q8r_init(soc_q8r_mat *m, uint32_t rows, uint32_t nb,
                 void *quants_buf, size_t quants_buf_bytes,
                 void *scales_buf, size_t scales_buf_bytes)
{
    size_t qb = soc_q8r_quants_bytes(rows, nb);
    size_t sb = soc_q8r_scales_bytes(rows, nb);

    if (!m || !quants_buf || !scales_buf || qb == 0 || sb == 0)
        return fail_einval();
    if (quants_buf_bytes < qb || scales_buf_bytes < sb)
        return fail_einval();
    if (((uintptr_t)quants_buf % SOC_REPACK_ALIGN) || ((uintptr_t)scales_buf % SOC_REPACK_ALIGN))
        return fail_einval();

    memset(m, 0, sizeof(*m));
    m->rows = rows;
    m->nb = nb;
    m->quants = (int8_t *)quants_buf;
    m->scales = (uint16_t *)scales_buf;
    m->quants_bytes = qb;
    m->scales_bytes = sb;

    /* Zero only the padding; the payload is overwritten by pack. */
    size_t qused = (size_t)rows * nb * SOC_WEIGHT_QUANT_BYTES;
    size_t sused = (size_t)rows * nb * sizeof(uint16_t);
    memset((char *)quants_buf + qused, 0, qb - qused);
    memset((char *)scales_buf + sused, 0, sb - sused);
    return 0;
}

int soc_q8r_alloc_host(soc_q8r_mat *m, uint32_t rows, uint32_t nb)
{
    size_t qb = soc_q8r_quants_bytes(rows, nb);
    size_t sb = soc_q8r_scales_bytes(rows, nb);

    if (!m || qb == 0 || sb == 0 || qb > SIZE_MAX - sb)
        return fail_einval();

    /* One block: quants first, scales right after. qb is a multiple of 4 KB,
     * so the scales array is 4 KB-aligned too. */
    void *mem = aligned_alloc(SOC_REPACK_ALIGN, qb + sb);
    if (!mem)
        return -1;                                       /* errno = ENOMEM */
    memset(mem, 0, qb + sb);

    if (soc_q8r_init(m, rows, nb, mem, qb, (char *)mem + qb, sb) < 0) {
        free(mem);
        return -1;
    }
    m->host_mem = mem;
    return 0;
}

void soc_q8r_free_host(soc_q8r_mat *m)
{
    if (!m)
        return;
    free(m->host_mem);
    memset(m, 0, sizeof(*m));
}

int soc_q8r_from_q8_0(soc_q8r_mat *m, const void *src, size_t src_row_stride)
{
    if (!m || !m->quants || !m->scales)
        return fail_einval();
    return soc_q8r_pack(src, src_row_stride, m->rows, m->nb, m->quants, m->scales);
}

/* ------------------------------------------------------------------------- */
/* Reference arithmetic                                                       */
/* ------------------------------------------------------------------------- */

float soc_fp16_to_fp32(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp  = (h >> 10) & 0x1Fu;
    uint32_t man  = h & 0x3FFu;
    uint32_t out;

    if (exp == 0x1F) {                          /* Inf / NaN */
        out = sign | 0x7F800000u | (man << 13);
    } else if (exp != 0) {                      /* normal: rebias 15 -> 127 */
        out = sign | ((exp + 112) << 23) | (man << 13);
    } else if (man == 0) {                      /* +/- 0 */
        out = sign;
    } else {                                    /* denormal: renormalize */
        uint32_t w = man << 6;                  /* mantissa bit 9 -> bit 15 */
        uint32_t n = 0;
        if (!(w & 0xFF00u)) { n += 8; w <<= 8; }
        if (!(w & 0xF000u)) { n += 4; w <<= 4; }
        if (!(w & 0xC000u)) { n += 2; w <<= 2; }
        if (!(w & 0x8000u)) { n += 1; w <<= 1; }
        out = sign | ((112 - n) << 23) | (((w >> 5) & 0x3FFu) << 13);
    }

    float f;
    memcpy(&f, &out, sizeof f);
    return f;
}

/* One block, in ggml's order. w_qs / x_qs: 32 int8. */
static inline float block_term(const int8_t *w_qs, uint16_t w_d, const int8_t *x_qs, uint16_t x_d)
{
    int sumi = 0;
#if SOC_GEMM_WEIGHT_BITS == 4
    const uint8_t *packed = (const uint8_t *)w_qs;
    for (int j = 0; j < 16; ++j) {
        int w0 = (int)(packed[j] & 0x0f) - 8;
        int w1 = (int)(packed[j] >> 4) - 8;
        sumi += w0 * x_qs[j] + w1 * x_qs[j + 16];
    }
#else
    for (int j = 0; j < SOC_QK; j++)
        sumi += w_qs[j] * x_qs[j];
#endif
    return (float)sumi * (soc_fp16_to_fp32(w_d) * soc_fp16_to_fp32(x_d));
}

static inline uint16_t blk_d(const unsigned char *blk)
{
    uint16_t d;
    memcpy(&d, blk, sizeof d);
    return d;
}

float soc_q8r_row_dot(const soc_q8r_mat *m, uint32_t row, const void *x_q8_0)
{
    const unsigned char *x = (const unsigned char *)x_q8_0;
    size_t base = (size_t)row * m->nb;
    float sumf = 0.0f;

    for (uint32_t b = 0; b < m->nb; b++) {
        const unsigned char *xb = x + (size_t)b * SOC_Q8_0_BLOCK_BYTES;
        sumf += block_term(&m->quants[(base + b) * SOC_WEIGHT_QUANT_BYTES],
                           m->scales[base + b],
                           (const int8_t *)(xb + 2), blk_d(xb));
    }
    return sumf;
}

void soc_q8r_gemv_ref(const soc_q8r_mat *m, const void *x_q8_0, float *y)
{
    for (uint32_t r = 0; r < m->rows; r++)
        y[r] = soc_q8r_row_dot(m, r, x_q8_0);
}

float soc_q8_0_row_dot_native(const void *w_row_q8_0, const void *x_q8_0, uint32_t nb)
{
    const unsigned char *w = (const unsigned char *)w_row_q8_0;
    const unsigned char *x = (const unsigned char *)x_q8_0;
    float sumf = 0.0f;

    for (uint32_t b = 0; b < nb; b++) {
        const unsigned char *wb = w + (size_t)b * SOC_WEIGHT_BLOCK_BYTES;
        const unsigned char *xb = x + (size_t)b * SOC_Q8_0_BLOCK_BYTES;
        sumf += block_term((const int8_t *)(wb + 2), blk_d(wb), (const int8_t *)(xb + 2), blk_d(xb));
    }
    return sumf;
}
