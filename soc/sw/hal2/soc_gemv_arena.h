#ifndef SOC_GEMV_ARENA_H
#define SOC_GEMV_ARENA_H
#include <stddef.h>
#include <stdint.h>
typedef struct soc_gemv_region {
    size_t offset, bytes;
    int used;
    struct soc_gemv_region *next;
} soc_gemv_region;
typedef struct {
    unsigned char *cpu;
    uint64_t device;
    size_t bytes;
    soc_gemv_region *head;
} soc_gemv_arena;
int soc_gemv_arena_init(soc_gemv_arena *, void *, uint64_t, size_t);
int soc_gemv_arena_alloc(soc_gemv_arena *, size_t, void **, uint64_t *, void **);
int soc_gemv_arena_contains(const soc_gemv_arena *, const void *, uint64_t, size_t);
void soc_gemv_arena_free(soc_gemv_arena *, void *);
void soc_gemv_arena_destroy(soc_gemv_arena *);
#endif
