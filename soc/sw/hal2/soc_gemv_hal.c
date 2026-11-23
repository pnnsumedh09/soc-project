#include "soc_gemv_hal.h"
#include <errno.h>

static int fail(int e) { errno = e; return -1; }
static int overlap(uint64_t a, uint64_t n, uint64_t b, uint64_t m) {
    return a < b + m && b < a + n;
}
static int validate_job(const soc_gemv_job *j) {
    if (!j || !j->rows || j->rows > SOC_GEMV_MAX_ROWS || !j->nb || j->nb > SOC_GEMV_MAX_NB)
        return fail(EINVAL);
    uint64_t a[] = {j->quants, j->scales, j->x, j->y};
    uint64_t n[] = {(uint64_t)j->rows * j->nb * 32,
                    (uint64_t)j->rows * j->nb * 2, (uint64_t)j->nb * 34,
                    (uint64_t)j->rows * 4};
    const unsigned align[] = {16, 2, 1, 4};
    for (unsigned i = 0; i < 4; ++i) {
        if (a[i] % align[i] || a[i] > UINT64_MAX - n[i]) return fail(EINVAL);
        for (unsigned k = 0; k < i; ++k)
            if (overlap(a[i], n[i], a[k], n[k])) return fail(EINVAL);
    }
    return 0;
}
static void address(soc_gemv_hal *h, uint32_t reg, uint64_t value) {
    h->io.write32(h->io.ctx, reg, (uint32_t)value);
    h->io.write32(h->io.ctx, reg + 4, (uint32_t)(value >> 32));
}
static uint64_t read64(soc_gemv_hal *h, uint32_t reg) {
    uint64_t low = h->io.read32(h->io.ctx, reg);
    return low | ((uint64_t)h->io.read32(h->io.ctx, reg + 4) << 32);
}
static void timer_start(soc_gemv_hal *h) {
    h->io.write32(h->io.ctx, SOC_GEMV_REG_TIMER_TCSR, SOC_GEMV_TIMER_OVERFLOW);
    h->io.write32(h->io.ctx, SOC_GEMV_REG_TIMER_TLR, 0);
    h->io.write32(h->io.ctx, SOC_GEMV_REG_TIMER_TCSR, SOC_GEMV_TIMER_LOAD);
    h->io.write32(h->io.ctx, SOC_GEMV_REG_TIMER_TCSR, SOC_GEMV_TIMER_ENABLE);
}
static int timer_stop(soc_gemv_hal *h, uint64_t *cycles) {
    h->io.write32(h->io.ctx, SOC_GEMV_REG_TIMER_TCSR, 0);
    uint32_t control = h->io.read32(h->io.ctx, SOC_GEMV_REG_TIMER_TCSR);
    uint32_t count = h->io.read32(h->io.ctx, SOC_GEMV_REG_TIMER_TCR);
    if (control & SOC_GEMV_TIMER_OVERFLOW) return fail(EOVERFLOW);
    *cycles = count;
    return 0;
}
int soc_gemv_hal_run_profiled(soc_gemv_hal *h, const soc_gemv_job *j,
                              uint64_t timeout, uint32_t *status,
                              soc_gemv_hw_counters *counters) {
    if (!h || !j || !status || !counters || !h->io.read32 || !h->io.write32 ||
        !h->io.now_ns || !h->io.to_device || !h->io.to_cpu || !timeout)
        return fail(EINVAL);
    *status = UINT32_MAX;
    *counters = (soc_gemv_hw_counters){0};
    if (h->poisoned) return fail(EIO);
    if (validate_job(j)) return -1;
    uint32_t ctrl = h->io.read32(h->io.ctx, SOC_GEMV_REG_CTRL);
    if (!(ctrl & SOC_GEMV_IDLE) || (ctrl & (SOC_GEMV_AUTO_RESTART | SOC_GEMV_START)))
        return fail(EBUSY);
    /* Reading CTRL clears stale DONE; reading VALID clears stale output-valid. */
    (void)h->io.read32(h->io.ctx, SOC_GEMV_REG_STATUS_VALID);
    (void)h->io.read32(h->io.ctx, SOC_GEMV_REG_READ_BYTES_VALID);
    (void)h->io.read32(h->io.ctx, SOC_GEMV_REG_WRITE_BYTES_VALID);
    (void)h->io.read32(h->io.ctx, SOC_GEMV_REG_MACS_VALID);
    h->io.write32(h->io.ctx, SOC_GEMV_REG_GIE, 0); /* Polling, no interrupts. */
    address(h, SOC_GEMV_REG_W, j->quants);
    address(h, SOC_GEMV_REG_S, j->scales);
    address(h, SOC_GEMV_REG_X, j->x);
    address(h, SOC_GEMV_REG_Y, j->y);
    h->io.write32(h->io.ctx, SOC_GEMV_REG_ROWS, j->rows);
    h->io.write32(h->io.ctx, SOC_GEMV_REG_NB, j->nb);
    h->io.to_device(h->io.ctx);
    timer_start(h);
    uint64_t start = h->io.now_ns(h->io.ctx);
    h->io.write32(h->io.ctx, SOC_GEMV_REG_CTRL, SOC_GEMV_START);
    int done = 0;
    for (;;) {
        ctrl = h->io.read32(h->io.ctx, SOC_GEMV_REG_CTRL);
        done |= !!(ctrl & SOC_GEMV_DONE);
        if (done && (ctrl & SOC_GEMV_IDLE)) break;
        if (h->io.now_ns(h->io.ctx) - start >= timeout) {
            h->io.write32(h->io.ctx, SOC_GEMV_REG_TIMER_TCSR, 0);
            h->poisoned = 1;
            return fail(ETIMEDOUT);
        }
    }
    if (timer_stop(h, &counters->pl_cycles)) {
        h->poisoned = 1;
        return -1;
    }
    h->io.to_cpu(h->io.ctx);
    uint32_t valid = h->io.read32(h->io.ctx, SOC_GEMV_REG_STATUS_VALID) & 1;
    valid &= h->io.read32(h->io.ctx, SOC_GEMV_REG_READ_BYTES_VALID) & 1;
    valid &= h->io.read32(h->io.ctx, SOC_GEMV_REG_WRITE_BYTES_VALID) & 1;
    valid &= h->io.read32(h->io.ctx, SOC_GEMV_REG_MACS_VALID) & 1;
    if (!valid) {
        h->poisoned = 1;
        return fail(EPROTO);
    }
    *status = h->io.read32(h->io.ctx, SOC_GEMV_REG_STATUS);
    counters->read_bytes = read64(h, SOC_GEMV_REG_READ_BYTES);
    counters->write_bytes = read64(h, SOC_GEMV_REG_WRITE_BYTES);
    counters->macs = read64(h, SOC_GEMV_REG_MACS);
    if (*status == 0 &&
        (counters->read_bytes != (uint64_t)j->rows * j->nb * 34 + (uint64_t)j->nb * 34 ||
         counters->write_bytes != (uint64_t)j->rows * 4 ||
         counters->macs != (uint64_t)j->rows * j->nb * 32)) {
        h->poisoned = 1;
        return fail(EPROTO);
    }
    return *status == 0 ? 0 : fail(EDOM);
}

int soc_gemv_hal_run(soc_gemv_hal *h, const soc_gemv_job *j,
                     uint64_t timeout, uint32_t *status) {
    soc_gemv_hw_counters counters;
    return soc_gemv_hal_run_profiled(h, j, timeout, status, &counters);
}

int soc_gemv_hal_run_chain(soc_gemv_hal *h, const soc_gemv_job *jobs,
                           uint32_t count, uint64_t timeout,
                           soc_gemv_job_result *results,
                           soc_gemv_chain_result *summary) {
    if (!h || !jobs || !results || !summary || !count ||
        count > SOC_GEMV_MAX_CHAIN_JOBS || !timeout || !h->io.read32 ||
        !h->io.write32 || !h->io.now_ns || !h->io.to_device || !h->io.to_cpu)
        return fail(EINVAL);
    *summary = (soc_gemv_chain_result){0};
    summary->failed_index = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        results[i] = (soc_gemv_job_result){0};
        results[i].kernel_status = UINT32_MAX;
    }
    if (h->poisoned) {
        summary->failed_index = 0;
        summary->error_code = EIO;
        results[0].error_code = EIO;
        return fail(EIO);
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (validate_job(&jobs[i])) {
            summary->failed_index = i;
            summary->error_code = errno;
            results[i].error_code = errno;
            return -1;
        }
    }

    uint64_t chain_start = h->io.now_ns(h->io.ctx);
    uint64_t previous_x = UINT64_MAX;
    uint32_t previous_nb = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t elapsed = h->io.now_ns(h->io.ctx) - chain_start;
        if (elapsed >= timeout) {
            summary->failed_index = i;
            summary->error_code = ETIMEDOUT;
            results[i].error_code = ETIMEDOUT;
            summary->elapsed_ns = elapsed;
            return fail(ETIMEDOUT);
        }
        uint64_t job_start = h->io.now_ns(h->io.ctx);
        uint32_t status = UINT32_MAX;
        soc_gemv_hw_counters counters;
        int rc = soc_gemv_hal_run_profiled(h, &jobs[i], timeout - elapsed, &status, &counters);
        int saved = errno;
        uint64_t job_elapsed = h->io.now_ns(h->io.ctx) - job_start;
        uint64_t weights = (uint64_t)jobs[i].rows * jobs[i].nb * 34;
        uint64_t activation = (uint64_t)jobs[i].nb * 34;
        uint64_t output = (uint64_t)jobs[i].rows * 4;
        uint64_t macs = (uint64_t)jobs[i].rows * jobs[i].nb * 32;
        results[i].elapsed_ns = job_elapsed;
        results[i].read_bytes = weights + activation;
        results[i].write_bytes = output;
        results[i].macs = macs;
        results[i].pl_cycles = counters.pl_cycles;
        results[i].kernel_status = status;
        results[i].error_code = rc ? saved : 0;
        summary->weight_bytes += weights;
        summary->activation_bytes += activation;
        if (jobs[i].x != previous_x || jobs[i].nb != previous_nb)
            summary->activation_unique_bytes += activation;
        summary->output_bytes += output;
        summary->macs += macs;
        previous_x = jobs[i].x;
        previous_nb = jobs[i].nb;
        if (rc) {
            summary->failed_index = i;
            summary->kernel_status = status;
            summary->error_code = saved;
            summary->elapsed_ns = h->io.now_ns(h->io.ctx) - chain_start;
            errno = saved;
            return -1;
        }
        ++summary->completed;
    }
    summary->elapsed_ns = h->io.now_ns(h->io.ctx) - chain_start;
    return 0;
}
