#define _POSIX_C_SOURCE 200809L
#include "soc_gemv.h"
#include "soc_repack.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    /* Guaranteed unavailable configuration: cannot touch an actual UIO device. */
    fputs("HOST NEGATIVE TEST: the next FPGA initialization refusal is expected; no board access is attempted\n", stderr);
    setenv("SOC_GEMV", "1", 1);
    setenv("SOC_GEMV_BACKEND", "fpga", 1);
    setenv("SOC_GEMV_STRICT", "0", 1);
    setenv("SOC_GEMV_VERIFY", "0", 1);
    setenv("SOC_GEMV_UIO_SYSFS", "/dev/null/no-uio", 1);
    unsigned char w[68]={0}, x[34]={0};
    float out[4]={-99,-99,-99,-99}, expected[2];
    w[1]=w[35]=x[1]=0x3c;
    for (unsigned i=0;i<32;++i) { w[2+i]=(unsigned char)i; w[36+i]=(unsigned char)(i+1); x[2+i]=2; }
    expected[0]=soc_q8_0_row_dot_native(w,x,1);
    expected[1]=soc_q8_0_row_dot_native(w+34,x,1);
    soc_gemv_q8_0(w,34,2,32,x,out+1,NULL);
    assert(!memcmp(out+1,expected,sizeof(expected)) && out[0]==-99 && out[3]==-99);
    soc_gemv_stats st; soc_gemv_get_stats(&st);
    assert(st.calls==0 && st.tensors==0 && st.fallback_calls==1);
    puts("HOST NEGATIVE TEST: unavailable FPGA backend uses CPU fallback: PASS");
    return 0;
}
