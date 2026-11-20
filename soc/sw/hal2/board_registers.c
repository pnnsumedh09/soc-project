/* Target-only register readback test. No DMA allocator dependency or START.
 * Validate the standalone generated register RTL first; use matching hardware.
 */
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "soc_gemv_hal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>
static volatile uint32_t *regs;
static void barrier(void) {
#if defined(__aarch64__)
    __asm__ __volatile__("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}
static uint32_t read32(void *ctx,uint32_t off) {
    (void)ctx; uint32_t value=regs[off/4]; barrier(); return value;
}
static void write32(void *ctx,uint32_t off,uint32_t value) {
    (void)ctx; barrier(); regs[off/4]=value; barrier();
}
static int number(const char *text,uint64_t *value) {
    if (!text || !*text || *text=='-') return -1;
    char *end; errno=0;
    *value=strtoull(text,&end,0);
    if (errno || end==text) return -1;
    while (*end=='\n' || *end==' ' || *end=='\t') ++end;
    return *end ? -1 : 0;
}
static int attribute(unsigned uio,const char *key,uint64_t *value) {
    char path[160],text[128];
    snprintf(path,sizeof(path),"/sys/class/uio/uio%u/maps/map0/%s",uio,key);
    FILE *f=fopen(path,"r");
    if (!f) return -1;
    int ok=fgets(text,sizeof(text),f)!=NULL;
    fclose(f);
    return ok ? number(text,value) : -1;
}
int main(int argc,char **argv) {
    if (argc!=3) {
        fprintf(stderr,"Usage: %s UIO_NUMBER EXPECTED_BASE (e.g. 0 0xa0000000)\n",argv[0]);
        return 2;
    }
#if !defined(__linux__) || !defined(__aarch64__) || defined(__AARCH64EB__)
    fputs("Linux/AArch64 board required; no MMIO performed\n",stderr); return 2;
#endif
    uint64_t index,expected,addr,size,offset;
    if (number(argv[1],&index) || index>65535 || number(argv[2],&expected) ||
        expected%SOC_GEMV_REG_BYTES || expected<0xa0000000ull || expected>0xafff0000ull) {
        fputs("Invalid UIO index/control base\n",stderr); return 2;
    }
    if (attribute((unsigned)index,"addr",&addr) || attribute((unsigned)index,"size",&size) ||
        attribute((unsigned)index,"offset",&offset) || addr!=expected ||
        size<SOC_GEMV_REG_BYTES || offset) {
        fputs("UIO map does not match the requested GEMV control bank\n",stderr); return 1;
    }
    char device[64],lock_path[128];
    snprintf(device,sizeof(device),"/dev/uio%u",(unsigned)index);
    snprintf(lock_path,sizeof(lock_path),"/tmp/soc-gemv-%llx.lock",(unsigned long long)addr);
    const char *override=getenv("SOC_GEMV_LOCK_FILE");
    int lock=open(override && *override ? override : lock_path,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    int fd=-1,failed=1;
    if (lock<0 || flock(lock,LOCK_EX|LOCK_NB)) goto out;
    char prior=0; ssize_t got=pread(lock,&prior,1,0);
    if (got<0 || (got==1 && prior!='0')) { errno=EBUSY; goto out; }
    fd=open(device,O_RDWR|O_SYNC|O_CLOEXEC);
    if (fd<0 || flock(fd,LOCK_EX|LOCK_NB)) goto out;
    void *mapping=mmap(NULL,SOC_GEMV_REG_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if (mapping==MAP_FAILED) goto out;
    regs=mapping;
    soc_gemv_hal hal={{NULL,read32,write32,NULL,NULL,NULL},0};
    if (pwrite(lock,"1",1,0)!=1) goto out;
    soc_gemv_register_failure detail;
    if (soc_gemv_hal_register_test(&hal,&detail)) {
        if (detail.offset!=UINT32_MAX)
            fprintf(stderr,"Register mismatch: offset=0x%02x expected=0x%08x actual=0x%08x\n",
                    detail.offset,detail.expected,detail.actual);
        goto out;
    }
    if (pwrite(lock,"0",1,0)!=1) goto out;
    failed=0;
out:
    if (failed) perror("Board register test (check ownership/hardware; armed marker requires reset)");
    if (regs) munmap((void *)regs,SOC_GEMV_REG_BYTES);
    if (fd>=0) close(fd);
    if (lock>=0) close(lock);
    printf("GEMV BOARD REGISTERS: %s; no START or DMA submitted by this test\n",failed?"FAIL":"PASS");
    return failed;
}
