#define SC_INCLUDE_DYNAMIC_PROCESSES

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "systemc.h"
#include "tlm_utils/tlm_quantumkeeper.h"

using namespace sc_core;
using namespace sc_dt;
using namespace std;

#include "debugdev.h"
#include "soc/interconnect/iconnect.h"
#include "soc/xilinx/zynqmp/xilinx-zynqmp.h"

#include "gemv_accel.h"

#define NR_MASTERS 1
#define NR_DEVICES 2

SC_MODULE(Top) {
    SC_HAS_PROCESS(Top);

    iconnect<NR_MASTERS, NR_DEVICES> bus;
    xilinx_zynqmp zynq;
    debugdev debug;
    GemvAccel gemv;
    sc_signal<bool> rst;

    void pull_reset() {
        rst.write(true);
        wait(1, SC_US);
        rst.write(false);
    }

    Top(sc_module_name name, const char * socket_description, sc_time quantum)
        : sc_module(name),
          bus("bus"),
          zynq("zynq", socket_description),
          debug("debug"),
          gemv("gemv"),
          rst("rst") {
        m_qk.set_global_quantum(quantum);
        zynq.rst(rst);

        bus.memmap(0xa0800000ull, 0x100 - 1,
                   ADDRMODE_RELATIVE, -1, debug.socket);
        bus.memmap(GEMV_CONTROL_BASE, GEMV_CONTROL_BYTES - 1,
                   ADDRMODE_RELATIVE, -1, gemv.control_socket);
        zynq.s_axi_hpm_fpd[0]->bind(*(bus.t_sk[0]));

        gemv.dma_socket.bind(*(zynq.s_axi_hp_fpd[0]));
        gemv.irq(zynq.pl2ps_irq[0]);
        debug.irq[0](zynq.pl2ps_irq[1]);

        zynq.tie_off();

        printf("zynqmp_gemv: control 0x%08x/0x%x, DMA via HP0 DDR, irq pl2ps_irq[0]\n",
               GEMV_CONTROL_BASE, GEMV_CONTROL_BYTES);
        SC_THREAD(pull_reset);
    }

private:
    tlm_utils::tlm_quantumkeeper m_qk;
};

static void usage() {
    cout << "usage: zynqmp_gemv <socket-path> <sync-quantum-ns> [-v <0..3>]" << endl;
}

int sc_main(int argc, char * argv[]) {
    if (argc < 3) {
        usage();
        return EXIT_FAILURE;
    }
    uint64_t sync_quantum = strtoull(argv[2], NULL, 10);
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "-v") && i + 1 < argc) {
            gemv_log_level() = atoi(argv[++i]);
        } else {
            usage();
            return EXIT_FAILURE;
        }
    }

    sc_set_time_resolution(1, SC_PS);
    new Top("top", argv[1], sc_time((double)sync_quantum, SC_NS));
    sc_start();
    return 0;
}
