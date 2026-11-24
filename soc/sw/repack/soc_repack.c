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

static size_t align_up(size_t value) {
    if (value > SIZE_MAX - (SOC_REPACK_ALIGN - 1)) {
        return 0;
    }
    return (value + SOC_REPACK_ALIGN - 1) / SOC_REPACK_ALIGN * SOC_REPACK_ALIGN;
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

size_t soc_q4r_quants_bytes(uint32_t rows, uint32_t nb) {
    size_t bytes = payload_bytes(rows, nb, SOC_Q4_0_QUANT_BYTES);
    return bytes ? align_up(bytes) : 0;
}

size_t soc_q4r_scales_bytes(uint32_t rows, uint32_t nb) {
    size_t bytes = payload_bytes(rows, nb, SOC_Q4_0_SCALE_BYTES);
    return bytes ? align_up(bytes) : 0;
}

int soc_q4r_pack(const void * src,
                 size_t       src_row_stride,
                 uint32_t     rows,
                 uint32_t     nb,
                 uint8_t *    quants,
                 uint16_t *   scales) {
    if (!src || !quants || !scales || !rows || !nb || src_row_stride < (size_t) nb * SOC_Q4_0_BLOCK_BYTES) {
        return fail_einval();
    }
    const uint8_t * base = (const uint8_t *) src;
    for (uint32_t row = 0; row < rows; ++row) {
        const uint8_t * src_row = base + (size_t) row * src_row_stride;
        size_t          out     = (size_t) row * nb;
        for (uint32_t block = 0; block < nb; ++block) {
            const uint8_t * src_block = src_row + (size_t) block * SOC_Q4_0_BLOCK_BYTES;
            scales[out + block]       = (uint16_t) src_block[0] | (uint16_t) src_block[1] << 8;
            memcpy(quants + (out + block) * SOC_Q4_0_QUANT_BYTES, src_block + SOC_Q4_0_SCALE_BYTES,
                   SOC_Q4_0_QUANT_BYTES);
        }
    }
    return 0;
}

int soc_q4r_unpack(const uint8_t *  quants,
                   const uint16_t * scales,
                   uint32_t         rows,
                   uint32_t         nb,
                   void *           dst,
                   size_t           dst_row_stride) {
    if (!quants || !scales || !dst || !rows || !nb || dst_row_stride < (size_t) nb * SOC_Q4_0_BLOCK_BYTES) {
        return fail_einval();
    }
    uint8_t * base = (uint8_t *) dst;
    for (uint32_t row = 0; row < rows; ++row) {
        uint8_t * dst_row = base + (size_t) row * dst_row_stride;
        size_t    in      = (size_t) row * nb;
        for (uint32_t block = 0; block < nb; ++block) {
            uint8_t * dst_block = dst_row + (size_t) block * SOC_Q4_0_BLOCK_BYTES;
            uint16_t  scale     = scales[in + block];
            dst_block[0]        = (uint8_t) scale;
            dst_block[1]        = (uint8_t) (scale >> 8);
            memcpy(dst_block + SOC_Q4_0_SCALE_BYTES, quants + (in + block) * SOC_Q4_0_QUANT_BYTES,
                   SOC_Q4_0_QUANT_BYTES);
        }
    }
    return 0;
}

int soc_q4r_init(soc_q4r_mat * m,
                 uint32_t      rows,
                 uint32_t      nb,
                 void *        quants_buf,
                 size_t        quants_buf_bytes,
                 void *        scales_buf,
                 size_t        scales_buf_bytes) {
    size_t quants_bytes = soc_q4r_quants_bytes(rows, nb);
    size_t scales_bytes = soc_q4r_scales_bytes(rows, nb);
    if (!m || !quants_buf || !scales_buf || !quants_bytes || !scales_bytes || quants_buf_bytes < quants_bytes ||
        scales_buf_bytes < scales_bytes || (uintptr_t) quants_buf % SOC_REPACK_ALIGN ||
        (uintptr_t) scales_buf % SOC_REPACK_ALIGN) {
        return fail_einval();
    }
    memset(m, 0, sizeof(*m));
    m->rows            = rows;
    m->nb              = nb;
    m->quants          = (uint8_t *) quants_buf;
    m->scales          = (uint16_t *) scales_buf;
    m->quants_bytes    = quants_bytes;
    m->scales_bytes    = scales_bytes;
    size_t quants_used = (size_t) rows * nb * SOC_Q4_0_QUANT_BYTES;
    size_t scales_used = (size_t) rows * nb * SOC_Q4_0_SCALE_BYTES;
    memset((uint8_t *) quants_buf + quants_used, 0, quants_bytes - quants_used);
    memset((uint8_t *) scales_buf + scales_used, 0, scales_bytes - scales_used);
    return 0;
}

int soc_q4r_alloc_host(soc_q4r_mat * m, uint32_t rows, uint32_t nb) {
    size_t quants_bytes = soc_q4r_quants_bytes(rows, nb);
    size_t scales_bytes = soc_q4r_scales_bytes(rows, nb);
    if (!m || !quants_bytes || !scales_bytes || quants_bytes > SIZE_MAX - scales_bytes) {
        return fail_einval();
    }
    void * memory = aligned_memory_alloc(quants_bytes + scales_bytes);
    if (!memory) {
        return -1;
    }
    memset(memory, 0, quants_bytes + scales_bytes);
    if (soc_q4r_init(m, rows, nb, memory, quants_bytes, (uint8_t *) memory + quants_bytes, scales_bytes)) {
        aligned_memory_free(memory);
        return -1;
    }
    m->host_mem = memory;
    return 0;
}

void soc_q4r_free_host(soc_q4r_mat * m) {
    if (!m) {
        return;
    }
    aligned_memory_free(m->host_mem);
    memset(m, 0, sizeof(*m));
}

int soc_q4r_from_q4_0(soc_q4r_mat * m, const void * src, size_t src_row_stride) {
    if (!m || !m->quants || !m->scales) {
        return fail_einval();
    }
    return soc_q4r_pack(src, src_row_stride, m->rows, m->nb, m->quants, m->scales);
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
        uint32_t word  = man << 6;
        uint32_t shift = 0;
        if (!(word & 0xff00u)) {
            shift += 8;
            word <<= 8;
        }
        if (!(word & 0xf000u)) {
            shift += 4;
            word <<= 4;
        }
        if (!(word & 0xc000u)) {
            shift += 2;
            word <<= 2;
        }
        if (!(word & 0x8000u)) {
            shift += 1;
            word <<= 1;
        }
        out = sign | ((112 - shift) << 23) | (((word >> 5) & 0x3ffu) << 13);
    }
    float result;
    memcpy(&result, &out, sizeof(result));
    return result;
}

static uint16_t load_u16(const uint8_t * source) {
    return (uint16_t) source[0] | (uint16_t) source[1] << 8;
}

static int32_t block_dot(const uint8_t * quants, const uint8_t * x) {
    const int8_t * x_quants = (const int8_t *) (x + SOC_Q4_0_SCALE_BYTES);
    int32_t        sum_low  = 0;
    int32_t        sum_high = 0;
    for (unsigned i = 0; i < SOC_Q4_0_QUANT_BYTES; ++i) {
        uint8_t packed = quants[i];
        sum_low += ((int32_t) (packed & 0x0f) - 8) * x_quants[i];
        sum_high += ((int32_t) (packed >> 4) - 8) * x_quants[i + SOC_Q4_0_QUANT_BYTES];
    }
    return sum_low + sum_high;
}

static float scaled_dot(int32_t dot, uint16_t weight_scale, const uint8_t * x) {
    return (float) dot * soc_fp16_to_fp32(weight_scale) * soc_fp16_to_fp32(load_u16(x));
}

float soc_q4r_row_dot(const soc_q4r_mat * m, uint32_t row, const void * x_q8_0) {
    size_t          base = (size_t) row * m->nb;
    const uint8_t * x    = (const uint8_t *) x_q8_0;
    float           sum  = 0.0f;
    for (uint32_t block = 0; block < m->nb; ++block) {
        const uint8_t * input = x + (size_t) block * SOC_Q8_0_BLOCK_BYTES;
        sum += scaled_dot(block_dot(m->quants + (base + block) * SOC_Q4_0_QUANT_BYTES, input), m->scales[base + block],
                          input);
    }
    return sum;
}

void soc_q4r_gemv_ref(const soc_q4r_mat * m, const void * x_q8_0, float * y) {
    for (uint32_t row = 0; row < m->rows; ++row) {
        y[row] = soc_q4r_row_dot(m, row, x_q8_0);
    }
}

float soc_q4_0_row_dot_native(const void * w_row_q4_0, const void * x_q8_0, uint32_t nb) {
    const uint8_t * weights = (const uint8_t *) w_row_q4_0;
    const uint8_t * x       = (const uint8_t *) x_q8_0;
    float           sum     = 0.0f;
    for (uint32_t block = 0; block < nb; ++block) {
        const uint8_t * weight = weights + (size_t) block * SOC_Q4_0_BLOCK_BYTES;
        const uint8_t * input  = x + (size_t) block * SOC_Q8_0_BLOCK_BYTES;
        sum += scaled_dot(block_dot(weight + SOC_Q4_0_SCALE_BYTES, input), load_u16(weight), input);
    }
    return sum;
}
