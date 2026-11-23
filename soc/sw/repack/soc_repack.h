#ifndef SOC_REPACK_H
#define SOC_REPACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOC_QK_K             256
#define SOC_Q4_K_BLOCK_BYTES 144
#define SOC_Q4_K_META_BYTES  16
#define SOC_Q4_K_QUANT_BYTES 128
#define SOC_Q8_K_BLOCK_BYTES 292
#define SOC_REPACK_ALIGN     4096
#define SOC_Q4_K_MAX_NB      19
#define SOC_Q4_K_MAX_ROWS    151936

typedef struct soc_q4kr_mat {
    uint32_t  rows;
    uint32_t  nb;
    uint8_t * quants;
    uint8_t * metadata;
    size_t    quants_bytes;
    size_t    metadata_bytes;
    void *    host_mem;
} soc_q4kr_mat;

size_t soc_q4kr_quants_bytes(uint32_t rows, uint32_t nb);
size_t soc_q4kr_metadata_bytes(uint32_t rows, uint32_t nb);
int    soc_q4kr_pack(const void * src,
                     size_t       src_row_stride,
                     uint32_t     rows,
                     uint32_t     nb,
                     uint8_t *    quants,
                     uint8_t *    metadata);
int    soc_q4kr_unpack(const uint8_t * quants,
                       const uint8_t * metadata,
                       uint32_t        rows,
                       uint32_t        nb,
                       void *          dst,
                       size_t          dst_row_stride);
int    soc_q4kr_init(soc_q4kr_mat * m,
                     uint32_t       rows,
                     uint32_t       nb,
                     void *         quants_buf,
                     size_t         quants_buf_bytes,
                     void *         metadata_buf,
                     size_t         metadata_buf_bytes);
int    soc_q4kr_alloc_host(soc_q4kr_mat * m, uint32_t rows, uint32_t nb);
void   soc_q4kr_free_host(soc_q4kr_mat * m);
int    soc_q4kr_from_q4_k(soc_q4kr_mat * m, const void * src, size_t src_row_stride);

float soc_fp16_to_fp32(uint16_t h);
float soc_q4kr_row_dot(const soc_q4kr_mat * m, uint32_t row, const void * x_q8_k);
void  soc_q4kr_gemv_ref(const soc_q4kr_mat * m, const void * x_q8_k, float * y);
float soc_q4_k_row_dot_native(const void * w_row_q4_k, const void * x_q8_k, uint32_t nb);

#ifdef __cplusplus
}
#endif

#endif
