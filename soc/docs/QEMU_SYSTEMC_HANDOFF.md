# GEMV QEMU/SystemC teammate handoff

Updated: 2026-11-20. This document is for the teammate implementing P3.4, the
QEMU/SystemC behavioral model of the current bus-mastering GEMV accelerator.
`../../SOC_PROJECT_PLAN.md` is the authoritative overall project plan.

## 1. Clone the correct repository and branch

This project is in the private fork, not upstream `ggml-org/llama.cpp`.
Use one of these commands depending on GitHub authentication:

```bash
cd /misc/scratch/$USER
git clone --branch soc-gemv-accel --single-branch \
  https://github.com/pnnsumedh09/soc-project.git soc-project
cd soc-project
```

or:

```bash
cd /misc/scratch/$USER
git clone --branch soc-gemv-accel --single-branch \
  git@github.com:pnnsumedh09/soc-project.git soc-project
cd soc-project
```

Verify the checkout before editing:

```bash
git status --short
git branch --show-current
git log -1 --oneline
git remote -v
```

The branch must be `soc-gemv-accel`. A fresh clone should be clean. There are
no active Git submodules in this repository (`.gitmodules` is empty), so a
recursive clone is not required.

Create a teammate branch rather than committing directly to the shared branch:

```bash
git switch -c qemu-gemv-<initials>
git push -u origin qemu-gemv-<initials>
```

Do not push to the upstream llama.cpp repository.

## 2. External course environment is not stored in this repository

The QEMU disk image, firmware/device-tree files and
`systemctlm-cosim-demo` checkout are course assets. They are not part of this
Git repository. Reuse the working Lab 3/tutorial environment or obtain it using
the course instructions. Keep a layout like:

```text
/misc/scratch/$USER/GEMV_QEMU/
├── images/linux/                 # Course QEMU Linux/firmware files
├── systemctlm-cosim-demo/        # Course platform checkout
├── tmp/                          # Remote-port socket created by qemu-boot
└── soc-project/                  # This repository
```

The platform's `libsystemctlm-soc` submodule must be initialized:

```bash
cd /misc/scratch/$USER/GEMV_QEMU/systemctlm-cosim-demo
git submodule update --init --recursive
test -f libsystemctlm-soc/soc/xilinx/zynqmp/xilinx-zynqmp.h
```

Do not guess a new URL for `systemctlm-cosim-demo`; use the exact course
checkout/tutorial URL or an existing known-working Lab 3 copy.

## 3. Read these files first

Current design and status:

- `SOC_PROJECT_PLAN.md`, especially sections 4.3, 5, 6, P3.4 and P4.
- `soc/docs/NEXT_STEPS.md`, section "QEMU/SystemC decision".
- `soc/docs/PRD.md` for the memory, numerical and verification contracts.

Current GEMV hardware/software contract:

- `soc/hw/gemv/gemv.cpp` and `gemv.h`: selected sequential hardware behavior.
- `soc/sw/repack/soc_repack.h` and `.c`: split weight layout and golden
  `soc_q8r_gemv_ref` arithmetic.
- `soc/sw/hal2/soc_gemv_hal.h` and `.c`: exact register offsets and job
  submission sequence.
- `soc/sw/gemv/soc_gemv.c`: llama.cpp hook, repacked-weight cache, CPU fallback
  and verification.
- `soc/sw/hal2/board_smoke.c`: small end-to-end GEMV acceptance sequence to
  reproduce in simulation.
- `soc/docs/results/p3_gemv_source_validation.md` and
  `soc/docs/results/hls_gemv_f150_b64_o8_v1.md`: arithmetic and HLS evidence.
- `soc/docs/results/p4_integration_source_validation.md`: current Vivado/board
  topology and what it does and does not prove.

Read-only Lab 3 reference:

- `soc/lab3_ref/LAB3_README.md`.
- `soc/lab3_ref/part3/dot_accel.h`.
- `soc/lab3_ref/part3/cosim/zynqmp_dot.cc`.
- `soc/lab3_ref/part3/cosim/setup.sh` and `Makefile.dot`.
- `soc/lab3_ref/part4/` for the old dot-product HAL/driver flow.

Do not modify `soc/lab3_ref/`; it is preserved reference material. Put the new
implementation under `soc/sw/cosim/` as specified by plan task P3.4.

## 4. What is already complete

- Q8_0 repack/golden GEMV, llama.cpp hook/cache and emulation backend pass.
- Sequential HLS GEMV C simulation, synthesis and generated-RTL co-simulation
  pass bit-exactly. The selected design has no row interleaving.
- Generated AXI-Lite register RTL test passes.
- Ultra96-V2 block design validates: control is at `0xA0000000/64KiB`; four
  GEMV masters reach HP0 DDR at `0x00000000/2GiB`; clock is 150 MHz; reset and
  interrupt are connected.
- Portable HAL source and local fake-MMIO tests pass.

Not complete yet:

- FPGA implementation/bitstream and routed timing.
- Physical-board DMA/coherency and full inference.
- A course-image kernel DMA allocator/driver.
- This new GEMV QEMU/SystemC model.

QEMU/SystemC is a behavioral HW/SW protocol check. It does not simulate the
generated HLS RTL, prove FPGA timing or measure real DDR/cache behavior.

## 5. Do not merely rerun the old Lab 3 dot accelerator

The old `DotAccel` is a pure bus slave at `0xA0100000`. The ARM copies every
input into accelerator scratchpad registers/memory. Running it again verifies
only the old Lab 3 design.

The current project uses a bus-mastering GEMV accelerator:

```text
QEMU ARM software
    |
    | AXI control writes through M_AXI_HPM0_FPD
    v
GEMV control target at 0xA0000000
    |
    | GEMV-initiated loads/stores through S_AXI_HP0_FPD
    v
QEMU guest physical DDR 0x00000000..0x7fffffff
```

The SystemC model therefore needs both:

1. A TLM target socket for CPU register access.
2. A TLM initiator socket for GEMV reads/writes to guest physical memory.

## 6. Exact v1 contract to model

Keep the selected design simple and sequential. Do not add row interleaving,
double buffering or a new numerical order in this task.

### Control region

Base `0xA0000000`, size `0x10000`:

| Offset | Meaning |
|---:|---|
| `0x00` | HLS control: start, done, idle, ready, auto-restart, interrupt |
| `0x04` | Global interrupt enable |
| `0x08` | IP interrupt enable |
| `0x0c` | IP interrupt status |
| `0x10/0x14` | 64-bit weight-quants address |
| `0x20/0x24` | 64-bit weight-scales address |
| `0x30/0x34` | 64-bit native-Q8_0 activation address |
| `0x40/0x44` | 64-bit FP32 output address |
| `0x50` | Rows |
| `0x58` | Q8_0 blocks per row (`nb`) |
| `0x60` | Kernel status |
| `0x64` | Status-valid flag |

Use `soc/sw/hal2/soc_gemv_hal.h` as the software authority and the generated
header/checks described under `soc/hw/gemv/registers/` as the hardware authority.

### Memory layout and bounds

- `rows <= 151936`.
- `nb <= 152`; a nonzero job requires `nb > 0`.
- Weight quants: `rows * nb * 32` bytes, split array, 16-byte aligned.
- Weight scales: `rows * nb * 2` bytes, little-endian FP16, split array.
- Activation: `nb * 34` bytes in native Q8_0 blocks: FP16 scale followed by
  32 signed bytes.
- Output: `rows * 4` bytes, little-endian FP32.
- All device addresses and complete ranges must be inside guest physical DDR
  `0x00000000..0x7fffffff` and must not overlap.

Status codes match `soc/hw/gemv/gemv.h`: success, bad shape, invalid activation
scale and invalid weight scale. Preserve ordered FP32 accumulation with separate
multiply/add and no FMA; compare against `soc_q8r_gemv_ref`.

### Start/completion behavior

Match the HLS/HAL behavior closely enough that the unmodified portable HAL can:

1. Observe idle.
2. Write the four addresses, `rows` and `nb`.
3. Write start.
4. Poll done and idle.
5. Read status-valid and status.

Polling is the required first path. The current Vivado design also wires the
accelerator interrupt to `pl_ps_irq0[0]`; interrupt support can follow after the
polling path passes.

## 7. Recommended source layout and implementation steps

Create, at minimum:

```text
soc/sw/cosim/
├── README.md
├── gemv_accel.h
├── gemv_accel.cpp
├── gemv_regs.h
├── cosim/
│   ├── setup.sh
│   ├── Makefile.gemv
│   └── zynqmp_gemv.cc
└── tests/
    └── ... standalone TLM/memory test ...
```

Suggested order:

1. Implement the register target with bounds, byte enables, little-endian
   32-bit accesses and HLS-like start/done/idle/status semantics.
2. Implement a worker process that snapshots a descriptor on start.
3. Implement an initiator load/store helper. All weights, scales, activation
   and output must travel as TLM transactions through this socket. Never cast a
   guest physical address to a host pointer.
4. Reuse the split-layout/golden arithmetic or an equivalent ordered
   implementation. Validate against `soc_q8r_gemv_ref`.
5. Add modeled delay using the existing HLS reports, but correctness comes
   first. Label timing as a behavioral estimate, not measured RTL latency.
6. Integrate the register target on the CPU-side bus at `0xA0000000`.
7. Bind the GEMV initiator toward Zynq `s_axi_hp_fpd[0]`, following the bus-master
   example in the Lab 3 handout and the port declarations in
   `libsystemctlm-soc/soc/xilinx/zynqmp/xilinx-zynqmp.h`.
8. Do not call `zynq.tie_off()` until after every socket used by the new model
   has been bound.

The old `zynqmp_dot.cc` is a useful platform skeleton, but its master/device
counts, map, IRQ and tie-offs must be redesigned for the GEMV initiator.

## 8. Guest DMA memory is part of the task

The SystemC initiator accesses guest physical addresses. A Linux userspace
pointer is not a physical/DMA address. Provide a simulation-specific path that:

- allocates or reserves physically addressable guest memory;
- exposes both its CPU mapping and guest physical/DMA address;
- keeps it alive until the simulated accelerator is idle;
- passes device addresses, not virtual pointers, to the HAL;
- stays below `0x80000000`.

The existing `soc/sw/hal2/soc_gemv_backend_fpga.c` is an optional physical-board
UIO/u-dma-buf adapter and may not match the QEMU guest. Reuse
`soc_gemv_hal.c`, but implement a QEMU-appropriate allocator/driver adapter.
A small custom kernel driver using a reserved region or DMA allocation is one
possible solution; document the actual choice and device-tree changes.

## 9. Build and start the baseline environment

First prove the existing course environment works. From the workspace root:

```bash
module load systemc/2.3.4
cd /misc/scratch/$USER/GEMV_QEMU
/home/projects/gerstl/ece382m/qemu-boot -c
```

Start the matching SystemC executable in a second terminal on the same LRC
machine. For the old environment sanity check only, the preserved dot model can
be installed and built with:

```bash
module load systemc/2.3.4
cd /misc/scratch/$USER/GEMV_QEMU
sh soc-project/soc/lab3_ref/part3/cosim/setup.sh systemctlm-cosim-demo
make -C systemctlm-cosim-demo -f Makefile.dot \
  SYSTEMC=/usr/local/packages/systemc-2.3.4 zynqmp_dot
cd systemctlm-cosim-demo
./zynqmp_dot unix:../tmp/qemu-rport-_amba@0_cosim@0 1000000 -v 2
```

This only checks the old Lab 3 environment. Replace it with the new
`zynqmp_gemv` executable and new setup/build files for project evidence. Use the
same socket path and a `1000000` ns synchronization quantum unless the course
platform requires a different value.

QEMU must start first and will wait for the SystemC remote-port connection.

## 10. Verification boundaries

Do not begin with full Qwen inference. Save logs for each boundary:

1. **Standalone model:** target-register reads/writes plus initiator transactions
   against a simple SystemC memory; tiny GEMV equals `soc_q8r_gemv_ref`.
2. **QEMU register path:** guest software reads idle and writes/reads all
   descriptor registers without starting a job.
3. **Tiny QEMU DMA job:** 1-2 rows and 1-2 blocks, including known addresses and
   transaction trace; exact result comparison.
4. **Smoke sequence:** repeated job, changed weights, invalid scale with output
   unchanged, then recovery, matching `board_smoke.c`.
5. **llama.cpp hook:** one real routed GEMV with `SOC_GEMV_VERIFY=1` and strict
   failure behavior.
6. **Short model run:** TinyStories first, `-n 1`, deterministic prompt/settings;
   compare CPU and accelerated outputs. Increase only after it passes.

Record at least: source commit, tool versions, exact commands, model/hash,
matrix shapes, accelerator call count, status/errors, max absolute/relative
error and simulated time. A QEMU pass is not an FPGA/RTL/physical-bandwidth pass.

## 11. Collaboration and submission

- Keep changes under `soc/sw/cosim/` plus only the minimal guarded llama/HAL
  integration required for a QEMU backend.
- Do not edit generated HLS work directories, `soc/lab3_ref/` or unrelated
  upstream llama.cpp files.
- Preserve CPU fallback and keep the FPGA/emulation backends working.
- Commit small boundaries with meaningful messages and push the teammate branch.
- Add results under `soc/docs/results/`, then send Sumedh the branch name and
  commit hash for review/merge.
- Do not merge to `soc-gemv-accel` or `main` without team review.
