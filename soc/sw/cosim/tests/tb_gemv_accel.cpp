#include <systemc.h>
#include <tlm.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>

#include <math.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "gemv_accel.h"
#include "soc_repack.h"

struct TestMemory : sc_core::sc_module {
    tlm_utils::simple_target_socket<TestMemory> socket;
    std::vector<unsigned char> bytes;
    uint64_t read_bytes;
    uint64_t write_bytes;

    TestMemory(sc_core::sc_module_name name, size_t size)
        : sc_core::sc_module(name), socket("socket"), bytes(size, 0),
          read_bytes(0), write_bytes(0) {
        socket.register_b_transport(this, &TestMemory::b_transport);
    }

    void b_transport(tlm::tlm_generic_payload & trans, sc_core::sc_time & delay) {
        uint64_t address = trans.get_address();
        size_t length = trans.get_data_length();
        if (!trans.get_data_ptr() || address > bytes.size() || length > bytes.size() - address) {
            trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
            return;
        }
        if (trans.is_read()) {
            memcpy(trans.get_data_ptr(), &bytes[(size_t)address], length);
            read_bytes += length;
        } else if (trans.is_write()) {
            memcpy(&bytes[(size_t)address], trans.get_data_ptr(), length);
            write_bytes += length;
        } else {
            trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
            return;
        }
        delay += sc_core::sc_time(1, sc_core::SC_NS);
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
};

struct CpuMaster : sc_core::sc_module {
    tlm_utils::simple_initiator_socket<CpuMaster> socket;

    CpuMaster(sc_core::sc_module_name name) : sc_core::sc_module(name), socket("socket") {}

    tlm::tlm_response_status access(tlm::tlm_command command, uint32_t offset,
                                    uint32_t & value, unsigned char * enables = NULL) {
        tlm::tlm_generic_payload trans;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        trans.set_command(command);
        trans.set_address(offset);
        trans.set_data_ptr(reinterpret_cast<unsigned char *>(&value));
        trans.set_data_length(4);
        trans.set_streaming_width(4);
        trans.set_byte_enable_ptr(enables);
        trans.set_byte_enable_length(enables ? 4 : 0);
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        socket->b_transport(trans, delay);
        return trans.get_response_status();
    }

    void write32(uint32_t offset, uint32_t value) {
        if (access(tlm::TLM_WRITE_COMMAND, offset, value) != tlm::TLM_OK_RESPONSE) {
            SC_REPORT_FATAL("CpuMaster", "register write failed");
        }
    }

    uint32_t read32(uint32_t offset) {
        uint32_t value = 0;
        if (access(tlm::TLM_READ_COMMAND, offset, value) != tlm::TLM_OK_RESPONSE) {
            SC_REPORT_FATAL("CpuMaster", "register read failed");
        }
        return value;
    }

    void write64(uint32_t offset, uint64_t value) {
        write32(offset, (uint32_t)value);
        write32(offset + 4, (uint32_t)(value >> 32));
    }

    uint64_t read64(uint32_t offset) {
        return (uint64_t)read32(offset) | ((uint64_t)read32(offset + 4) << 32);
    }
};

static int failures;

static void check(bool condition, const char * message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void store_u16(unsigned char * data, uint16_t value) {
    data[0] = (unsigned char)value;
    data[1] = (unsigned char)(value >> 8);
}

static void fill_fixture(TestMemory & memory, uint32_t rows, uint32_t nb,
                         uint64_t quants, uint64_t scales, uint64_t x) {
    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t block = 0; block < nb; ++block) {
            uint64_t index = (uint64_t)row * nb + block;
            store_u16(&memory.bytes[(size_t)(scales + index * 2)], 0x3c00);
            for (unsigned element = 0; element < 32; ++element) {
                memory.bytes[(size_t)(quants + index * 32 + element)] =
                    (unsigned char)(int8_t)((row * 17 + block * 7 + element * 3) % 31 - 15);
            }
        }
    }
    for (uint32_t block = 0; block < nb; ++block) {
        uint64_t base = x + (uint64_t)block * 34;
        store_u16(&memory.bytes[(size_t)base], 0x3800);
        for (unsigned element = 0; element < 32; ++element) {
            memory.bytes[(size_t)(base + 2 + element)] =
                (unsigned char)(int8_t)((block * 5 + element * 2) % 23 - 11);
        }
    }
}

static void configure(CpuMaster & cpu, uint64_t quants, uint64_t scales,
                      uint64_t x, uint64_t y, uint32_t rows, uint32_t nb) {
    cpu.write64(GEMV_REG_W_QUANTS, quants);
    cpu.write64(GEMV_REG_W_SCALES, scales);
    cpu.write64(GEMV_REG_X_NATIVE, x);
    cpu.write64(GEMV_REG_Y, y);
    cpu.write32(GEMV_REG_ROWS, rows);
    cpu.write32(GEMV_REG_NB, nb);
}

static uint32_t run_job(CpuMaster & cpu) {
    cpu.write32(GEMV_REG_CTRL, GEMV_CTRL_START);
    sc_core::sc_start(sc_core::sc_time(100, sc_core::SC_US));
    uint32_t control = cpu.read32(GEMV_REG_CTRL);
    check((control & (GEMV_CTRL_DONE | GEMV_CTRL_IDLE)) ==
          (GEMV_CTRL_DONE | GEMV_CTRL_IDLE), "job did not finish idle and done");
    check(cpu.read32(GEMV_REG_CTRL) == GEMV_CTRL_IDLE, "done was not clear-on-read");
    check(cpu.read32(GEMV_REG_STATUS_VALID) == 1, "status valid was not set");
    check(cpu.read32(GEMV_REG_STATUS_VALID) == 0, "status valid was not clear-on-read");
    return cpu.read32(GEMV_REG_STATUS);
}

int sc_main(int, char **) {
    gemv_log_level() = 0;
    CpuMaster cpu("cpu");
    GemvAccel accel("gemv");
    TestMemory memory("memory", 1 << 20);
    sc_core::sc_signal<bool> irq;
    cpu.socket.bind(accel.control_socket);
    accel.dma_socket.bind(memory.socket);
    accel.irq(irq);
    sc_core::sc_start(sc_core::SC_ZERO_TIME);

    check(cpu.read32(GEMV_REG_CTRL) == GEMV_CTRL_IDLE, "reset control is not idle");

    unsigned char enables[4] = {TLM_BYTE_ENABLED, TLM_BYTE_DISABLED,
                                TLM_BYTE_ENABLED, TLM_BYTE_DISABLED};
    uint32_t partial = 0x11223344;
    check(cpu.access(tlm::TLM_WRITE_COMMAND, GEMV_REG_ROWS, partial, enables) ==
          tlm::TLM_OK_RESPONSE, "byte-enabled register write failed");
    check(cpu.read32(GEMV_REG_ROWS) == 0x00220044, "byte enables were not applied");
    uint32_t unaligned = 0;
    check(cpu.access(tlm::TLM_READ_COMMAND, GEMV_REG_ROWS + 1, unaligned) ==
          tlm::TLM_ADDRESS_ERROR_RESPONSE, "unaligned register access was accepted");
    cpu.write64(GEMV_REG_W_QUANTS, UINT64_C(0x1122334455667780));
    check(cpu.read64(GEMV_REG_W_QUANTS) == UINT64_C(0x1122334455667780),
          "64-bit descriptor register lost a high or low word");

    const uint32_t rows = 2;
    const uint32_t nb = 2;
    const uint64_t quants = 0x1000;
    const uint64_t scales = 0x2000;
    const uint64_t x = 0x3000;
    const uint64_t y = 0x4000;
    fill_fixture(memory, rows, nb, quants, scales, x);
    configure(cpu, quants, scales, x, y, rows, nb);
    cpu.write32(GEMV_REG_IER, GEMV_IRQ_DONE);
    cpu.write32(GEMV_REG_GIE, 1);
    check(run_job(cpu) == GEMV_STATUS_OK, "valid job failed");
    check(irq.read() && cpu.read32(GEMV_REG_ISR) == GEMV_IRQ_DONE,
          "completion interrupt was not asserted");
    cpu.write32(GEMV_REG_ISR, GEMV_IRQ_DONE);
    sc_core::sc_start(sc_core::SC_ZERO_TIME);
    check(!irq.read(), "interrupt toggle-on-write did not clear the line");
    cpu.write32(GEMV_REG_GIE, 0);

    soc_q8r_mat matrix;
    memset(&matrix, 0, sizeof(matrix));
    matrix.rows = rows;
    matrix.nb = nb;
    matrix.quants = reinterpret_cast<int8_t *>(&memory.bytes[(size_t)quants]);
    matrix.scales = reinterpret_cast<uint16_t *>(&memory.bytes[(size_t)scales]);
    float expected[rows];
    soc_q8r_gemv_ref(&matrix, &memory.bytes[(size_t)x], expected);
    check(memcmp(&memory.bytes[(size_t)y], expected, sizeof(expected)) == 0,
          "GEMV output differs from soc_q8r_gemv_ref");

    memory.bytes[(size_t)quants] ^= 0x5a;
    soc_q8r_gemv_ref(&matrix, &memory.bytes[(size_t)x], expected);
    check(run_job(cpu) == GEMV_STATUS_OK, "repeated changed-weight job failed");
    check(memcmp(&memory.bytes[(size_t)y], expected, sizeof(expected)) == 0,
          "changed weights were not observed through DMA");

    float sentinel[rows] = {-123.5f, -123.5f};
    memcpy(&memory.bytes[(size_t)y], sentinel, sizeof(sentinel));
    store_u16(&memory.bytes[(size_t)x], 0x7c00);
    check(run_job(cpu) == GEMV_STATUS_BAD_X_SCALE, "invalid activation scale was accepted");
    check(memcmp(&memory.bytes[(size_t)y], sentinel, sizeof(sentinel)) == 0,
          "invalid activation job changed output");
    store_u16(&memory.bytes[(size_t)x], 0x3800);
    check(run_job(cpu) == GEMV_STATUS_OK, "recovery job failed");

    configure(cpu, quants, quants + 16, x, y, rows, nb);
    check(run_job(cpu) == GEMV_STATUS_BAD_SHAPE, "overlapping ranges were accepted");

    check(accel.completed_jobs() == 5, "unexpected completed job count");
    check(accel.dma_read_bytes() == memory.read_bytes, "DMA read accounting mismatch");
    check(accel.dma_write_bytes() == memory.write_bytes, "DMA write accounting mismatch");
    check(memory.read_bytes != 0 && memory.write_bytes != 0, "no DMA traffic observed");

    if (failures == 0) {
        printf("GEMV COSIM STANDALONE: PASS; jobs=%" PRIu64 " read=%" PRIu64
               " write=%" PRIu64 " simulated=%s\n",
               accel.completed_jobs(), accel.dma_read_bytes(), accel.dma_write_bytes(),
               sc_core::sc_time_stamp().to_string().c_str());
    } else {
        printf("GEMV COSIM STANDALONE: FAIL; failures=%d\n", failures);
    }
    return failures ? 1 : 0;
}
