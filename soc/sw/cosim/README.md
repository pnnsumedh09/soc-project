# GEMV QEMU/SystemC behavioral model

This directory models the selected sequential GEMV accelerator for HW/SW protocol validation. It is not generated RTL, a timing-closure result, or a physical DDR performance model.

## Implemented boundary

- AXI-Lite-style TLM target at `0xA0000000`, with the generated HLS register offsets, 32-bit little-endian accesses, byte enables, clear-on-read DONE/READY/status-valid state, and GIE/IER/ISR behavior.
- Bus-mastering TLM initiator for all activation, split weight, scale, and output accesses. Guest physical addresses are never cast to host pointers.
- Descriptor shape, alignment, complete 2 GiB DDR range, overflow, and non-overlap checks before DMA.
- Ordered Q8_0 arithmetic matching `soc_q8r_gemv_ref`, including finite FP16 scale checks and separate multiply/add compilation.
- A behavioral delay based on the sequential 150 MHz HLS schedule. It is an estimate layered on the memory target's transaction delay.
- Standalone SystemC memory test covering registers, byte enables, exact GEMV output, repeated jobs, changed weights, invalid activation scale with unchanged output, recovery, bad ranges, and observed DMA traffic.
- ZynqMP platform mapping with CPU control traffic through HPM0 and accelerator DMA through HP0.

## Standalone test

On the LRC machine with SystemC 2.3.4:

```sh
module load systemc/2.3.4
make -C soc/sw/cosim clean test SYSTEMC=/usr/local/packages/systemc-2.3.4
```

The expected final line starts with `GEMV COSIM STANDALONE: PASS`.

## Install into the course platform

Use a known-working course `systemctlm-cosim-demo` checkout with its `libsystemctlm-soc` submodule initialized:

```sh
module load systemc/2.3.4
sh soc/sw/cosim/cosim/setup.sh /misc/scratch/$USER/GEMV_QEMU/systemctlm-cosim-demo
make -C /misc/scratch/$USER/GEMV_QEMU/systemctlm-cosim-demo \
    -f Makefile.gemv SYSTEMC=/usr/local/packages/systemc-2.3.4 zynqmp_gemv
```

Start QEMU first, then run the model on the same LRC host:

```sh
cd /misc/scratch/$USER/GEMV_QEMU
/home/projects/gerstl/ece382m/qemu-boot -c

cd systemctlm-cosim-demo
./zynqmp_gemv unix:../tmp/qemu-rport-_amba@0_cosim@0 1000000 -v 2
```

The model maps the GEMV control bank at `0xA0000000/64KiB`. It binds its initiator directly to `zynq.s_axi_hp_fpd[0]` before `zynq.tie_off()`.

## Guest DMA requirement

Linux virtual addresses are not device addresses. Do not pass `malloc` pointers to the HAL. The guest needs a course-image-compatible allocator or reserved-memory driver that returns both a CPU mapping and a guest physical/DMA address below `0x80000000` and keeps the allocation alive until IDLE.

That guest allocator is intentionally not guessed here. The course image, kernel configuration, device tree, module policy, and available DMA facilities are external to this repository. Before implementing it, capture:

```sh
uname -a
cat /proc/cmdline
grep -iE 'cma|reserved' /proc/meminfo
ls -l /dev/uio* /dev/dma_heap/* /dev/udmabuf* 2>/dev/null
find /sys/firmware/devicetree/base/reserved-memory -maxdepth 2 -type f -print 2>/dev/null
```

Select one verified path:

1. A course-provided DMA allocator/device, if present.
2. A small platform driver using `dma_alloc_coherent()` and `mmap()`, with an ioctl returning the DMA address.
3. A device-tree reserved region plus a driver that owns and maps it.

Do not use an unreserved `/dev/mem` range: Linux or firmware may allocate it concurrently.

## Verification sequence

1. Run the standalone test.
2. Boot QEMU and read `CTRL`; reset value must include IDLE.
3. Run destructive descriptor-register readback without START.
4. Add the verified guest DMA adapter and run a 1-2 row, 1-2 block job.
5. Reproduce `soc/sw/hal2/board_smoke.c`.
6. Add a QEMU backend to the existing GEMV hook only after the smoke boundary passes.

Save commands, commit, tool versions, shapes, status, exact error comparison, DMA addresses, and simulated time under `soc/docs/results/`.
