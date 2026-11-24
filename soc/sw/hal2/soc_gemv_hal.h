#ifndef SOC_GEMV_HAL_H
#define SOC_GEMV_HAL_H
#include <stddef.h>
#include <stdint.h>
#include "soc_repack.h"

/* Matches gemv.cpp and the reviewed HLS register map. Offsets, not PS addresses. */
enum {
    SOC_GEMV_REG_CTRL = 0x00,
    SOC_GEMV_REG_GIE = 0x04,
    SOC_GEMV_REG_W = 0x10,
    SOC_GEMV_REG_S = 0x20,
    SOC_GEMV_REG_X = 0x30,
    SOC_GEMV_REG_Y = 0x40,
    SOC_GEMV_REG_ROWS = 0x50,
    SOC_GEMV_REG_NB = 0x58,
    SOC_GEMV_REG_STATUS = 0x60,
    SOC_GEMV_REG_STATUS_VALID = 0x64,
    SOC_GEMV_REG_BYTES = 0x10000,
    SOC_GEMV_START = 1, SOC_GEMV_DONE = 2, SOC_GEMV_IDLE = 4,
    SOC_GEMV_AUTO_RESTART = 128,
    SOC_GEMV_MAX_ROWS = SOC_Q4_0_MAX_ROWS, SOC_GEMV_MAX_NB = SOC_Q4_0_MAX_NB
};

typedef struct {
    void *ctx;
    uint32_t (*read32)(void *ctx, uint32_t offset);
    void (*write32)(void *ctx, uint32_t offset, uint32_t value);
    uint64_t (*now_ns)(void *ctx);
    /* Platform DMA ownership/order hooks. Noncached adapter uses barriers. */
    void (*to_device)(void *ctx);
    void (*to_cpu)(void *ctx);
} soc_gemv_io;

typedef struct {
    soc_gemv_io io;
    int poisoned; /* Unknown completion: prohibit further jobs until external reset. */
} soc_gemv_hal;

typedef struct {
    uint64_t quants, scales, x, y;
    uint32_t rows, nb;
} soc_gemv_job;

/* Serialized caller; returns 0, or -1 with errno. No retries after a timeout.
 * Buffers must remain mapped/allocated and untouched while DMA may be active. */
int soc_gemv_hal_run(soc_gemv_hal *hal, const soc_gemv_job *job,
                     uint64_t timeout_ns, uint32_t *kernel_status);
#endif
