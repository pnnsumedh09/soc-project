#include "bw-read.h"

#include <stdio.h>
#include <vector>

static int check(const std::vector<soc_bw_word> & data, uint32_t offset,
                 uint32_t nbytes, uint32_t expected, uint32_t expected_status) {
    uint32_t checksum = 0xdeadbeef;
    uint32_t status = 0xdeadbeef;
    soc_bw_read(data.data() + offset, nbytes, checksum, status);
    if (checksum != expected || status != expected_status) {
        fprintf(stderr, "offset=%u bytes=%u checksum=%08x expected=%08x status=%u expected=%u\n",
                offset, nbytes, checksum, expected, status, expected_status);
        return 1;
    }
    return 0;
}

int main() {
    const uint32_t count = 65538;
    std::vector<soc_bw_word> data(count);
    std::vector<uint32_t> prefix(count + 1, 0);
    int failures = 0;
    for (uint32_t seed = 0; seed < 3; ++seed) {
        for (uint32_t i = 0; i < count; ++i) {
            soc_bw_word word = 0;
            uint32_t sum = 0;
            for (int j = 0; j < 4; ++j) {
                uint32_t lane = seed == 0 ? 0 : seed == 1 ? UINT32_MAX :
                                ((i * 4 + j) * UINT32_C(2654435761)) ^ UINT32_C(0xa5c37e19);
                word |= soc_bw_word(lane) << (32 * j);
                sum += lane;
            }
            data[i] = word;
            prefix[i + 1] = prefix[i] + sum;
        }
        const uint32_t sizes[] = {0, 16, 32, 4080, 4096, 4112, 8192, 1048576};
        for (uint32_t offset = 0; offset <= 1; ++offset) {
            for (unsigned j = 0; j < sizeof(sizes) / sizeof(sizes[0]); ++j) {
                uint32_t words = sizes[j] / 16;
                failures += check(data, offset, sizes[j], prefix[offset + words] - prefix[offset], 0);
            }
        }
        failures += check(data, 0, 1, 0, 1);
        failures += check(data, 0, 15, 0, 1);
        failures += check(data, 0, SOC_BW_MAX_BYTES + 16, 0, 1);
        failures += check(data, 0, UINT32_MAX, 0, 1);
    }
    printf("bandwidth kernel: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
