#ifndef SOC_COSIM_GEMV_ACCEL_H
#define SOC_COSIM_GEMV_ACCEL_H

#include <stddef.h>
#include <stdint.h>
#include <systemc.h>
#include <tlm.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>

#include "gemv_regs.h"

struct GemvTiming {
    double clock_ns;
    unsigned register_cycles;

    GemvTiming(double clock_ns_ = 6.67, unsigned register_cycles_ = 1)
        : clock_ns(clock_ns_), register_cycles(register_cycles_) {}

    sc_core::sc_time period() const {
        return sc_core::sc_time(clock_ns, sc_core::SC_NS);
    }
};

int & gemv_log_level();

SC_MODULE(GemvAccel) {
    tlm_utils::simple_target_socket<GemvAccel> control_socket;
    tlm_utils::simple_initiator_socket<GemvAccel> dma_socket;
    sc_core::sc_out<bool> irq;

    SC_HAS_PROCESS(GemvAccel);
    GemvAccel(sc_core::sc_module_name name, const GemvTiming & timing = GemvTiming());

    uint64_t completed_jobs() const { return completed_jobs_; }
    uint64_t dma_read_bytes() const { return dma_read_bytes_; }
    uint64_t dma_write_bytes() const { return dma_write_bytes_; }

private:
    struct Descriptor {
        uint64_t w_quants;
        uint64_t w_scales;
        uint64_t x_native;
        uint64_t y;
        uint32_t rows;
        uint32_t nb;
    };

    GemvTiming timing_;
    uint64_t w_quants_;
    uint64_t w_scales_;
    uint64_t x_native_;
    uint64_t y_;
    uint32_t rows_;
    uint32_t nb_;
    uint32_t status_;
    uint32_t gie_;
    uint32_t ier_;
    uint32_t isr_;
    bool start_;
    bool done_;
    bool idle_;
    bool ready_;
    bool auto_restart_;
    bool status_valid_;
    Descriptor active_;
    sc_core::sc_event start_event_;
    uint64_t completed_jobs_;
    uint64_t dma_read_bytes_;
    uint64_t dma_write_bytes_;

    void b_transport(tlm::tlm_generic_payload & trans, sc_core::sc_time & delay);
    bool read_register(uint32_t offset, uint32_t & value);
    bool write_register(uint32_t offset, uint32_t value, uint32_t byte_mask,
                        const sc_core::sc_time & delay);
    void launch(const sc_core::sc_time & delay);
    void worker();
    uint32_t execute(const Descriptor & descriptor);
    bool dma_transfer(tlm::tlm_command command, uint64_t address, void * data, size_t bytes);
    bool validate(const Descriptor & descriptor) const;
    Descriptor snapshot() const;
    void update_irq();
};

#endif
