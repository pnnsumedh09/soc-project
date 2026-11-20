#ifndef SOC_BW_READ_H
#define SOC_BW_READ_H

#include <stdint.h>

#ifdef SOC_BW_HOST
typedef unsigned __int128 soc_bw_word;
#else
#include <ap_int.h>
typedef ap_uint<128> soc_bw_word;
#endif

enum { SOC_BW_MAX_BYTES = 256 * 1024 * 1024 };

void soc_bw_read(const soc_bw_word * mem, uint32_t nbytes,
                 uint32_t & checksum, uint32_t & status);

#endif
