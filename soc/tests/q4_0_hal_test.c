#include "soc_gemv_hal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t registers[SOC_GEMV_REG_BYTES / sizeof(uint32_t)];
    uint64_t time_ns;
    int      started;
    unsigned to_device_calls;
    unsigned to_cpu_calls;
} mock_device;

static uint32_t mock_read32(void * context, uint32_t offset) {
    mock_device * device = (mock_device *) context;
    if (offset == SOC_GEMV_REG_CTRL) {
        return device->started ? SOC_GEMV_DONE | SOC_GEMV_IDLE : SOC_GEMV_IDLE;
    }
    if (offset == SOC_GEMV_REG_STATUS_VALID) {
        return 1;
    }
    return device->registers[offset / sizeof(uint32_t)];
}

static void mock_write32(void * context, uint32_t offset, uint32_t value) {
    mock_device * device                         = (mock_device *) context;
    device->registers[offset / sizeof(uint32_t)] = value;
    if (offset == SOC_GEMV_REG_CTRL && (value & SOC_GEMV_START)) {
        device->started = 1;
    }
}

static uint64_t mock_now_ns(void * context) {
    mock_device * device = (mock_device *) context;
    device->time_ns += 10;
    return device->time_ns;
}

static void mock_to_device(void * context) {
    ((mock_device *) context)->to_device_calls++;
}

static void mock_to_cpu(void * context) {
    ((mock_device *) context)->to_cpu_calls++;
}

static uint64_t read_address(const mock_device * device, uint32_t offset) {
    return device->registers[offset / 4] | (uint64_t) device->registers[offset / 4 + 1] << 32;
}

int main(void) {
    mock_device device;
    memset(&device, 0, sizeof(device));
    soc_gemv_hal hal = {
        { &device, mock_read32, mock_write32, mock_now_ns, mock_to_device, mock_to_cpu },
        0
    };
    soc_gemv_job job = { 0x10000000, 0x20000000, 0x30000000, 0x40000000, 7, 5 };
    uint32_t     status;
    if (soc_gemv_hal_run(&hal, &job, 1000, &status) || status != 0 || device.to_device_calls != 1 ||
        device.to_cpu_calls != 1 || read_address(&device, SOC_GEMV_REG_W) != job.quants ||
        read_address(&device, SOC_GEMV_REG_S) != job.scales || read_address(&device, SOC_GEMV_REG_X) != job.x ||
        read_address(&device, SOC_GEMV_REG_Y) != job.y || device.registers[SOC_GEMV_REG_ROWS / 4] != job.rows ||
        device.registers[SOC_GEMV_REG_NB / 4] != job.nb) {
        fprintf(stderr, "Q4_0 HAL programming failed\n");
        return 1;
    }

    device.started = 0;
    job.scales++;
    errno = 0;
    if (soc_gemv_hal_run(&hal, &job, 1000, &status) == 0 || errno != EINVAL) {
        fprintf(stderr, "Q4_0 HAL scale alignment check failed\n");
        return 1;
    }

    puts("Q4_0 HAL: PASS");
    return 0;
}
