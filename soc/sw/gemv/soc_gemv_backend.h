/*
 * soc_gemv_backend.h - backend interface behind soc_gemv (plan tasks P2.3 / P4.2)
 *
 * A backend provides two things:
 *   1. memory the engine can read (4 KB aligned; physically contiguous for
 *      the FPGA), returned as a CPU pointer plus a device address, and
 *   2. run(): y = W x for one repacked matrix.
 *
 * "emul" (soc_gemv_backend_emul.c) does both on the CPU: heap memory and the
 * golden arithmetic from soc_repack. The optional FPGA backend (HAL v2, P4.2) implements
 * the same five functions with contiguous DMA memory, register programming,
 * start and wait; board support and execution still require validation.
 */
#ifndef SOC_GEMV_BACKEND_H
#define SOC_GEMV_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "soc_repack.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct soc_gemv_backend {
    const char *name;

    /* Called once before first use. 0 = OK. */
    int  (*init)(void);
    void (*shutdown)(void);

    /* Allocate `bytes` (a multiple of SOC_REPACK_ALIGN) of engine-readable,
     * SOC_REPACK_ALIGN-aligned memory. Returns 0 and fills cpu (CPU-visible
     * pointer), dev_addr (address the engine uses; physical address on the
     * board) and handle (opaque, passed back to free). */
    int  (*alloc)(size_t bytes, void **cpu, uint64_t *dev_addr, void **handle);
    void (*free)(void *handle);

    /* y[0..m->rows) = W x. q_dev / metadata_dev are the device addresses of
     * m->quants / m->metadata. x_q8_k holds m->nb native Q8_K blocks.
     * Returns 0 on success; nonzero makes the caller fall back to the CPU. */
    int  (*run)(const soc_q4kr_mat *m, uint64_t q_dev, uint64_t metadata_dev,
                const void *x_q8_k, float *y);
} soc_gemv_backend;

extern const soc_gemv_backend soc_gemv_backend_emul;
#ifdef SOC_GEMV_WITH_FPGA
extern const soc_gemv_backend soc_gemv_backend_fpga;
#endif

#ifdef __cplusplus
}
#endif

#endif /* SOC_GEMV_BACKEND_H */
