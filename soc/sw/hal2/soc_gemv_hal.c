#include "soc_gemv_hal.h"
#include <errno.h>

static int fail(int e) { errno = e; return -1; }
static int overlap(uint64_t a, uint64_t n, uint64_t b, uint64_t m) {
    return a < b + m && b < a + n;
}
static void address(soc_gemv_hal *h, uint32_t reg, uint64_t value) {
    h->io.write32(h->io.ctx, reg, (uint32_t)value);
    h->io.write32(h->io.ctx, reg + 4, (uint32_t)(value >> 32));
}
int soc_gemv_hal_run(soc_gemv_hal *h, const soc_gemv_job *j,
                     uint64_t timeout, uint32_t *status) {
    if (!h || !j || !status || !h->io.read32 || !h->io.write32 ||
        !h->io.now_ns || !h->io.to_device || !h->io.to_cpu || !timeout)
        return fail(EINVAL);
    *status = UINT32_MAX;
    if (h->poisoned) return fail(EIO);
    if (!j->rows || j->rows > SOC_GEMV_MAX_ROWS || !j->nb || j->nb > SOC_GEMV_MAX_NB ||
        !j->n || j->n > SOC_GEMV_MAX_N)
        return fail(EINVAL);
    uint64_t a[] = {j->quants, j->scales, j->x, j->y};
    uint64_t n[] = {(uint64_t)j->rows * j->nb * 32,
                    (uint64_t)j->rows * j->nb * 2,
                    (uint64_t)j->n * j->nb * 34,
                    (uint64_t)j->n * j->rows * 4};
    const unsigned align[] = {16, 2, 1, 4};
    for (unsigned i = 0; i < 4; ++i) {
        if (a[i] % align[i] || a[i] > UINT64_MAX - n[i]) return fail(EINVAL);
        for (unsigned k = 0; k < i; ++k)
            if (overlap(a[i], n[i], a[k], n[k])) return fail(EINVAL);
    }
    uint32_t ctrl = h->io.read32(h->io.ctx, SOC_GEMV_REG_CTRL);
    if (!(ctrl & SOC_GEMV_IDLE) || (ctrl & (SOC_GEMV_AUTO_RESTART | SOC_GEMV_START)))
        return fail(EBUSY);
    /* Reading CTRL clears stale DONE; reading VALID clears stale output-valid. */
    (void)h->io.read32(h->io.ctx, SOC_GEMV_REG_STATUS_VALID);
    h->io.write32(h->io.ctx, SOC_GEMV_REG_GIE, 0); /* Polling, no interrupts. */
    address(h, SOC_GEMV_REG_W, j->quants);
    address(h, SOC_GEMV_REG_S, j->scales);
    address(h, SOC_GEMV_REG_X, j->x);
    address(h, SOC_GEMV_REG_Y, j->y);
    h->io.write32(h->io.ctx, SOC_GEMV_REG_ROWS, j->rows);
    h->io.write32(h->io.ctx, SOC_GEMV_REG_NB, j->nb);
    h->io.write32(h->io.ctx, SOC_GEMV_REG_N, j->n);
    h->io.to_device(h->io.ctx);
    uint64_t start = h->io.now_ns(h->io.ctx);
    h->io.write32(h->io.ctx, SOC_GEMV_REG_CTRL, SOC_GEMV_START);
    int done = 0;
    for (;;) {
        ctrl = h->io.read32(h->io.ctx, SOC_GEMV_REG_CTRL);
        done |= !!(ctrl & SOC_GEMV_DONE);
        if (done && (ctrl & SOC_GEMV_IDLE)) break;
        if (h->io.now_ns(h->io.ctx) - start >= timeout) {
            h->poisoned = 1;
            return fail(ETIMEDOUT);
        }
    }
    h->io.to_cpu(h->io.ctx);
    if (!(h->io.read32(h->io.ctx, SOC_GEMV_REG_STATUS_VALID) & 1)) {
        h->poisoned = 1;
        return fail(EPROTO);
    }
    *status = h->io.read32(h->io.ctx, SOC_GEMV_REG_STATUS);
    return *status == 0 ? 0 : fail(EDOM);
}
