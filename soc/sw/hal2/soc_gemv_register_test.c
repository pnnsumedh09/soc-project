#include "soc_gemv_hal.h"
#include <errno.h>

/* Parameter registers only: no START writes and no DMA submission.
 * Save/restore all tested words; cross-check neighbours to detect aliasing.
 * Requires exclusive engine ownership and a previously validated register RTL.
 */
static void mismatch(soc_gemv_register_failure *f, uint32_t off, uint32_t expected, uint32_t actual) {
    if (f && f->offset==UINT32_MAX) *f=(soc_gemv_register_failure){off,expected,actual};
}
int soc_gemv_hal_register_test(soc_gemv_hal *h, soc_gemv_register_failure *failure) {
    if (failure) *failure=(soc_gemv_register_failure){UINT32_MAX,0,0};
    static const uint32_t offsets[] = {
        0x10,0x14,0x20,0x24,0x30,0x34,0x40,0x44,0x50,0x58
    };
    static const uint32_t fixed[] = {0, UINT32_MAX, 0xaa55aa55, 0x55aa55aa};
    uint32_t saved[10];
    if (!h || !h->io.read32 || !h->io.write32) { errno=EINVAL; return -1; }
    if (h->poisoned) { errno=EIO; return -1; }
    uint32_t ctrl=h->io.read32(h->io.ctx,SOC_GEMV_REG_CTRL);
    if (!(ctrl&SOC_GEMV_IDLE) || (ctrl&(SOC_GEMV_START|SOC_GEMV_AUTO_RESTART))) {
        errno=EBUSY; return -1;
    }
    for (unsigned i=0;i<10;++i) saved[i]=h->io.read32(h->io.ctx,offsets[i]);
    int failed=0;
    for (unsigned target=0;target<10 && !failed;++target) {
        for (unsigned p=0;p<68 && !failed;++p) {
            uint32_t pattern=p<4 ? fixed[p] : p<36 ? (UINT32_C(1)<<(p-4)) : ~(UINT32_C(1)<<(p-36));
            h->io.write32(h->io.ctx,offsets[target],pattern);
            for (unsigned i=0;i<10;++i) {
                uint32_t expected=i==target ? pattern : saved[i];
                uint32_t actual=h->io.read32(h->io.ctx,offsets[i]);
                if (actual!=expected) {
                    mismatch(failure,offsets[i],expected,actual); failed=1;
                }
            }
        }
        h->io.write32(h->io.ctx,offsets[target],saved[target]);
    }
    /* Also restore untouched words after any fault and verify restoration. */
    for (unsigned i=0;i<10;++i) h->io.write32(h->io.ctx,offsets[i],saved[i]);
    for (unsigned i=0;i<10;++i) {
        uint32_t actual=h->io.read32(h->io.ctx,offsets[i]);
        if (actual!=saved[i]) {
            mismatch(failure,offsets[i],saved[i],actual); failed=1;
        }
    }
    ctrl=h->io.read32(h->io.ctx,SOC_GEMV_REG_CTRL);
    if (!(ctrl&SOC_GEMV_IDLE) || (ctrl&(SOC_GEMV_START|SOC_GEMV_AUTO_RESTART))) {
        mismatch(failure,SOC_GEMV_REG_CTRL,SOC_GEMV_IDLE,
                 ctrl&(SOC_GEMV_IDLE|SOC_GEMV_START|SOC_GEMV_AUTO_RESTART)); failed=1;
    }
    if (failed) { errno=EIO; return -1; }
    return 0;
}
