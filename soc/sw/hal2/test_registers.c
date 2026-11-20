#include "soc_gemv_hal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint32_t words[32]; unsigned writes; int fault; } model;
static uint32_t rd(void *p,uint32_t off) {
    model *m=p;
    if (m->fault==2 && off==0x14) off=0x10;
    return m->words[off/4];
}
static void wr(void *p,uint32_t off,uint32_t data) {
    model *m=p; ++m->writes;
    assert(off!=SOC_GEMV_REG_CTRL); /* A register test must never start DMA. */
    if (m->fault==1 && off==0x10) data&=~(UINT32_C(1)<<19);
    if (m->fault==2 && off==0x14) off=0x10;
    if (m->fault==3 && off==0x58) return;
    m->words[off/4]=data;
}
static soc_gemv_hal init(model *m) {
    memset(m,0,sizeof(*m)); m->words[0]=SOC_GEMV_IDLE;
    return (soc_gemv_hal){{m,rd,wr,NULL,NULL,NULL},0};
}
int main(void) {
    model m; soc_gemv_hal h=init(&m); soc_gemv_register_failure detail;
    const uint32_t offsets[]={0x10,0x14,0x20,0x24,0x30,0x34,0x40,0x44,0x50,0x58};
    for (unsigned i=0;i<10;++i) m.words[offsets[i]/4]=0x10203040u+i;
    uint32_t before[32]; memcpy(before,m.words,sizeof(before));
    assert(!soc_gemv_hal_register_test(&h,&detail));
    assert(!memcmp(before,m.words,sizeof(before)) && m.writes==700 && detail.offset==UINT32_MAX);
    for (int fault=1;fault<=3;++fault) {
        h=init(&m); m.fault=fault;
        assert(soc_gemv_hal_register_test(&h,&detail)==-1 && errno==EIO);
        assert(detail.offset==(fault==1 ? 0x10u : fault==2 ? 0x14u : 0x58u));
        assert(detail.expected!=detail.actual);
    }
    h=init(&m); m.words[0]=0;
    assert(soc_gemv_hal_register_test(&h,&detail)==-1 && errno==EBUSY && !m.writes);
    h=init(&m); m.words[0]|=SOC_GEMV_AUTO_RESTART;
    assert(soc_gemv_hal_register_test(&h,&detail)==-1 && errno==EBUSY && !m.writes);
    h=init(&m); h.poisoned=1;
    assert(soc_gemv_hal_register_test(&h,&detail)==-1 && errno==EIO && !m.writes);
    puts("Register-pattern/restore and injected stuck-bit/alias/drop tests: PASS (model only)");
    return 0;
}
