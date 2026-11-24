#ifndef SOC_REPACK_H
#define SOC_REPACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOC_QK4_0            32
#define SOC_Q4_0_BLOCK_BYTES 18
#define SOC_Q4_0_QUANT_BYTES 16
#define SOC_Q4_0_SCALE_BYTES 2
#define SOC_Q8_0_BLOCK_BYTES 34
#define SOC_REPACK_ALIGN     4096
#define SOC_Q4_0_MAX_NB      152
#define SOC_Q4_0_MAX_ROWS    151936

typedef struct soc_q4r_mat {
    uint32_t   rows;
    uint32_t   nb;
    uint8_t *  quants;
    uint16_t * scales;
    size_t     quants_bytes;
    size_t     scales_bytes;
    void *     host_mem;
} soc_q4r_mat;

size_t soc_q4r_quants_bytes(uint32_t rows, uint32_t nb);
size_t soc_q4r_scales_bytes(uint32_t rows, uint32_t nb);
int    soc_q4r_pack(const void * src,
                    size_t       src_row_stride,
                    uint32_t     rows,
                    uint32_t     nb,
                    uint8_t *    quants,
                    uint16_t *   scales);
int    soc_q4r_unpack(const uint8_t *  quants,
                      const uint16_t * scales,
                      uint32_t         rows,
                      uint32_t         nb,
                      void *           dst,
                      size_t           dst_row_stride);
int    soc_q4r_init(soc_q4r_mat * m,
                    uint32_t      rows,
                    uint32_t      nb,
                    void *        quants_buf,
                    size_t        quants_buf_bytes,
                    void *        scales_buf,
                    size_t        scales_buf_bytes);
int    soc_q4r_alloc_host(soc_q4r_mat * m, uint32_t rows, uint32_t nb);
void   soc_q4r_free_host(soc_q4r_mat * m);
int    soc_q4r_from_q4_0(soc_q4r_mat * m, const void * src, size_t src_row_stride);

float soc_fp16_to_fp32(uint16_t h);
float soc_q4r_row_dot(const soc_q4r_mat * m, uint32_t row, const void * x_q8_0);
void  soc_q4r_gemv_ref(const soc_q4r_mat * m, const void * x_q8_0, float * y);
float soc_q4_0_row_dot_native(const void * w_row_q4_0, const void * x_q8_0, uint32_t nb);

#ifdef __cplusplus
}
#endif

#endif
