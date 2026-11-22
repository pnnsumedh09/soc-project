/* Optional Linux/A53 adapter: UIO map0 + persistent u-dma-buf arena.
 * Never treat heap pointers as device addresses. Requires confirmed board setup.
 */
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "soc_gemv_backend.h"
#include "soc_gemv_hal.h"
#include "soc_gemv_arena.h"
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24, "FP32 host required");

static struct {
    int uio_fd, dma_fd, lock_fd, ready;
    volatile uint32_t *regs;
    void *mapping;
    size_t bytes;
    soc_gemv_arena arena;
    soc_gemv_hal hal;
    void *x, *y, *xh, *yh;
    uint64_t xd, yd, timeout;
} g = {.uio_fd = -1, .dma_fd = -1, .lock_fd = -1};

static int fail(int e) { errno = e; return -1; }
static const char *setting(const char *key, const char *dflt) {
    const char *s = getenv(key); return s && *s ? s : dflt;
}
static int number(const char *s, uint64_t *v) {
    if (!s || !*s || *s == '-') return fail(EINVAL);
    char *end;
    errno = 0;
    unsigned long long n = strtoull(s, &end, 0);
    if (errno || end == s) return fail(EINVAL);
    while (*end == '\n' || *end == ' ' || *end == '\t') ++end;
    if (*end) return fail(EINVAL);
    *v = n; return 0;
}
static int attribute(const char *dir, const char *name, uint64_t *value) {
    char path[512], text[128];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof(path)) return fail(ENAMETOOLONG);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = fgets(text, sizeof(text), f) != NULL;
    fclose(f);
    return ok ? number(text, value) : fail(EIO);
}
static void barrier(void *unused) {
    (void)unused;
#if defined(__aarch64__)
    __asm__ __volatile__("dsb sy" ::: "memory");
#else
    __sync_synchronize(); /* Native compile checks only; init refuses this host. */
#endif
}
static uint32_t read32(void *unused, uint32_t off) {
    (void)unused;
    uint32_t value = g.regs[off / 4];
    barrier(NULL);
    return value;
}
static void write32(void *unused, uint32_t off, uint32_t value) {
    (void)unused;
    barrier(NULL); g.regs[off / 4] = value; barrier(NULL);
}
static uint64_t now_ns(void *unused) {
    (void)unused;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
/* A persistent marker prevents a later process reusing buffers after a crash or
 * timeout. Clear it manually only after resetting/reloading the accelerator.
 * Mark before START, clear only after verified completion and idle. */
static int marker(char value) {
    return pwrite(g.lock_fd, &value, 1, 0) == 1 ? 0 : fail(EIO);
}
static void fpga_shutdown(void) {
    g.ready = 0;
    soc_gemv_arena_destroy(&g.arena);
    if (g.mapping) munmap(g.mapping, g.bytes);
    if (g.regs) munmap((void *)g.regs, SOC_GEMV_REG_BYTES);
    g.mapping = NULL; g.regs = NULL;
    if (g.dma_fd >= 0) close(g.dma_fd);
    if (g.uio_fd >= 0) close(g.uio_fd);
    if (g.lock_fd >= 0) close(g.lock_fd);
    g.dma_fd = g.uio_fd = g.lock_fd = -1;
    g.x = g.y = g.xh = g.yh = NULL;
    /* The external driver owns the persistent pool. Do not unload it while
     * a failed/crashed accelerator can still be issuing DMA. */
}
static int fpga_init(void) {
#if !defined(__linux__) || !defined(__aarch64__) || defined(__AARCH64EB__)
    fprintf(stderr, "soc_gemv fpga: Linux/AArch64 board required\n");
    return fail(ENOTSUP);
#endif
    if (g.ready) return 0;
    if (g.hal.poisoned) return fail(EIO);
    uint64_t base, map_size, map_offset, phys, size, mode, coherent, timeout;
    const char *us = setting("SOC_GEMV_UIO_SYSFS", "/sys/class/uio/uio0/maps/map0");
    const char *ds = setting("SOC_GEMV_DMA_SYSFS", "/sys/class/u-dma-buf/udmabuf0");
    if (attribute(us, "addr", &base) || attribute(us, "size", &map_size) ||
        attribute(us, "offset", &map_offset) ||
        attribute(ds, "phys_addr", &phys) || attribute(ds, "size", &size) ||
        attribute(ds, "sync_mode", &mode) || attribute(ds, "dma_coherent", &coherent) ||
        number(setting("SOC_GEMV_TIMEOUT_MS", "10000"), &timeout)) return -1;
    uint64_t expected;
    if (number(setting("SOC_GEMV_EXPECTED_BASE", "0xa0000000"), &expected)) return -1;
    /* HP0 is noncoherent. Only write-combined, noncached CPU mappings here.
     * Cached/coherent modes need a different, verified ownership strategy. */
    if (base != expected || map_offset || map_size < SOC_GEMV_REG_BYTES ||
        (mode != 2 && mode != 6) || coherent || !timeout || timeout > 600000 ||
        !size || size > SIZE_MAX || size % SOC_REPACK_ALIGN ||
        phys % SOC_REPACK_ALIGN || phys > UINT64_MAX - size || phys + size > 0x80000000ull)
        return fail(EINVAL);
    char default_lock[128];
    snprintf(default_lock, sizeof(default_lock), "/tmp/soc-gemv-%llx.lock", (unsigned long long)base);
    g.lock_fd = open(setting("SOC_GEMV_LOCK_FILE", default_lock), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (g.lock_fd < 0 || flock(g.lock_fd, LOCK_EX | LOCK_NB)) goto bad;
    char previous = 0;
    ssize_t got = pread(g.lock_fd, &previous, 1, 0);
    if (got < 0) goto bad;
    if (got == 1 && previous != '0') {
        fprintf(stderr, "soc_gemv fpga: uncleared DMA marker; reset hardware before clearing %s\n",
                setting("SOC_GEMV_LOCK_FILE", default_lock));
        errno = EBUSY; goto bad;
    }
    g.uio_fd = open(setting("SOC_GEMV_UIO_DEVICE", "/dev/uio0"), O_RDWR | O_SYNC | O_CLOEXEC);
    if (g.uio_fd < 0 || flock(g.uio_fd, LOCK_EX | LOCK_NB)) goto bad;
    g.dma_fd = open(setting("SOC_GEMV_DMA_DEVICE", "/dev/udmabuf0"), O_RDWR | O_SYNC | O_CLOEXEC);
    if (g.dma_fd < 0 || flock(g.dma_fd, LOCK_EX | LOCK_NB)) goto bad;
    void *regs = mmap(NULL, SOC_GEMV_REG_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, g.uio_fd, 0);
    if (regs == MAP_FAILED) goto bad;
    g.regs = regs;
    uint32_t ctrl = read32(NULL, SOC_GEMV_REG_CTRL);
    if (!(ctrl & SOC_GEMV_IDLE) || (ctrl & (SOC_GEMV_AUTO_RESTART | SOC_GEMV_START))) {
        errno = EBUSY; goto bad;
    }
    g.bytes = (size_t)size;
    g.mapping = mmap(NULL, g.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, g.dma_fd, 0);
    if (g.mapping == MAP_FAILED) { g.mapping = NULL; goto bad; }
    if (soc_gemv_arena_init(&g.arena, g.mapping, phys, g.bytes)) goto bad;
    const size_t xb = 8192;
    const size_t yb = ((size_t)SOC_GEMV_MAX_ROWS * 4 + 4095) & ~(size_t)4095;
    if (soc_gemv_arena_alloc(&g.arena, xb, &g.x, &g.xd, &g.xh) ||
        soc_gemv_arena_alloc(&g.arena, yb, &g.y, &g.yd, &g.yh)) goto bad;
    g.hal = (soc_gemv_hal){{NULL, read32, write32, now_ns, barrier, barrier}, 0};
    g.timeout = timeout * 1000000ull;
    if (marker('0')) goto bad;
    g.ready = 1;
    return 0;
bad: {
        int saved = errno;
        fpga_shutdown(); errno = saved; return -1;
    }
}
static int fpga_alloc(size_t n, void **cpu, uint64_t *dev, void **handle) {
    if (!g.ready || g.hal.poisoned) return fail(EIO);
    return soc_gemv_arena_alloc(&g.arena, n, cpu, dev, handle);
}
static void fpga_free(void *handle) {
    /* Quarantine the whole arena after unknown completion: no reuse. */
    if (g.ready && !g.hal.poisoned) soc_gemv_arena_free(&g.arena, handle);
}
static int fpga_run(const soc_q8r_mat *m, uint64_t q, uint64_t s, const void *x, float *y) {
    if (!g.ready || g.hal.poisoned) return fail(EIO);
    if (!m || !x || !y || !m->rows || m->rows > SOC_GEMV_MAX_ROWS ||
        !m->nb || m->nb > SOC_GEMV_MAX_NB ||
        !soc_gemv_arena_contains(&g.arena, m->quants, q, (size_t)m->rows * m->nb * 32) ||
        !soc_gemv_arena_contains(&g.arena, m->scales, s, (size_t)m->rows * m->nb * 2))
        return fail(EINVAL);
    memcpy(g.x, x, m->nb * 34);
    soc_gemv_job j = {q, s, g.xd, g.yd, m->rows, m->nb};
    uint32_t status;
    if (marker('1')) { g.hal.poisoned = 1; return -1; }
    int rc = soc_gemv_hal_run(&g.hal, &j, g.timeout, &status);
    int saved = errno;
    if (rc && saved == EBUSY) g.hal.poisoned = 1;
    if (!g.hal.poisoned) {
        /* An external user bypassing the locks can still invalidate ownership. */
        if (!(read32(NULL, SOC_GEMV_REG_CTRL) & SOC_GEMV_IDLE) || marker('0')) {
            g.hal.poisoned = 1; return fail(EIO);
        }
    }
    if (rc) { errno = saved; return -1; }
    memcpy(y, g.y, m->rows * sizeof(float));
    return 0;
}
const soc_gemv_backend soc_gemv_backend_fpga = {
    "fpga", fpga_init, fpga_shutdown, fpga_alloc, fpga_free, fpga_run
};
