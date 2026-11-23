/* PYNQ/ZynqMP adapter: existing UIO control HAL and XRT DDR buffers. */
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "soc_gemv_backend.h"
#include "soc_gemv_hal.h"
#include <xrt.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

typedef struct buffer {
    struct buffer *next;
    unsigned int bo;
    void *cpu;
    uint64_t device;
    size_t bytes;
} buffer;

enum {
    PROFILE_X_COPY, PROFILE_Q_SYNC, PROFILE_S_SYNC, PROFILE_X_SYNC,
    PROFILE_OWNERSHIP, PROFILE_HAL, PROFILE_Y_SYNC, PROFILE_Y_COPY, PROFILE_PHASES
};

static struct {
    void *library;
    xclDeviceHandle device;
    __typeof__(&xclOpen) open;
    __typeof__(&xclClose) close;
    __typeof__(&xclAllocBO) alloc;
    __typeof__(&xclFreeBO) free;
    __typeof__(&xclMapBO) map;
    __typeof__(&xclGetBOProperties) properties;
    __typeof__(&xclSyncBO) sync;
    buffer *buffers, *x, *y;
    size_t used;
    int uio, lock, ready;
    void *register_mapping;
    volatile uint32_t *registers;
    soc_gemv_hal hal;
    uint64_t timeout;
    int profile;
    uint64_t attempts, successful, failed, run_ns, phase_ns[PROFILE_PHASES];
} g = {.uio = -1, .lock = -1};

static int fail(int error) { errno = error; return -1; }
static const char *setting(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return value && *value ? value : fallback;
}
static int number(const char *value, uint64_t *result) {
    if (!value || !*value || *value == '-') return fail(EINVAL);
    char *end;
    errno = 0;
    unsigned long long n = strtoull(value, &end, 0);
    if (errno || end == value) return fail(EINVAL);
    while (*end == ' ' || *end == '\n' || *end == '\t') ++end;
    if (*end) return fail(EINVAL);
    *result = n;
    return 0;
}
static int attribute(const char *directory, const char *name, uint64_t *result) {
    char path[512], value[128];
    int n = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (n < 0 || (size_t)n >= sizeof(path)) return fail(ENAMETOOLONG);
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    int ok = fgets(value, sizeof(value), file) != NULL;
    fclose(file);
    return ok ? number(value, result) : fail(EIO);
}
static void barrier(void *unused) {
    (void)unused;
#if defined(__aarch64__)
    __asm__ __volatile__("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}
static uint32_t read32(void *unused, uint32_t offset) {
    (void)unused;
    uint32_t value = g.registers[offset / 4];
    barrier(NULL);
    return value;
}
static void write32(void *unused, uint32_t offset, uint32_t value) {
    (void)unused;
    barrier(NULL);
    g.registers[offset / 4] = value;
    barrier(NULL);
}
static uint64_t now_ns(void *unused) {
    (void)unused;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint64_t profile_start(void) {
    return g.profile ? now_ns(NULL) : 0;
}
static void profile_end(unsigned phase, uint64_t start) {
    if (g.profile) {
        int saved = errno;
        g.phase_ns[phase] += now_ns(NULL) - start;
        errno = saved;
    }
}
static void print_profile(void) {
    if (!g.profile || !g.attempts) return;
    static const char *names[] = {
        "activation_copy", "weight_quants_sync", "weight_scales_sync", "activation_sync",
        "ownership_checks", "hal_submit_wait", "output_sync", "output_copy"
    };
    fprintf(stderr, "soc_gemv XRT profile: attempts=%llu successful=%llu failed=%llu\n",
            (unsigned long long)g.attempts, (unsigned long long)g.successful,
            (unsigned long long)g.failed);
    fprintf(stderr, "  run_total %.6f s\n", g.run_ns * 1e-9);
    uint64_t accounted = 0;
    for (unsigned i = 0; i < PROFILE_PHASES; ++i) {
        accounted += g.phase_ns[i];
        fprintf(stderr, "  %-20s %.6f s %6.2f%%\n", names[i], g.phase_ns[i] * 1e-9,
                g.run_ns ? 100.0 * (double)g.phase_ns[i] / (double)g.run_ns : 0.0);
    }
    uint64_t other = g.run_ns > accounted ? g.run_ns - accounted : 0;
    fprintf(stderr, "  %-20s %.6f s %6.2f%%\n", "other", other * 1e-9,
            g.run_ns ? 100.0 * (double)other / (double)g.run_ns : 0.0);
    fprintf(stderr, "  HAL timing includes register programming and CPU polling; not pure FPGA compute time.\n");
    g.attempts = g.successful = g.failed = g.run_ns = 0;
    memset(g.phase_ns, 0, sizeof(g.phase_ns));
}
static int marker(char value) {
    return pwrite(g.lock, &value, 1, 0) == 1 ? 0 : fail(EIO);
}
static void quarantine(void) {
    fprintf(stderr, "soc_gemv XRT: unknown completion; retaining process, locks and DMA buffers.\n"
                    "Do not kill this process or reuse the accelerator. Capture UART output; shut down/reset the board.\n");
    fflush(stderr);
    for (;;) pause();
}
static buffer *lookup(const void *cpu, uint64_t device, size_t bytes) {
    for (buffer *b = g.buffers; b; b = b->next)
        if (b->cpu == cpu && b->device == device && bytes <= b->bytes) return b;
    return NULL;
}
static void release(buffer *b) {
    buffer **link = &g.buffers;
    while (*link && *link != b) link = &(*link)->next;
    if (!*link) return;
    *link = b->next;
    munmap(b->cpu, b->bytes);
    g.free(g.device, b->bo);
    g.used -= b->bytes;
    free(b);
}
static int allocate_buffer(size_t bytes, buffer **out) {
    if (!bytes || bytes > SIZE_MAX - 4095) return fail(EINVAL);
    bytes = (bytes + 4095) & ~(size_t)4095;
    buffer *b = calloc(1, sizeof(*b));
    if (!b) return -1;
    /* Bank 0, default BO flags: the same allocation mode as PYNQ allocate. */
    b->bo = g.alloc(g.device, bytes, 0, 0);
    if (b->bo >= 0x80000000u) { free(b); return fail(ENOMEM); }
    struct xclBOProperties properties;
    memset(&properties, 0, sizeof(properties));
    if (g.properties(g.device, b->bo, &properties) || properties.size < bytes ||
        properties.size > SIZE_MAX || properties.paddr % 4096 ||
        properties.paddr >= 0x80000000ull ||
        properties.size > 0x80000000ull - properties.paddr) {
        g.free(g.device, b->bo); free(b); return fail(EINVAL);
    }
    b->bytes = (size_t)properties.size;
    b->device = properties.paddr;
    for (buffer *other = g.buffers; other; other = other->next) {
        if (b->device < other->device + other->bytes && other->device < b->device + b->bytes) {
            g.free(g.device, b->bo); free(b); return fail(EINVAL);
        }
    }
    b->cpu = g.map(g.device, b->bo, true);
    if (!b->cpu || b->cpu == MAP_FAILED || (uintptr_t)b->cpu % 4096) {
        if (b->cpu && b->cpu != MAP_FAILED) munmap(b->cpu, b->bytes);
        g.free(g.device, b->bo); free(b); return fail(EIO);
    }
    b->next = g.buffers;
    g.buffers = b;
    g.used += b->bytes;
    *out = b;
    return 0;
}
static int synchronize(buffer *b, enum xclBOSyncDirection direction, size_t bytes) {
    if (g.sync(g.device, b->bo, direction, bytes, 0)) return fail(EIO);
    return 0;
}
static void shutdown_backend(void) {
    if (g.hal.poisoned) quarantine();
    print_profile();
    g.ready = 0;
    while (g.buffers) release(g.buffers);
    g.x = g.y = NULL;
    if (g.register_mapping) munmap(g.register_mapping, SOC_GEMV_REG_BYTES);
    g.register_mapping = NULL; g.registers = NULL;
    if (g.uio >= 0) close(g.uio);
    if (g.device) g.close(g.device);
    g.device = NULL; g.uio = -1;
    if (g.library) dlclose(g.library);
    g.library = NULL;
    if (g.lock >= 0) close(g.lock);
    g.lock = -1;
}
static int init_backend(void) {
#if !defined(SOC_GEMV_XRT_TEST) && (!defined(__linux__) || !defined(__aarch64__) || defined(__AARCH64EB__))
    fprintf(stderr, "soc_gemv XRT: little-endian Linux/AArch64 required\n");
    return fail(ENOTSUP);
#endif
    if (g.ready) return 0;
    g.profile = strcmp(setting("SOC_GEMV_PROFILE", "0"), "0") != 0;
    uint64_t base, size, offset, timeout;
    const char *sysfs = setting("SOC_GEMV_UIO_SYSFS", "/sys/class/uio/uio4/maps/map0");
    if (attribute(sysfs, "addr", &base) || attribute(sysfs, "size", &size) ||
        attribute(sysfs, "offset", &offset) || base != 0xa0000000ull ||
        size < SOC_GEMV_REG_BYTES || offset ||
        number(setting("SOC_GEMV_TIMEOUT_MS", "10000"), &timeout) || !timeout || timeout > 600000)
        return fail(EINVAL);
    g.timeout = timeout * 1000000ull;
    g.lock = open(setting("SOC_GEMV_LOCK_FILE", "/tmp/soc-gemv-a0000000.lock"),
                  O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (g.lock < 0 || flock(g.lock, LOCK_EX | LOCK_NB)) goto bad;
    char previous = 0;
    ssize_t got = pread(g.lock, &previous, 1, 0);
    if (got < 0 || (got == 1 && previous != '0')) {
        fprintf(stderr, "soc_gemv XRT: uncleared DMA marker; establish hardware reset before clearing it\n");
        errno = EBUSY; goto bad;
    }
    g.library = dlopen(setting("SOC_GEMV_XRT_LIBRARY", "libxrt_core.so"), RTLD_NOW | RTLD_LOCAL);
    if (!g.library) { fprintf(stderr, "soc_gemv XRT: %s\n", dlerror()); errno = ENOENT; goto bad; }
#define LOAD(member, symbol) do { \
    void *symbol_address = dlsym(g.library, #symbol); \
    _Static_assert(sizeof(g.member) == sizeof(symbol_address), "Function pointer size"); \
    memcpy(&g.member, &symbol_address, sizeof(symbol_address)); \
    if (!g.member) { errno = ENOSYS; goto bad; } \
} while (0)
    LOAD(open, xclOpen); LOAD(close, xclClose);
    LOAD(alloc, xclAllocBO); LOAD(free, xclFreeBO); LOAD(map, xclMapBO);
    LOAD(properties, xclGetBOProperties); LOAD(sync, xclSyncBO);
#undef LOAD
    g.device = g.open(0, NULL, 0);
    if (!g.device) { errno = ENODEV; goto bad; }
    g.uio = open(setting("SOC_GEMV_UIO_DEVICE", "/dev/uio4"), O_RDWR | O_SYNC | O_CLOEXEC);
    if (g.uio < 0 || flock(g.uio, LOCK_EX | LOCK_NB)) goto bad;
    g.register_mapping = mmap(NULL, SOC_GEMV_REG_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, g.uio, 0);
    if (g.register_mapping == MAP_FAILED) { g.register_mapping = NULL; goto bad; }
    g.registers = g.register_mapping;
    uint32_t ctrl = read32(NULL, SOC_GEMV_REG_CTRL);
    if (!(ctrl & SOC_GEMV_IDLE) || (ctrl & (SOC_GEMV_START | SOC_GEMV_AUTO_RESTART))) {
        errno = EBUSY; goto bad;
    }
    if (allocate_buffer(8192, &g.x) ||
        allocate_buffer((size_t)SOC_GEMV_MAX_ROWS * sizeof(float), &g.y)) goto bad;
    g.hal = (soc_gemv_hal){{NULL, read32, write32, now_ns, barrier, barrier}, 0};
    if (marker('0')) goto bad;
    g.ready = 1;
    fprintf(stderr, "soc_gemv XRT: UIO=%s, bank=0, no software allocation cap\n",
            setting("SOC_GEMV_UIO_DEVICE", "/dev/uio4"));
    return 0;
bad: {
    int saved = errno;
    shutdown_backend(); errno = saved; return -1;
}}
static int alloc_backend(size_t bytes, void **cpu, uint64_t *device, void **handle) {
    if (!g.ready || !cpu || !device || !handle) return fail(EINVAL);
    buffer *b;
    if (allocate_buffer(bytes, &b)) return -1;
    *cpu = b->cpu; *device = b->device; *handle = b;
    return 0;
}
static void free_backend(void *handle) {
    if (g.hal.poisoned) quarantine();
    if (g.ready && handle) release(handle);
}
static int run_job(const soc_q8r_mat *m, uint64_t q, uint64_t s, const void *x, float *y) {
    if (!g.ready || !m || !x || !y || !m->rows || m->rows > SOC_GEMV_MAX_ROWS ||
        !m->nb || m->nb > SOC_GEMV_MAX_NB) return fail(EINVAL);
    size_t qb = (size_t)m->rows * m->nb * 32, sb = (size_t)m->rows * m->nb * 2;
    size_t xb = (size_t)m->nb * 34, yb = (size_t)m->rows * sizeof(float);
    buffer *w = lookup(m->quants, q, qb), *scales = lookup(m->scales, s, sb);
    if (!w || !scales) return fail(EINVAL);
    uint64_t tick = profile_start();
    memcpy(g.x->cpu, x, xb);
    profile_end(PROFILE_X_COPY, tick);
    /* Synchronize only this matrix, not every matrix in the weight cache. */
    tick = profile_start();
    int sync_rc = synchronize(w, XCL_BO_SYNC_BO_TO_DEVICE, qb);
    profile_end(PROFILE_Q_SYNC, tick);
    if (sync_rc) return -1;
    tick = profile_start();
    sync_rc = synchronize(scales, XCL_BO_SYNC_BO_TO_DEVICE, sb);
    profile_end(PROFILE_S_SYNC, tick);
    if (sync_rc) return -1;
    tick = profile_start();
    sync_rc = synchronize(g.x, XCL_BO_SYNC_BO_TO_DEVICE, xb);
    profile_end(PROFILE_X_SYNC, tick);
    if (sync_rc) return -1;
    soc_gemv_job job = {q, s, g.x->device, g.y->device, m->rows, m->nb};
    tick = profile_start();
    int marker_rc = marker('1');
    profile_end(PROFILE_OWNERSHIP, tick);
    if (marker_rc) return -1;
    uint32_t status;
    tick = profile_start();
    int rc = soc_gemv_hal_run(&g.hal, &job, g.timeout, &status);
    int saved = errno;
    profile_end(PROFILE_HAL, tick);
    if (g.hal.poisoned) quarantine();
    tick = profile_start();
    uint32_t ctrl = read32(NULL, SOC_GEMV_REG_CTRL);
    if (!(ctrl & SOC_GEMV_IDLE) || (ctrl & (SOC_GEMV_START | SOC_GEMV_AUTO_RESTART))) {
        g.hal.poisoned = 1; quarantine();
    }
    if (marker('0')) { g.hal.poisoned = 1; quarantine(); }
    profile_end(PROFILE_OWNERSHIP, tick);
    if (rc) { errno = saved; return -1; }
    tick = profile_start();
    sync_rc = synchronize(g.y, XCL_BO_SYNC_BO_FROM_DEVICE, yb);
    profile_end(PROFILE_Y_SYNC, tick);
    if (sync_rc) return -1;
    tick = profile_start();
    memcpy(y, g.y->cpu, yb);
    profile_end(PROFILE_Y_COPY, tick);
    return 0;
}
static int run_backend(const soc_q8r_mat *m, uint64_t q, uint64_t s, const void *x, float *y) {
    if (!g.profile) return run_job(m, q, s, x, y);
    uint64_t start = now_ns(NULL);
    int rc = run_job(m, q, s, x, y);
    int saved = errno;
    g.run_ns += now_ns(NULL) - start;
    ++g.attempts;
    if (rc) ++g.failed;
    else ++g.successful;
    errno = saved;
    return rc;
}
const soc_gemv_backend soc_gemv_backend_fpga = {
    "fpga", init_backend, shutdown_backend, alloc_backend, free_backend, run_backend
};
