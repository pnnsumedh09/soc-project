#include "soc_gemv_hal.h"
#include "soc_gemv_arena.h"
#include "soc_repack.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t regs[32];
    unsigned writes, reads, polls, starts, device_sync, cpu_sync;
    int running, timeout, no_valid, delay_idle;
    uint32_t result;
    uint64_t now;
} fake;
static uint32_t rd(void *p, uint32_t off) {
    fake *f = p; ++f->reads;
    if (off == SOC_GEMV_REG_CTRL && f->running && !f->timeout) {
        ++f->polls;
        if (f->polls == 2) {
            f->regs[SOC_GEMV_REG_CTRL/4] = SOC_GEMV_DONE | (f->delay_idle ? 0 : SOC_GEMV_IDLE);
            f->regs[SOC_GEMV_REG_STATUS/4] = f->result;
            f->regs[SOC_GEMV_REG_STATUS_VALID/4] = !f->no_valid;
        }
        if (f->delay_idle && f->polls == 4) f->regs[SOC_GEMV_REG_CTRL/4] |= SOC_GEMV_IDLE;
    }
    uint32_t value = f->regs[off/4];
    if (off == SOC_GEMV_REG_CTRL) f->regs[off/4] &= ~SOC_GEMV_DONE;
    if (off == SOC_GEMV_REG_STATUS_VALID) f->regs[off/4] = 0;
    return value;
}
static void wr(void *p, uint32_t off, uint32_t v) {
    fake *f = p; ++f->writes; f->regs[off/4] = v;
    if (off == SOC_GEMV_REG_CTRL && v == SOC_GEMV_START) {
        assert(f->device_sync == f->starts + 1);
        ++f->starts; f->running = 1; f->polls = 0; f->regs[off/4] = 0;
    }
}
static uint64_t now(void *p) { fake *f = p; f->now += 10; return f->now; }
static void device(void *p) { ++((fake *)p)->device_sync; }
static void cpu(void *p) { ++((fake *)p)->cpu_sync; }
static soc_gemv_hal init(fake *f) {
    memset(f, 0, sizeof(*f)); f->regs[0] = SOC_GEMV_IDLE | SOC_GEMV_DONE;
    f->regs[SOC_GEMV_REG_STATUS_VALID/4] = 1; // stale valid must be cleared
    return (soc_gemv_hal){{f, rd, wr, now, device, cpu}, 0};
}
static void test_control(void) {
    fake f; soc_gemv_hal h = init(&f);
    soc_gemv_job j = {0x100001000ull, 0x200002000ull, 0x300003000ull, 0x400004000ull, 3, 2};
    uint32_t status;
    assert(!soc_gemv_hal_run(&h, &j, 1000, &status) && status == 0);
    assert(f.starts == 1 && f.polls >= 2 && f.cpu_sync == 1);
    assert(f.regs[SOC_GEMV_REG_W/4] == 0x1000 && f.regs[SOC_GEMV_REG_W/4+1] == 1);
    assert(f.regs[SOC_GEMV_REG_S/4+1] == 2 && f.regs[SOC_GEMV_REG_X/4+1] == 3);
    assert(f.regs[SOC_GEMV_REG_Y/4+1] == 4);
    assert(f.regs[SOC_GEMV_REG_ROWS/4] == 3 && f.regs[SOC_GEMV_REG_NB/4] == 2);
    assert(!soc_gemv_hal_run(&h, &j, 1000, &status) && f.starts == 2);
    h = init(&f); f.delay_idle = 1;
    assert(!soc_gemv_hal_run(&h, &j, 1000, &status) && f.polls == 4);
    h = init(&f); f.result = 3;
    assert(soc_gemv_hal_run(&h, &j, 1000, &status) == -1 && errno == EDOM && status == 3 && !h.poisoned);
    h = init(&f); f.timeout = 1;
    assert(soc_gemv_hal_run(&h, &j, 25, &status) == -1 && errno == ETIMEDOUT && h.poisoned);
    unsigned writes = f.writes;
    assert(soc_gemv_hal_run(&h, &j, 1000, &status) == -1 && errno == EIO && f.writes == writes);
    h = init(&f); f.no_valid = 1;
    assert(soc_gemv_hal_run(&h, &j, 1000, &status) == -1 && errno == EPROTO && h.poisoned);
    h = init(&f); f.regs[0] = 0;
    assert(soc_gemv_hal_run(&h, &j, 1000, &status) == -1 && errno == EBUSY && !f.writes);
    h = init(&f); f.regs[0] = SOC_GEMV_IDLE | SOC_GEMV_AUTO_RESTART;
    assert(soc_gemv_hal_run(&h, &j, 1000, &status) == -1 && errno == EBUSY && !f.writes);
    soc_gemv_job bad[] = {j,j,j,j,j,j,j};
    bad[0].rows = 0; bad[1].nb = 153; bad[2].rows = 151937;
    bad[3].quants++; bad[4].scales = j.quants;
    bad[5].y = UINT64_MAX - 3; bad[6].x = j.quants + 16;
    for (size_t i=0; i<sizeof(bad)/sizeof(bad[0]); ++i) {
        h=init(&f);
        assert(soc_gemv_hal_run(&h,&bad[i],1000,&status)==-1 && errno==EINVAL && !f.reads && !f.writes);
    }
}
static void test_arena(void) {
    void *memory = aligned_alloc(4096, 32768); assert(memory);
    soc_gemv_arena a;
    assert(!soc_gemv_arena_init(&a, memory, 0x10000000, 32768));
    void *p1,*p2,*h1,*h2; uint64_t d1,d2;
    assert(!soc_gemv_arena_alloc(&a,8192,&p1,&d1,&h1));
    assert(!soc_gemv_arena_alloc(&a,4096,&p2,&d2,&h2));
    assert(d2==d1+8192 && (unsigned char *)p2==(unsigned char *)p1+8192);
    assert(soc_gemv_arena_contains(&a,p1,d1,8192));
    assert(!soc_gemv_arena_contains(&a,p1,d2,1));
    assert(!soc_gemv_arena_contains(&a,p1,d1,8193));
    assert(soc_gemv_arena_alloc(&a,32768,&p2,&d2,&h2)==-1 && errno==ENOMEM);
    assert(soc_gemv_arena_alloc(&a,1,&p2,&d2,&h2)==-1 && errno==EINVAL);
    soc_gemv_arena_free(&a,h1);
    assert(!soc_gemv_arena_contains(&a,p1,d1,1));
    soc_gemv_arena_free(&a,h2);
    assert(!soc_gemv_arena_alloc(&a,32768,&p1,&d1,&h1));
    soc_gemv_arena_destroy(&a); free(memory);
}
int main(void) {
    test_control(); test_arena();
    puts("HAL control/timeout/ownership and arena tests: PASS (mock MMIO; no FPGA)");
    return 0;
}
