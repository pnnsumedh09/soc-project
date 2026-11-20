#include "soc_gemv_arena.h"
#include "soc_repack.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
int soc_gemv_arena_init(soc_gemv_arena *a, void *cpu, uint64_t dev, size_t n) {
    if (!a || !cpu || !n || (uintptr_t)cpu % SOC_REPACK_ALIGN ||
        dev % SOC_REPACK_ALIGN || n % SOC_REPACK_ALIGN || dev > UINT64_MAX - n) {
        errno = EINVAL; return -1;
    }
    soc_gemv_region *r = calloc(1, sizeof(*r));
    if (!r) return -1;
    r->bytes = n;
    *a = (soc_gemv_arena){cpu, dev, n, r};
    return 0;
}
int soc_gemv_arena_alloc(soc_gemv_arena *a, size_t n, void **cpu, uint64_t *dev, void **handle) {
    if (!a || !cpu || !dev || !handle || !n || n % SOC_REPACK_ALIGN) {
        errno = EINVAL; return -1;
    }
    for (soc_gemv_region *r = a->head; r; r = r->next) {
        if (r->used || r->bytes < n) continue;
        if (r->bytes > n) {
            soc_gemv_region *tail = calloc(1, sizeof(*tail));
            if (!tail) return -1;
            *tail = (soc_gemv_region){r->offset + n, r->bytes - n, 0, r->next};
            r->next = tail;
            r->bytes = n;
        }
        r->used = 1;
        *cpu = a->cpu + r->offset; *dev = a->device + r->offset; *handle = r;
        return 0;
    }
    errno = ENOMEM; return -1;
}
int soc_gemv_arena_contains(const soc_gemv_arena *a, const void *cpu, uint64_t dev, size_t n) {
    if (!a || !n) return 0;
    for (const soc_gemv_region *r = a->head; r; r = r->next)
        if (r->used && cpu == a->cpu + r->offset && dev == a->device + r->offset && n <= r->bytes)
            return 1;
    return 0;
}
void soc_gemv_arena_free(soc_gemv_arena *a, void *handle) {
    if (!a || !handle) return;
    for (soc_gemv_region *r = a->head; r; r = r->next)
        if (r == handle) { r->used = 0; break; }
    for (soc_gemv_region *r = a->head; r && r->next;) {
        if (!r->used && !r->next->used) {
            soc_gemv_region *next = r->next;
            r->bytes += next->bytes; r->next = next->next; free(next);
        } else r = r->next;
    }
}
void soc_gemv_arena_destroy(soc_gemv_arena *a) {
    if (!a) return;
    while (a->head) { soc_gemv_region *r = a->head; a->head = r->next; free(r); }
    memset(a, 0, sizeof(*a));
}
