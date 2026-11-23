#ifndef SOC_GEMV_HAL_H
#define SOC_GEMV_HAL_H
#include <stddef.h>
#include <stdint.h>

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
    SOC_GEMV_REG_READ_BYTES = 0x70,
    SOC_GEMV_REG_READ_BYTES_VALID = 0x78,
    SOC_GEMV_REG_WRITE_BYTES = 0x80,
    SOC_GEMV_REG_WRITE_BYTES_VALID = 0x88,
    SOC_GEMV_REG_MACS = 0x90,
    SOC_GEMV_REG_MACS_VALID = 0x98,
    SOC_GEMV_REG_TIMER_TCSR = 0x1000,
    SOC_GEMV_REG_TIMER_TLR = 0x1004,
    SOC_GEMV_REG_TIMER_TCR = 0x1008,
    SOC_GEMV_REG_BYTES = 0x10000,
    SOC_GEMV_START = 1, SOC_GEMV_DONE = 2, SOC_GEMV_IDLE = 4,
    SOC_GEMV_AUTO_RESTART = 128,
    SOC_GEMV_MAX_ROWS = 151936, SOC_GEMV_MAX_NB = 152,
    SOC_GEMV_MAX_CHAIN_JOBS = 256
};

enum {
    SOC_GEMV_TIMER_LOAD = 0x20,
    SOC_GEMV_TIMER_ENABLE = 0x80,
    SOC_GEMV_TIMER_OVERFLOW = 0x100
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

typedef struct {
    uint64_t pl_cycles;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t macs;
} soc_gemv_hw_counters;

typedef struct {
    uint64_t elapsed_ns;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t macs;
    uint64_t pl_cycles;
    uint32_t kernel_status;
    int32_t error_code;
} soc_gemv_job_result;

typedef struct {
    uint64_t elapsed_ns;
    uint64_t weight_bytes;
    uint64_t activation_bytes;
    uint64_t activation_unique_bytes;
    uint64_t output_bytes;
    uint64_t macs;
    uint32_t completed;
    uint32_t failed_index;
    uint32_t kernel_status;
    int32_t error_code;
} soc_gemv_chain_result;

typedef struct {
    uint32_t offset, expected, actual; /* offset=UINT32_MAX when no word mismatch */
} soc_gemv_register_failure;

/* Destructive parameter readback test with save/restore; never starts a job.
 * Exclusive idle engine required. Does not validate DMA or internal BRAM. */
int soc_gemv_hal_register_test(soc_gemv_hal *hal, soc_gemv_register_failure *failure);

/* Serialized caller; returns 0, or -1 with errno. No retries after a timeout.
 * Buffers must remain mapped/allocated and untouched while DMA may be active. */
int soc_gemv_hal_run(soc_gemv_hal *hal, const soc_gemv_job *job,
                     uint64_t timeout_ns, uint32_t *kernel_status);
int soc_gemv_hal_run_profiled(soc_gemv_hal *hal, const soc_gemv_job *job,
                              uint64_t timeout_ns, uint32_t *kernel_status,
                              soc_gemv_hw_counters *counters);

/* Compatibility chain for the scalar engine. It uses one hardware start per
 * job. Byte/MAC totals include attempted jobs, not observed AXI traffic. */
int soc_gemv_hal_run_chain(soc_gemv_hal *hal, const soc_gemv_job *jobs,
                           uint32_t count, uint64_t timeout_ns,
                           soc_gemv_job_result *results,
                           soc_gemv_chain_result *summary);
#endif
