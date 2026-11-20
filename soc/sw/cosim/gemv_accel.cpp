#include "gemv_accel.h"

#include <algorithm>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "soc_repack.h"

using sc_core::sc_time;
using sc_core::sc_time_stamp;

int & gemv_log_level() {
    static int level = 1;
    return level;
}

#define GEMV_LOG(level, ...) do { \
    if (gemv_log_level() >= (level)) { \
        printf("[%12s] GemvAccel: ", sc_time_stamp().to_string().c_str()); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static bool finite_half(uint16_t value) {
    return (value & 0x7c00u) != 0x7c00u;
}

static uint16_t load_u16(const unsigned char * data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t merge_bytes(uint32_t old_value, uint32_t new_value, uint32_t mask) {
    return (old_value & ~mask) | (new_value & mask);
}

GemvAccel::GemvAccel(sc_core::sc_module_name name, const GemvTiming & timing)
    : sc_core::sc_module(name),
      control_socket("control_socket"),
      dma_socket("dma_socket"),
      irq("irq"),
      timing_(timing),
      w_quants_(0),
      w_scales_(0),
      x_native_(0),
      y_(0),
      rows_(0),
      nb_(0),
      status_(0),
      gie_(0),
      ier_(0),
      isr_(0),
      start_(false),
      done_(false),
      idle_(true),
      ready_(false),
      auto_restart_(false),
      status_valid_(false),
      completed_jobs_(0),
      dma_read_bytes_(0),
      dma_write_bytes_(0) {
    memset(&active_, 0, sizeof(active_));
    irq.initialize(false);
    control_socket.register_b_transport(this, &GemvAccel::b_transport);
    SC_THREAD(worker);
}

void GemvAccel::b_transport(tlm::tlm_generic_payload & trans, sc_time & delay) {
    trans.set_dmi_allowed(false);
    if (trans.get_command() == tlm::TLM_IGNORE_COMMAND) {
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
        return;
    }
    if (trans.get_data_length() != 4 || (trans.get_address() & 3) != 0 ||
        trans.get_address() >= GEMV_CONTROL_BYTES ||
        (trans.get_streaming_width() != 0 && trans.get_streaming_width() < 4)) {
        trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        return;
    }

    unsigned char * data = trans.get_data_ptr();
    if (!data) {
        trans.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return;
    }
    uint32_t byte_mask = 0xffffffffu;
    const unsigned char * enables = trans.get_byte_enable_ptr();
    unsigned enable_length = trans.get_byte_enable_length();
    if (enables) {
        if (enable_length == 0) {
            trans.set_response_status(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
            return;
        }
        byte_mask = 0;
        for (unsigned i = 0; i < 4; ++i) {
            if (enables[i % enable_length] == TLM_BYTE_ENABLED) {
                byte_mask |= 0xffu << (8 * i);
            }
        }
    }

    uint32_t value = 0;
    memcpy(&value, data, sizeof(value));
    bool ok = false;
    uint32_t offset = (uint32_t)trans.get_address();
    if (trans.is_write()) {
        ok = write_register(offset, value, byte_mask, delay);
    } else if (trans.is_read()) {
        ok = read_register(offset, value);
        if (ok) {
            uint32_t original = 0;
            memcpy(&original, data, sizeof(original));
            value = merge_bytes(original, value, byte_mask);
            memcpy(data, &value, sizeof(value));
        }
    }
    delay += timing_.register_cycles * timing_.period();
    trans.set_response_status(ok ? tlm::TLM_OK_RESPONSE : tlm::TLM_ADDRESS_ERROR_RESPONSE);
}

bool GemvAccel::read_register(uint32_t offset, uint32_t & value) {
    switch (offset) {
        case GEMV_REG_CTRL:
            value = (start_ ? (uint32_t)GEMV_CTRL_START : 0u) |
                    (done_ ? (uint32_t)GEMV_CTRL_DONE : 0u) |
                    (idle_ ? (uint32_t)GEMV_CTRL_IDLE : 0u) |
                    (ready_ ? (uint32_t)GEMV_CTRL_READY : 0u) |
                    (auto_restart_ ? (uint32_t)GEMV_CTRL_AUTO_RESTART : 0u);
            done_ = false;
            ready_ = false;
            return true;
        case GEMV_REG_GIE: value = gie_; return true;
        case GEMV_REG_IER: value = ier_; return true;
        case GEMV_REG_ISR: value = isr_; return true;
        case GEMV_REG_W_QUANTS: value = (uint32_t)w_quants_; return true;
        case GEMV_REG_W_QUANTS + 4: value = (uint32_t)(w_quants_ >> 32); return true;
        case GEMV_REG_W_SCALES: value = (uint32_t)w_scales_; return true;
        case GEMV_REG_W_SCALES + 4: value = (uint32_t)(w_scales_ >> 32); return true;
        case GEMV_REG_X_NATIVE: value = (uint32_t)x_native_; return true;
        case GEMV_REG_X_NATIVE + 4: value = (uint32_t)(x_native_ >> 32); return true;
        case GEMV_REG_Y: value = (uint32_t)y_; return true;
        case GEMV_REG_Y + 4: value = (uint32_t)(y_ >> 32); return true;
        case GEMV_REG_ROWS: value = rows_; return true;
        case GEMV_REG_NB: value = nb_; return true;
        case GEMV_REG_STATUS: value = status_; return true;
        case GEMV_REG_STATUS_VALID:
            value = status_valid_ ? 1 : 0;
            status_valid_ = false;
            return true;
        default: value = 0; return true;
    }
}

bool GemvAccel::write_register(uint32_t offset, uint32_t value, uint32_t byte_mask,
                               const sc_time & delay) {
    uint32_t old_value;
    uint32_t merged;
    switch (offset) {
        case GEMV_REG_CTRL:
            old_value = (start_ ? (uint32_t)GEMV_CTRL_START : 0u) |
                        (auto_restart_ ? (uint32_t)GEMV_CTRL_AUTO_RESTART : 0u);
            merged = merge_bytes(old_value, value, byte_mask);
            auto_restart_ = (merged & GEMV_CTRL_AUTO_RESTART) != 0;
            if (merged & GEMV_CTRL_START) {
                start_ = true;
                if (idle_) launch(delay);
            } else if (!auto_restart_) {
                start_ = false;
            }
            return true;
        case GEMV_REG_GIE:
            gie_ = merge_bytes(gie_, value, byte_mask) & 1;
            update_irq();
            return true;
        case GEMV_REG_IER:
            ier_ = merge_bytes(ier_, value, byte_mask) & 3;
            return true;
        case GEMV_REG_ISR:
            isr_ ^= value & byte_mask & 3;
            update_irq();
            return true;
        case GEMV_REG_W_QUANTS:
            w_quants_ = (w_quants_ & 0xffffffff00000000ull) |
                        merge_bytes((uint32_t)w_quants_, value, byte_mask);
            return true;
        case GEMV_REG_W_QUANTS + 4:
            w_quants_ = (w_quants_ & 0xffffffffull) |
                        ((uint64_t)merge_bytes((uint32_t)(w_quants_ >> 32), value, byte_mask) << 32);
            return true;
        case GEMV_REG_W_SCALES:
            w_scales_ = (w_scales_ & 0xffffffff00000000ull) |
                        merge_bytes((uint32_t)w_scales_, value, byte_mask);
            return true;
        case GEMV_REG_W_SCALES + 4:
            w_scales_ = (w_scales_ & 0xffffffffull) |
                        ((uint64_t)merge_bytes((uint32_t)(w_scales_ >> 32), value, byte_mask) << 32);
            return true;
        case GEMV_REG_X_NATIVE:
            x_native_ = (x_native_ & 0xffffffff00000000ull) |
                        merge_bytes((uint32_t)x_native_, value, byte_mask);
            return true;
        case GEMV_REG_X_NATIVE + 4:
            x_native_ = (x_native_ & 0xffffffffull) |
                        ((uint64_t)merge_bytes((uint32_t)(x_native_ >> 32), value, byte_mask) << 32);
            return true;
        case GEMV_REG_Y:
            y_ = (y_ & 0xffffffff00000000ull) |
                 merge_bytes((uint32_t)y_, value, byte_mask);
            return true;
        case GEMV_REG_Y + 4:
            y_ = (y_ & 0xffffffffull) |
                 ((uint64_t)merge_bytes((uint32_t)(y_ >> 32), value, byte_mask) << 32);
            return true;
        case GEMV_REG_ROWS: rows_ = merge_bytes(rows_, value, byte_mask); return true;
        case GEMV_REG_NB: nb_ = merge_bytes(nb_, value, byte_mask); return true;
        case GEMV_REG_STATUS:
        case GEMV_REG_STATUS_VALID:
            return true;
        default:
            return true;
    }
}

GemvAccel::Descriptor GemvAccel::snapshot() const {
    Descriptor descriptor;
    descriptor.w_quants = w_quants_;
    descriptor.w_scales = w_scales_;
    descriptor.x_native = x_native_;
    descriptor.y = y_;
    descriptor.rows = rows_;
    descriptor.nb = nb_;
    return descriptor;
}

void GemvAccel::launch(const sc_time & delay) {
    active_ = snapshot();
    idle_ = false;
    done_ = false;
    ready_ = false;
    if (!auto_restart_) start_ = false;
    start_event_.notify(delay);
}

void GemvAccel::worker() {
    for (;;) {
        wait(start_event_);
        uint32_t result = execute(active_);
        status_ = result;
        status_valid_ = true;
        done_ = true;
        ready_ = true;
        idle_ = true;
        completed_jobs_++;
        if (ier_ & GEMV_IRQ_DONE) isr_ ^= GEMV_IRQ_DONE;
        if (ier_ & GEMV_IRQ_READY) isr_ ^= GEMV_IRQ_READY;
        update_irq();
        GEMV_LOG(1, "DONE rows=%u nb=%u status=%u", active_.rows, active_.nb, result);

        if (start_) {
            launch(sc_core::SC_ZERO_TIME);
        }
    }
}

static bool range_inside(uint64_t address, uint64_t bytes) {
    uint64_t end = GEMV_DDR_BASE + GEMV_DDR_BYTES;
    return bytes != 0 && address < end &&
           bytes <= end - address;
}

static bool ranges_overlap(uint64_t a, uint64_t a_bytes, uint64_t b, uint64_t b_bytes) {
    return a < b + b_bytes && b < a + a_bytes;
}

bool GemvAccel::validate(const Descriptor & descriptor) const {
    if (descriptor.rows > GEMV_MAX_ROWS || descriptor.nb > GEMV_MAX_NB ||
        (descriptor.rows != 0 && descriptor.nb == 0)) {
        return false;
    }
    if (descriptor.rows == 0) return true;

    uint64_t addresses[4] = {
        descriptor.w_quants, descriptor.w_scales, descriptor.x_native, descriptor.y
    };
    uint64_t bytes[4] = {
        (uint64_t)descriptor.rows * descriptor.nb * 32,
        (uint64_t)descriptor.rows * descriptor.nb * 2,
        (uint64_t)descriptor.nb * 34,
        (uint64_t)descriptor.rows * 4
    };
    unsigned alignment[4] = {16, 2, 1, 4};
    for (unsigned i = 0; i < 4; ++i) {
        if ((addresses[i] % alignment[i]) != 0 || !range_inside(addresses[i], bytes[i])) {
            return false;
        }
        for (unsigned j = 0; j < i; ++j) {
            if (ranges_overlap(addresses[i], bytes[i], addresses[j], bytes[j])) return false;
        }
    }
    return true;
}

bool GemvAccel::dma_transfer(tlm::tlm_command command, uint64_t address,
                             void * data, size_t bytes) {
    unsigned char * cursor = static_cast<unsigned char *>(data);
    while (bytes != 0) {
        size_t boundary = 4096 - (size_t)(address & 4095);
        size_t chunk = std::min(bytes, boundary);
        tlm::tlm_generic_payload trans;
        sc_time delay = sc_core::SC_ZERO_TIME;
        trans.set_command(command);
        trans.set_address(address);
        trans.set_data_ptr(cursor);
        trans.set_data_length((unsigned)chunk);
        trans.set_streaming_width((unsigned)chunk);
        trans.set_byte_enable_ptr(NULL);
        trans.set_dmi_allowed(false);
        trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        dma_socket->b_transport(trans, delay);
        if (delay != sc_core::SC_ZERO_TIME) wait(delay);
        if (trans.is_response_error()) {
            GEMV_LOG(0, "DMA %s failed at 0x%" PRIx64 " (%s)",
                     command == tlm::TLM_READ_COMMAND ? "read" : "write", address,
                     trans.get_response_string().c_str());
            return false;
        }
        if (command == tlm::TLM_READ_COMMAND) dma_read_bytes_ += chunk;
        else dma_write_bytes_ += chunk;
        address += chunk;
        cursor += chunk;
        bytes -= chunk;
    }
    return true;
}

uint32_t GemvAccel::execute(const Descriptor & descriptor) {
    if (!validate(descriptor)) return GEMV_STATUS_BAD_SHAPE;
    if (descriptor.rows == 0) {
        wait(timing_.period());
        return GEMV_STATUS_OK;
    }

    std::vector<unsigned char> x((size_t)descriptor.nb * 34);
    std::vector<float> x_scales(descriptor.nb);
    if (!dma_transfer(tlm::TLM_READ_COMMAND, descriptor.x_native, x.data(), x.size())) {
        return GEMV_STATUS_BAD_SHAPE;
    }
    for (uint32_t block = 0; block < descriptor.nb; ++block) {
        uint16_t bits = load_u16(&x[(size_t)block * 34]);
        if (!finite_half(bits)) return GEMV_STATUS_BAD_X_SCALE;
        x_scales[block] = soc_fp16_to_fp32(bits);
    }
    unsigned activation_cycles = 51 + (descriptor.nb - 1) * 42;
    wait(activation_cycles * timing_.period());

    std::vector<unsigned char> quants((size_t)descriptor.nb * 32);
    std::vector<unsigned char> scale_bytes((size_t)descriptor.nb * 2);
    std::vector<float> w_scales(descriptor.nb);
    for (uint32_t row = 0; row < descriptor.rows; ++row) {
        uint64_t block_base = (uint64_t)row * descriptor.nb;
        if (!dma_transfer(tlm::TLM_READ_COMMAND,
                          descriptor.w_scales + block_base * 2,
                          scale_bytes.data(), scale_bytes.size())) {
            return GEMV_STATUS_BAD_SHAPE;
        }
        for (uint32_t block = 0; block < descriptor.nb; ++block) {
            uint16_t bits = load_u16(&scale_bytes[(size_t)block * 2]);
            if (!finite_half(bits)) return GEMV_STATUS_BAD_W_SCALE;
            w_scales[block] = soc_fp16_to_fp32(bits);
        }
        if (!dma_transfer(tlm::TLM_READ_COMMAND,
                          descriptor.w_quants + block_base * 32,
                          quants.data(), quants.size())) {
            return GEMV_STATUS_BAD_SHAPE;
        }

        float sum = 0.0f;
        for (uint32_t block = 0; block < descriptor.nb; ++block) {
            int32_t integer_sum = 0;
            const int8_t * wq = reinterpret_cast<const int8_t *>(&quants[(size_t)block * 32]);
            const int8_t * xq = reinterpret_cast<const int8_t *>(&x[(size_t)block * 34 + 2]);
            for (unsigned element = 0; element < 32; ++element) {
                integer_sum += (int32_t)wq[element] * (int32_t)xq[element];
            }
            float scale_product = w_scales[block] * x_scales[block];
            float term = (float)integer_sum * scale_product;
            sum = sum + term;
        }
        unsigned row_cycles = 6 + descriptor.nb * 3 + 25 + descriptor.nb * 10;
        wait(row_cycles * timing_.period());
        if (!dma_transfer(tlm::TLM_WRITE_COMMAND, descriptor.y + (uint64_t)row * 4,
                          &sum, sizeof(sum))) {
            return GEMV_STATUS_BAD_SHAPE;
        }
    }
    return GEMV_STATUS_OK;
}

void GemvAccel::update_irq() {
    irq.write((gie_ & 1) != 0 && isr_ != 0);
}
