#include "bw-read.h"

void soc_bw_read(const soc_bw_word * mem, uint32_t nbytes,
                 uint32_t & checksum, uint32_t & status) {
#ifndef SOC_BW_HOST
#pragma HLS INTERFACE s_axilite port=return bundle=control
#pragma HLS INTERFACE s_axilite port=mem bundle=control offset=0x10
#pragma HLS INTERFACE s_axilite port=nbytes bundle=control offset=0x20
#pragma HLS INTERFACE s_axilite port=checksum bundle=control offset=0x28
#pragma HLS INTERFACE s_axilite port=status bundle=control offset=0x30
#endif
    checksum = 0;
    status = 0;
    if ((nbytes & 15) != 0 || nbytes > SOC_BW_MAX_BYTES) {
        status = 1;
        return;
    }

    uint32_t sums[4] = {0, 0, 0, 0};
#ifndef SOC_BW_HOST
#pragma HLS ARRAY_PARTITION variable=sums complete
#endif
    // Separate accumulators keep the checksum off the memory throughput path.
read_words:
    for (uint32_t i = 0; i < nbytes / 16; ++i) {
#ifndef SOC_BW_HOST
#pragma HLS PIPELINE II=1
#endif
        soc_bw_word word = mem[i];
        for (int j = 0; j < 4; ++j) {
#ifndef SOC_BW_HOST
#pragma HLS UNROLL
#endif
            sums[j] += (uint32_t) (word >> (32 * j));
        }
    }
    checksum = sums[0] + sums[1] + sums[2] + sums[3];
}
