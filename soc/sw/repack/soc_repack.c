#include "soc_repack.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#    include <malloc.h>
#endif

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#    error "soc_repack assumes a little-endian host"
#endif

static int fail_einval(void) {
    errno = EINVAL;
    return -1;
}

static void * aligned_memory_alloc(size_t bytes) {
#ifdef _WIN32
    return _aligned_malloc(bytes, SOC_REPACK_ALIGN);
#else
    return aligned_alloc(SOC_REPACK_ALIGN, bytes);
#endif
}

static void aligned_memory_free(void * memory) {
#ifdef _WIN32
    _aligned_free(memory);
#else
    free(memory);
#endif
}

static size_t align_up(size_t x) {
    if (x > SIZE_MAX - (SOC_REPACK_ALIGN - 1)) {
        return 0;
    }
    return (x + SOC_REPACK_ALIGN - 1) / SOC_REPACK_ALIGN * SOC_REPACK_ALIGN;
}

static size_t payload_bytes(uint32_t rows, uint32_t nb, size_t unit) {
    if (!rows || !nb || (size_t) rows > SIZE_MAX / nb) {
        return 0;
    }
    size_t blocks = (size_t) rows * nb;
    if (blocks > SIZE_MAX / unit) {
        return 0;
    }
    return blocks * unit;
}

size_t soc_q4kr_quants_bytes(uint32_t rows, uint32_t nb) {
    size_t n = payload_bytes(rows, nb, SOC_Q4_K_QUANT_BYTES);
    return n ? align_up(n) : 0;
}

size_t soc_q4kr_metadata_bytes(uint32_t rows, uint32_t nb) {
    size_t n = payload_bytes(rows, nb, SOC_Q4_K_META_BYTES);
    return n ? align_up(n) : 0;
}

int soc_q4kr_pack(const void * src,
                  size_t       src_row_stride,
                  uint32_t     rows,
                  uint32_t     nb,
                  uint8_t *    quants,
                  uint8_t *    metadata) {
    if (!src || !quants || !metadata || !rows || !nb || src_row_stride < (size_t) nb * SOC_Q4_K_BLOCK_BYTES) {
        return fail_einval();
    }
    const uint8_t * base = (const uint8_t *) src;
    for (uint32_t row = 0; row < rows; ++row) {
        const uint8_t * src_row = base + (size_t) row * src_row_stride;
        size_t          out     = (size_t) row * nb;
        for (uint32_t block = 0; block < nb; ++block) {
            const uint8_t * src_block = src_row + (size_t) block * SOC_Q4_K_BLOCK_BYTES;
            memcpy(metadata + (out + block) * SOC_Q4_K_META_BYTES, src_block, SOC_Q4_K_META_BYTES);
            memcpy(quants + (out + block) * SOC_Q4_K_QUANT_BYTES, src_block + SOC_Q4_K_META_BYTES,
                   SOC_Q4_K_QUANT_BYTES);
        }
    }
    return 0;
}

int soc_q4kr_unpack(const uint8_t * quants,
                    const uint8_t * metadata,
                    uint32_t        rows,
                    uint32_t        nb,
                    void *          dst,
                    size_t          dst_row_stride) {
    if (!quants || !metadata || !dst || !rows || !nb || dst_row_stride < (size_t) nb * SOC_Q4_K_BLOCK_BYTES) {
        return fail_einval();
    }
    uint8_t * base = (uint8_t *) dst;
    for (uint32_t row = 0; row < rows; ++row) {
        uint8_t * dst_row = base + (size_t) row * dst_row_stride;
        size_t    in      = (size_t) row * nb;
        for (uint32_t block = 0; block < nb; ++block) {
            uint8_t * dst_block = dst_row + (size_t) block * SOC_Q4_K_BLOCK_BYTES;
            memcpy(dst_block, metadata + (in + block) * SOC_Q4_K_META_BYTES, SOC_Q4_K_META_BYTES);
            memcpy(dst_block + SOC_Q4_K_META_BYTES, quants + (in + block) * SOC_Q4_K_QUANT_BYTES, SOC_Q4_K_QUANT_BYTES);
        }
    }
    return 0;
}

int soc_q4kr_init(soc_q4kr_mat * m,
                  uint32_t       rows,
                  uint32_t       nb,
                  void *         quants_buf,
                  size_t         quants_buf_bytes,
                  void *         metadata_buf,
                  size_t         metadata_buf_bytes) {
    size_t qb = soc_q4kr_quants_bytes(rows, nb);
    size_t mb = soc_q4kr_metadata_bytes(rows, nb);
    if (!m || !quants_buf || !metadata_buf || !qb || !mb || quants_buf_bytes < qb || metadata_buf_bytes < mb ||
        (uintptr_t) quants_buf % SOC_REPACK_ALIGN || (uintptr_t) metadata_buf % SOC_REPACK_ALIGN) {
        return fail_einval();
    }
    memset(m, 0, sizeof(*m));
    m->rows           = rows;
    m->nb             = nb;
    m->quants         = (uint8_t *) quants_buf;
    m->metadata       = (uint8_t *) metadata_buf;
    m->quants_bytes   = qb;
    m->metadata_bytes = mb;
    size_t qused      = (size_t) rows * nb * SOC_Q4_K_QUANT_BYTES;
    size_t mused      = (size_t) rows * nb * SOC_Q4_K_META_BYTES;
    memset((uint8_t *) quants_buf + qused, 0, qb - qused);
    memset((uint8_t *) metadata_buf + mused, 0, mb - mused);
    return 0;
}

int soc_q4kr_alloc_host(soc_q4kr_mat * m, uint32_t rows, uint32_t nb) {
    size_t qb = soc_q4kr_quants_bytes(rows, nb);
    size_t mb = soc_q4kr_metadata_bytes(rows, nb);
    if (!m || !qb || !mb || qb > SIZE_MAX - mb) {
        return fail_einval();
    }
    void * mem = aligned_memory_alloc(qb + mb);
    if (!mem) {
        return -1;
    }
    memset(mem, 0, qb + mb);
    if (soc_q4kr_init(m, rows, nb, mem, qb, (uint8_t *) mem + qb, mb)) {
        aligned_memory_free(mem);
        return -1;
    }
    m->host_mem = mem;
    return 0;
}

void soc_q4kr_free_host(soc_q4kr_mat * m) {
    if (!m) {
        return;
    }
    aligned_memory_free(m->host_mem);
    memset(m, 0, sizeof(*m));
}

int soc_q4kr_from_q4_k(soc_q4kr_mat * m, const void * src, size_t src_row_stride) {
    if (!m || !m->quants || !m->metadata) {
        return fail_einval();
    }
    return soc_q4kr_pack(src, src_row_stride, m->rows, m->nb, m->quants, m->metadata);
}

float soc_fp16_to_fp32(uint16_t h) {
    uint32_t sign = (uint32_t) (h & 0x8000u) << 16;
    uint32_t exp  = (h >> 10) & 0x1fu;
    uint32_t man  = h & 0x3ffu;
    uint32_t out;
    if (exp == 0x1f) {
        out = sign | 0x7f800000u | (man << 13);
    } else if (exp) {
        out = sign | ((exp + 112) << 23) | (man << 13);
    } else if (!man) {
        out = sign;
    } else {
        uint32_t w = man << 6;
        uint32_t n = 0;
        if (!(w & 0xff00u)) {
            n += 8;
            w <<= 8;
        }
        if (!(w & 0xf000u)) {
            n += 4;
            w <<= 4;
        }
        if (!(w & 0xc000u)) {
            n += 2;
            w <<= 2;
        }
        if (!(w & 0x8000u)) {
            n += 1;
            w <<= 1;
        }
        out = sign | ((112 - n) << 23) | (((w >> 5) & 0x3ffu) << 13);
    }
    float result;
    memcpy(&result, &out, sizeof(result));
    return result;
}

static uint16_t load_u16(const uint8_t * p) {
    return (uint16_t) p[0] | (uint16_t) p[1] << 8;
}

static int16_t load_i16(const uint8_t * p) {
    return (int16_t) load_u16(p);
}

static float load_f32(const uint8_t * p) {
    float value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static void scale_min(unsigned group, const uint8_t * packed, uint8_t * scale, uint8_t * min) {
    if (group < 4) {
        *scale = packed[group] & 63;
        *min   = packed[group + 4] & 63;
    } else {
        *scale = (packed[group + 4] & 0x0f) | ((packed[group - 4] >> 6) << 4);
        *min   = (packed[group + 4] >> 4) | ((packed[group] >> 6) << 4);
    }
}

static void block_accumulate(const uint8_t * meta, const uint8_t * q4, const uint8_t * x, float sums[8], float * sumf) {
    const int8_t *  q8       = (const int8_t *) (x + 4);
    const uint8_t * bsums    = x + 4 + SOC_QK_K;
    int32_t         lanes[8] = { 0 };
    int32_t         min_sum  = 0;
    for (unsigned group = 0; group < 8; ++group) {
        uint8_t scale, min;
        scale_min(group, meta + 4, &scale, &min);
        unsigned chunk = group / 2;
        for (unsigned i = 0; i < 32; ++i) {
            uint8_t packed = q4[chunk * 32 + i];
            int32_t weight = group & 1 ? packed >> 4 : packed & 0x0f;
            lanes[i & 7] += (int32_t) scale * weight * q8[group * 32 + i];
        }
        min_sum += min * (load_i16(bsums + group * 4) + load_i16(bsums + group * 4 + 2));
    }
    float xd = load_f32(x);
    float d  = soc_fp16_to_fp32(load_u16(meta)) * xd;
    for (unsigned lane = 0; lane < 8; ++lane) {
        sums[lane] += d * lanes[lane];
    }
    float dmin = soc_fp16_to_fp32(load_u16(meta + 2)) * xd;
    *sumf -= dmin * min_sum;
}

static float finish(const float sums[8], float sumf) {
    for (unsigned lane = 0; lane < 8; ++lane) {
        sumf += sums[lane];
    }
    return sumf;
}

float soc_q4kr_row_dot(const soc_q4kr_mat * m, uint32_t row, const void * x_q8_k) {
    size_t base    = (size_t) row * m->nb;
    float  sums[8] = { 0 };
    float  sumf    = 0.0f;
    for (uint32_t block = 0; block < m->nb; ++block) {
        block_accumulate(m->metadata + (base + block) * SOC_Q4_K_META_BYTES,
                         m->quants + (base + block) * SOC_Q4_K_QUANT_BYTES,
                         (const uint8_t *) x_q8_k + (size_t) block * SOC_Q8_K_BLOCK_BYTES, sums, &sumf);
    }
    return finish(sums, sumf);
}

void soc_q4kr_gemv_ref(const soc_q4kr_mat * m, const void * x_q8_k, float * y) {
    for (uint32_t row = 0; row < m->rows; ++row) {
        y[row] = soc_q4kr_row_dot(m, row, x_q8_k);
    }
}

float soc_q4_k_row_dot_native(const void * w_row_q4_k, const void * x_q8_k, uint32_t nb) {
    const uint8_t * w       = (const uint8_t *) w_row_q4_k;
    const uint8_t * x       = (const uint8_t *) x_q8_k;
    float           sums[8] = { 0 };
    float           sumf    = 0.0f;
    for (uint32_t block = 0; block < nb; ++block) {
        const uint8_t * native = w + (size_t) block * SOC_Q4_K_BLOCK_BYTES;
        block_accumulate(native, native + SOC_Q4_K_META_BYTES, x + (size_t) block * SOC_Q8_K_BLOCK_BYTES, sums, &sumf);
    }
    return finish(sums, sumf);
}
