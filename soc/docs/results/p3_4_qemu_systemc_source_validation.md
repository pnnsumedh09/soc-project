# P3.4 QEMU/SystemC source validation

Date: 2026-11-20. Branch: `qemu-gemv-ng`. Host: Windows workspace without SystemC or the course QEMU image.

## Added source

- `soc/sw/cosim/gemv_accel.{h,cpp}`: register target plus bus-mastering DMA initiator and sequential GEMV worker.
- `soc/sw/cosim/gemv_regs.h`: control map, status values, limits and the 2 GiB guest DDR contract.
- `soc/sw/cosim/tests/tb_gemv_accel.cpp`: standalone TLM memory/register/arithmetic test.
- `soc/sw/cosim/cosim/`: ZynqMP platform, installer and `Makefile.gemv` integration.
- `soc/sw/cosim/README.md`: build/run sequence, verification boundaries and guest DMA prerequisites.

The implementation keeps all weight, scale, activation and output traffic on the initiator socket. It validates shape, alignment, complete DDR ranges and non-overlap before starting DMA. Arithmetic preserves the existing block order and uses the shared FP16 conversion. The platform maps control at `0xA0000000/64KiB`, binds CPU traffic through HPM0, binds GEMV DMA directly to HP0, connects IRQ0 and calls `tie_off()` only after those bindings.

## Local checks

| Check | Result |
|---|---|
| Sparse checkout branch/commit and cleanliness before edits | PASS: `soc-gemv-accel` at `7606735a`; working branch `qemu-gemv-ng` |
| Focused checkout | PASS: 67 relevant project files plus repository instructions; unrelated tracked files remain in Git and were not deleted |
| `git diff --check` | PASS |
| Standalone Makefile dry-run | PASS: expected C/C++ compile and SystemC link commands generated |
| Course-platform Makefile dry-run in this checkout | NOT APPLICABLE: it intentionally includes the external demo's base `Makefile` after installation |
| C++ syntax with official SystemC 2.3.4 headers, warnings as errors | PASS: model and standalone testbench |
| SystemC link and standalone execution | NOT RUN: a built SystemC library is absent on this host |
| QEMU register/DMA jobs | NOT RUN: course QEMU image and `systemctlm-cosim-demo` are external assets |

No RTL timing, board behavior, QEMU DMA allocator or full-model result is claimed.

## Required next evidence

1. On LRC, run `make -C soc/sw/cosim clean test SYSTEMC=/usr/local/packages/systemc-2.3.4` and preserve the complete log.
2. Install/build `zynqmp_gemv` in the known-working course platform and correct any version-specific API/build differences without changing the register or arithmetic contract.
3. Boot QEMU first and verify reset IDLE plus descriptor readback without START.
4. Inspect the guest kernel/device tree and choose a real allocator/driver. Never pass a Linux virtual address or an unreserved `/dev/mem` address.
5. Run a tiny physical-address DMA job and then reproduce `board_smoke.c` before adding the llama.cpp QEMU backend.
