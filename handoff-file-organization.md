# SoC accelerator repository handoff and file organization

This file is the starting point for an agent or teammate creating another GEMV accelerator design. Read it before scanning llama.cpp. The project-specific implementation is small; most of the upstream repository does not need to be read or changed.

## Current lineage

```text
gemv-golden
    |
    +-- gemv-q4       Q4_K weights x Q8_K activations
            |
            +-- gemv-q4-0   Q4_0 weights x Q8_0 activations
```

`gemv-q4-0` starts at Q4_K commit `28308437c`. Each accelerator branch should own one weight/activation format and one hardware artifact name. Do not make one branch silently accelerate several unrelated quantization layouts.

## Read these files first

An agent normally needs only this set for initial design work:

| File | Why it matters |
|---|---|
| `soc/README.md` | Active branch format, limits, layout, validation state |
| `soc/sw/repack/soc_repack.h` | Single source of truth for block sizes, maximum dimensions, and repacked matrix structure |
| `soc/sw/repack/soc_repack.c` | Native-to-hardware packing and software reference arithmetic |
| `soc/hw/gemv/gemv.h` | HLS types, maximum dimensions, and kernel status codes |
| `soc/hw/gemv/gemv.cpp` | Synthesizable kernel and AXI/control ports |
| `soc/hw/gemv/tb.cpp` | Kernel/repack correctness and error-status test |
| `soc/sw/hal2/soc_gemv_hal.h` | Software view of the HLS register ABI |
| `soc/sw/hal2/soc_gemv_hal.c` | Register programming, validation, timeout, and completion protocol |
| `soc/sw/gemv/soc_gemv.c` | Persistent weight cache, dispatch, fallback, verification, and statistics |
| `ggml/src/ggml-cpu/ggml-cpu.c` | Narrow llama.cpp interception point and eligibility rules |
| `soc/hw/gemv/build_hls.tcl` | Reproducible C simulation, synthesis, co-simulation, and IP export |
| `soc/hw/vivado/gemv/build_vivado.tcl` | Ultra96 block design, address assignment, timing checks, and artifact export |

Read the matching quant block definition and generic vector-dot implementation in upstream ggml before changing arithmetic. For example, Q4_0 uses `block_q4_0`, `block_q8_0`, and `ggml_vec_dot_q4_0_q8_0_generic`. The generic implementation defines the required nibble ordering, scale type, and floating-point accumulation order.

## Directory map

```text
soc/
|-- README.md                       active design summary
|-- submission_defaults.cmake      default board build options
|-- hw/
|   |-- gemv/
|   |   |-- gemv.cpp               HLS kernel
|   |   |-- gemv.h                 HLS interface and status ABI
|   |   |-- tb.cpp                 standalone and Vitis testbench
|   |   |-- build_hls.tcl          HLS stage driver
|   |   |-- check_export.tcl       generated register ABI check
|   |   `-- export_existing.tcl    package an already synthesized solution
|   |-- vivado/gemv/
|   |   `-- build_vivado.tcl       Ultra96 integration and bitstream
|   `-- out/                       generated deployment artifacts; names must identify the design
|-- sw/
|   |-- repack/                    native quant layout -> accelerator layout and reference math
|   |-- gemv/                      ggml-facing dispatcher, cache, emulation backend, CMake glue
|   `-- hal2/                      register HAL, DMA arena, UIO adapter, XRT adapter
|-- tests/                         small component tests that do not need llama.cpp
`-- third_party/xrt/include/xrt/    board-matched XRT C headers used for cross-builds

ggml/src/ggml-cpu/ggml-cpu.c       only upstream execution file normally changed
```

The inherited `soc/hw/common/dot_q8_0_core.*` and old hardware files are not automatically valid for a new design. Use them only when the new arithmetic actually matches them.

## Quantization-specific files

These files are expected to change for almost every new accelerator format:

1. `soc/sw/repack/soc_repack.h`
   - native block byte sizes
   - activation block byte size
   - maximum block count and rows
   - repacked stream structure

2. `soc/sw/repack/soc_repack.c`
   - pack/unpack functions
   - exact scalar reference
   - native fallback reference

3. `soc/hw/gemv/gemv.cpp` and `gemv.h`
   - HLS ports and element types
   - scale decoding
   - quant unpacking
   - integer dot product
   - floating-point accumulation order
   - invalid-scale and invalid-shape status

4. `soc/hw/gemv/tb.cpp`
   - deterministic native blocks
   - independent reference computation
   - pack/unpack round trip
   - hardware/reference comparison
   - negative status cases

5. `ggml/src/ggml-cpu/ggml-cpu.c`
   - exact `GGML_TYPE_*` eligibility
   - required block multiple
   - matching `type_traits_cpu[type].vec_dot`
   - activation format comment

6. `soc/sw/gemv/soc_gemv.*` and `soc_gemv_backend*`
   - public format-specific entry point
   - cache fingerprint block size
   - buffer sizes and device addresses
   - statistics byte accounting

7. HLS/Vivado Tcl
   - unique configuration tag
   - AXI port names and types
   - generated register macro names
   - report loop marker
   - unique `.bit`, `.hwh`, and `.xsa` stem

## Files that should usually remain stable

The following logic is format-independent unless the memory architecture or control protocol changes:

- cache hashing, locking, stale-weight detection, and CPU fallback policy in `soc_gemv.c`
- timeout, poison-on-unknown-completion, and polling rules in `soc_gemv_hal.c`
- UIO discovery and mapping rules
- XRT buffer allocation, ownership marker, quarantine behavior, and phase profiler
- Ultra96 PS configuration, `0xA0000000` control base, 64 KiB range, reset topology, and timing checks
- CMake source wiring in `soc/sw/gemv/ggml-soc-gemv.cmake`

Change those only when the new design truly changes the protocol. A quantization change normally changes byte counts, pointer names, and buffer members, not ownership or timeout behavior.

## Register ABI convention

The project keeps the same offsets across designs:

| Offset | Meaning |
|---:|---|
| `0x00` | HLS control |
| `0x04` | global interrupt enable, kept disabled by polling software |
| `0x10` | first weight stream pointer |
| `0x20` | second weight/metadata stream pointer |
| `0x30` | native activation pointer |
| `0x40` | output pointer |
| `0x50` | row count |
| `0x58` | quant block count per row |
| `0x60` | kernel status |
| `0x64` | kernel status-valid flag |

The name at `0x20` can be `w_scales`, `w_metadata`, or another format-specific stream. `check_export.tcl`, `soc_gemv_hal.h`, the kernel signature, and both FPGA backends must agree exactly.

## Efficient branch workflow

Create one branch and one worktree per accelerator. Branch from the latest verified design that is structurally closest to the new one. Do not modify the passing worktree in place.

For kernel/HAL work, use a sparse worktree containing only:

```text
AGENTS.md
CONTRIBUTING.md
CMakeLists.txt
soc/
ggml/src/ggml-cpu/ggml-cpu.c
```

This keeps iteration fast and discourages unrelated llama.cpp edits. Before a full llama build, either clone the branch normally or run `git sparse-checkout disable` in the sparse worktree to materialize the upstream sources.

Never reuse a generated HLS directory or hardware artifact from another quant format. Configuration tags must include the format, for example:

```text
q4k_f100_b64_o8
q4_0_f100_b64_o8
```

## Required validation order

Use this order so inexpensive failures are found first:

1. Native layout review
   - confirm block structs and sizes from ggml
   - confirm the exact generic vector-dot arithmetic

2. Repacker test
   - native -> repacked -> native is byte-identical
   - reject zero sizes, undersized strides, overflow, and bad alignment

3. Host kernel test
   - compile `gemv.cpp` with `SOC_GEMV_HW_HOST`
   - compare hardware output with an independent native-layout reference
   - test invalid shape and non-finite scale status

4. HAL mock test
   - verify all pointer offsets, rows, block count, start, completion, status-valid, and ownership hooks
   - verify format-specific pointer alignment

5. Driver compile checks
   - warning-as-error syntax check for the HAL, arena, generic UIO adapter, and XRT adapter
   - XRT test compile uses `SOC_GEMV_XRT_TEST` and repository-local XRT headers

6. Dispatcher/emulation test
   - two calls to the same matrix
   - one cache entry
   - no fallback for a supported shape
   - verification reports bit-exact output

7. Vitis HLS
   - C simulation
   - synthesis and report inspection
   - RTL co-simulation
   - IP export and generated ABI check

8. Vivado
   - exported-IP preflight
   - block design
   - bitstream
   - setup and hold timing checks

9. Full llama.cpp build
   - expand the sparse checkout or make a normal clone
   - build emulation first
   - verify the ggml hook with a real model of the target quant type
   - cross-compile the FPGA/XRT configuration only after emulation passes

10. Ultra96
    - program only the matching artifact
    - verify address, clock, UIO, XRT, and CMA
    - run with strict verification before measuring performance

## Llama integration boundary

The integration intentionally avoids a new ggml operator or GGUF type. During a single-column decode matrix multiplication, ggml already quantizes the activation to the vector-dot type required by the weight tensor. The hook runs after that quantization barrier and intercepts only:

- the exact target weight type
- one activation column
- two-dimensional weight tensors
- normal model-weight buffers
- shapes supported by the accelerator

Thread zero calls the SoC dispatcher. Unsupported types and shapes continue through llama.cpp. This is why a mixed-format model remains correct even when only one tensor type is accelerated.

When adapting the hook, verify `type_traits_cpu[target_type].vec_dot_type` and `vec_dot`. For Q4_0, those are Q8_0 activation blocks and the Q4_0/Q8_0 vector-dot function.

## Profiling contract

Preserve these controls across designs:

- `SOC_GEMV_PROFILE=1`: XRT phase timing
- `SOC_GEMV_STATS=1`: aggregate calls, rows, bytes, cache, repack, fallback, and verification
- `SOC_GEMV_VERIFY=1`: recompute successful FPGA/emulation output with llama.cpp reference arithmetic
- `SOC_GEMV_STRICT=1`: abort instead of hiding a backend error with CPU fallback

Keep profiler phase names aligned with the actual stream, such as `weight_scales_sync` or `weight_metadata_sync`.

## Completion evidence for every design

A design handoff is incomplete without:

- source commit hash
- quant block and activation layouts
- supported row/column bounds
- host kernel result
- HAL test result
- driver compile result
- Vitis C/RTL simulation result
- HLS timing, latency/II, and utilization reports
- Vivado setup/hold timing report
- matching bitstream/HWH/XSA hashes
- real-model strict verification log
- CPU and FPGA measurements with the same model, prompt, token count, threads, and affinity

Never label an inherited bitstream as validated for a new kernel. Never benchmark after a correctness or timing failure.
