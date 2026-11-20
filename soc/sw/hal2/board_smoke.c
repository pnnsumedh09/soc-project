/* Explicit board-only test. Does not run automatically in `make test`. */
#include "soc_gemv_backend.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    const soc_gemv_backend *b = &soc_gemv_backend_fpga;
    if (b->init()) { perror("FPGA init (check bitstream/UIO/DMA configuration)"); return 1; }
    const uint32_t rows=9, nb=33; /* Includes multiple rows and >32 blocks. */
    size_t qb=soc_q8r_quants_bytes(rows,nb), sb=soc_q8r_scales_bytes(rows,nb);
    void *q=NULL,*s=NULL,*qh=NULL,*sh=NULL; uint64_t qd=0,sd=0;
    soc_q8r_mat m;
    unsigned char *native=calloc((size_t)rows*nb,34), *x=calloc(nb,34);
    float got[9], expected[9]; int failed=1;
    if (!native || !x || b->alloc(qb,&q,&qd,&qh) || b->alloc(sb,&s,&sd,&sh) ||
        soc_q8r_init(&m,rows,nb,q,qb,s,sb)) goto out;
    for (uint32_t block=0; block<rows*nb; ++block) {
        native[block*34+1]=0x3c; /* FP16 1 */
        for (uint32_t j=0;j<32;++j) native[block*34+2+j]=(unsigned char)(block*13+j*7);
    }
    for (uint32_t block=0;block<nb;++block) {
        x[block*34+1]=0x38; /* FP16 0.5 */
        for (uint32_t j=0;j<32;++j) x[block*34+2+j]=(unsigned char)(block*3+j*5);
    }
    if (soc_q8r_from_q8_0(&m,native,nb*34)) goto out;
    soc_q8r_gemv_ref(&m,x,expected);
    for (unsigned pass=0;pass<2;++pass) {
        if (b->run(&m,qd,sd,x,got) || memcmp(got,expected,sizeof(got))) goto out;
    }
    /* Update weights in place: verify device sees the changed CPU writes. */
    memset(native+2,0,32);
    if (soc_q8r_from_q8_0(&m,native,nb*34)) goto out;
    soc_q8r_gemv_ref(&m,x,expected);
    if (b->run(&m,qd,sd,x,got) || memcmp(got,expected,sizeof(got))) goto out;
    /* Error completion must leave the caller's output untouched. */
    x[0]=0; x[1]=0x7c;
    for (unsigned i=0;i<9;++i) got[i]=-123.5f;
    if (!b->run(&m,qd,sd,x,got)) goto out;
    for (unsigned i=0;i<9;++i) if (got[i]!=-123.5f) goto out;
    x[1]=0x38;
    if (b->run(&m,qd,sd,x,got) || memcmp(got,expected,sizeof(got))) goto out;
    failed=0;
out:
    if (failed) perror("FPGA smoke failure");
    if (qh) b->free(qh);
    if (sh) b->free(sh);
    b->shutdown(); free(native); free(x);
    printf("FPGA board GEMV smoke: %s\n",failed?"FAIL":"PASS");
    return failed;
}
