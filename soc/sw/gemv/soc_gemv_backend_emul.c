/*
 * soc_gemv_backend_emul.c - CPU emulation backend (plan task P2.3)
 *
 * Heap memory stands in for contiguous DMA memory, and run() is the golden
 * arithmetic (soc_q8r_gemv_ref): same data layout and operation order the
 * hardware engine must implement. Used to validate the hook, the repack and
 * the weight cache end to end before any FPGA exists.
 */
#include "soc_gemv_backend.h"

#include <stdlib.h>

static int emul_init(void)
{
    return 0;
}

static void emul_shutdown(void)
{
}

static int emul_alloc(size_t bytes, void **cpu, uint64_t *dev_addr, void **handle)
{
    if (bytes == 0 || bytes % SOC_REPACK_ALIGN)
        return -1;
    void *p = aligned_alloc(SOC_REPACK_ALIGN, bytes);
    if (!p)
        return -1;
    *cpu = p;
    *dev_addr = (uint64_t)(uintptr_t)p;   /* no separate device address space */
    *handle = p;
    return 0;
}

static void emul_free(void *handle)
{
    free(handle);
}

static int emul_run(const soc_q8r_mat *m, uint64_t q_dev, uint64_t s_dev,
                    const void *x_q8_0, float *y)
{
    (void)q_dev;
    (void)s_dev;
    soc_q8r_gemv_ref(m, x_q8_0, y);
    return 0;
}

const soc_gemv_backend soc_gemv_backend_emul = {
    .name     = "emul",
    .init     = emul_init,
    .shutdown = emul_shutdown,
    .alloc    = emul_alloc,
    .free     = emul_free,
    .run      = emul_run,
};
