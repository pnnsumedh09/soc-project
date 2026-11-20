# P4.1/P4.2 integration source validation

Date: 2026-11-20. Codex with Sumedh. Source written and locally checked on arm64 macOS with Apple clang; no actual Vitis/Vivado, Linux target execution or board MMIO/DMA result is claimed. User reports the course image's allocator/access support is not yet known.

## Implemented

- `soc/hw/vivado/gemv/build_vivado.tcl`: check/BD/bitstream stages for the selected sequential GEMV, explicit AXI-Lite base 0xA0000000/64KiB, four HLS memory masters via SmartConnect to PS HP0, clocks/resets, part/clock/register checks, design/address-map export, timing checks and BIT/HWH/XSA export. Requires verified PS config or matching board definition for BD/build; no DDR/MIO preset is invented.
- `soc/hw/gemv/export_existing.tcl` and `check_export.tcl`: package existing sequential RTL without resetting or rerunning C simulation/synthesis/co-simulation; validate generated register header against the software ABI. Existing synthesis/RTL/project must be present.
- `soc/sw/hal2/`: portable control protocol and DMA arena plus optional Linux/AArch64 UIO/u-dma-buf adapter; reusable x/y scratch, persistent weight device addresses, failure-safe output staging and CPU fallback integration. Emulation remains the build/runtime default. The concrete adapter requires confirmed course approval/support, identity PL-visible pool addresses, noncached write-combined mapping and dedicated ownership; it is not assumed available.
- Existing hook/backend dispatch gains optional `fpga` selection and CMake option `GGML_SOC_GEMV_FPGA`; hook location, weight format and HLS arithmetic are unchanged.
- `board_smoke.c`: target-only repeated-job/changed-weight/error-output/recovery test. Compiled locally but NOT run against hardware.
- `soc/tools/board_preflight.py`: read-only discovery JSON for kernel, page size, UIO mappings, u-dma-buf attributes and CMA/memory. No MMIO access, allocation, driver installation or device-tree modification.

## Checks performed

| Check | Result |
|---|---|
| `make -C soc/sw/hal2 test` | PASS: mocked MMIO control/status/order, repeated calls, stale done/valid, high/low addresses, delayed idle, malformed/overlapping/overflow addresses, auto-restart/busy, timeout poisoning, missing valid, allocator identity/bounds/coalescing and absent-FPGA CPU fallback |
| HAL/arena UBSan | PASS, no diagnostics, separately compiled instrumented binary |
| Existing software-hook suite, emulation-only build | PASS: 89 emulated GEMVs / 48,256 rows, exact golden comparison, one fallback, cache/stale/multithread checks |
| Same hook suite with FPGA source/selection compiled in, runtime emulation | PASS with the same 89-call / 48,256-row counts; proves optional addition does not break tested emulation behavior, not FPGA execution |
| FPGA backend and board-smoke compilation | PASS with C11, warnings-as-errors and contraction disabled; native initialization refuses non-Linux/AArch64 hardware access |
| `python3 soc/hw/vivado/gemv/test_scripts.py` | PASS using temporary fixtures and mocked vendor commands: preflight creates no project; BD topology/address; absent PS settings, invalid base, r8, wrong part/clock/ABI rejected; package existing project without resets/sim/synth calls |
| Board discovery script on host | PASS: reports Darwin/arm64 and no UIO/DMA devices without trying MMIO |
| Full target CMake/toolchain build | NOT RUN: CMake/target toolchain unavailable locally |
| Vitis packaging / Vivado BD, implementation and timing | NOT RUN: vendor tools unavailable locally |
| Linux adapter, board single GEMV, full inference/performance | NOT RUN: hardware/access/allocator configuration unconfirmed |

Local evidence: `/tmp/soc_p4_hal.log`, `/tmp/soc_p4_hal_ubsan.log`, `/tmp/soc_p4_hook_emul.log`, `/tmp/soc_p4_hook_fpga_build.log`. The emulation suite's reported GB/s is CPU emulation timing and is not PL bandwidth.

## Timeout and memory lifetime

No blind retry or reuse after unknown hardware completion. HAL poisons subsequent jobs after timeout/protocol error. Concrete adapter stops allocations/frees and leaves a persistent in-flight marker; driver-owned persistent DMA memory is not released by closing the userspace mapping. External reset/quiescence is required before clearing an armed marker or reusing/unloading the pool. Advisory locks/marker rely on all owners following the same protocol; this is not kernel-enforced isolation or automated hardware reset. Successful kernel error completion can fall back safely without copying invalid y.

The existing weight-cache lock serializes the adapter; standalone HAL/arena users must also serialize. Cache policy is deliberately restricted to u-dma-buf sync modes 2/6 with O_SYNC on AArch64 and noncoherent HP0. Cached memory, HPC coherency, arbitrary physical heaps, SMMU translation and general DMA-BUF importing are not implemented.

## AXI width correction found during integration

Earlier notes conflated element/access widths with AXI data-bus widths. The supplied current top RPT explicitly lists `m_axi_weights_RDATA` as 128 bits, `m_axi_scales_RDATA` and `m_axi_activation_RDATA` as 32 bits, and output WDATA as 32 bits. Scales/activations still use 16-bit/8-bit elements and narrow accesses; that is not a 16-bit/8-bit physical AXI bus. The current SmartConnect supports the reported 32/128-bit widths. Actual selected export interfaces remain subject to Vivado validation. This corrects the physical-port interpretation in older notes; it does not change scheduling results or imply full-width payload utilization.

## Next required evidence

1. Package the existing sequential synthesis if no matching IP export exists, then run Vivado `SOC_GEMV_BUILD_STAGE=check`; no kernel simulation rerun needed.
2. Confirm a matching course PS configuration/board definition, run `bd`, inspect address map, then implement and check routed timing.
3. Collect read-only board discovery and confirm course-approved device access, pool capacity/addressability and cache policy; adapt the Linux layer if this image uses another allocator.
4. Load matching hardware, run board smoke, then target-hook verification/full-model evaluation. Gates M0/D0/B0/A0 remain unpassed.

Sources for the optional strategy: [Linux UIO](https://www.kernel.org/doc/html/latest/driver-api/uio-howto.html), [u-dma-buf](https://github.com/ikwzm/udmabuf), [Vitis HLS 2022.2 export_design](https://docs.amd.com/r/2022.2-English/ug1399-vitis-hls/export_design), [SmartConnect widths](https://docs.amd.com/r/en-US/pg247-smartconnect/Width-Conversion). These are interface references, not board acceptance evidence.

## First LRC packaging and header-selector fix: 2026-11-20

Sumedh supplied actual Vitis HLS/Vivado 2022.2 export output on yoshi, 11:35:58–11:36:40 CST. Existing sequential project f150_b64_o8 opened without reset; export_design completed and generated solution/impl/export.zip at 11:36:30. Packaging succeeded. The script then failed in our post-export register-header checker, before claiming ABI agreement.

User's find output confirms both generated files under impl/ip/drivers/soc_gemv_hw_v1_0/src: xsoc_gemv_hw.h (API) and xsoc_gemv_hw_hw.h (register definitions), with another pair under impl/misc. The wildcard `*_hw.h` matched both files inside ip and incorrectly rejected the count of two. Fixed it to select exact basename `xsoc_gemv_hw_hw.h`; do not include misc copies or silently bypass register checks.

Local regression now reproduces both real filenames, passes preflight/BD/packaging dispatch with mocked commands, and still rejects a missing register header, incorrect offsets, part and clock. No HLS/kernel/HAL change. Preserve the successful existing export; after pulling this correction, run Vivado stage check directly to perform the ABI/preflight check. No repeat of packaging, C simulation, synthesis or co-simulation is required. Actual ABI confirmation and Vivado integration remain pending the new preflight output.
