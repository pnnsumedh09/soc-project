# P4.2: GEMV register HAL and optional FPGA backend

Source implemented and locally tested; target Linux/UIO/DMA setup and board operation are NOT yet verified. No HLS arithmetic change. Sequential GEMV remains the selected design. The course board's supported allocator/access method is still unknown, so the Linux adapter is optional, not an established submission dependency.

## What is implemented

- `soc_gemv_hal.c/.h`: portable, serialized control protocol with exact reviewed register offsets, 64-bit addresses, shape/alignment/range/overlap checks, stale done/valid clearing, ownership barriers, start/poll/status and bounded timeout. Waits for both DONE and IDLE, and accepts output only with status-valid and status zero.
- `soc_gemv_arena.c/.h`: 4-KiB-aligned first-fit suballocation and coalescing inside an externally allocated DMA pool. Checks that weight CPU/device addresses identify the same live allocation. Allocator metadata is ordinary CPU memory; engine buffers are never heap allocations.
- `soc_gemv_backend_fpga.c`: Linux/AArch64 UIO map0 registers and u-dma-buf pool. Reserves reusable activation/output scratch, stages x, uses persistent repacked weights directly, and copies y to the caller only on success. Existing weight-cache lock serializes all allocations/jobs. Nonzero return follows the existing CPU fallback; strict mode aborts instead.
- `board_smoke.c`: explicit target-only single-GEMV bring-up test: repeated calls, changed weights, nonfinite-scale failure without copying invalid results, then recovery. It is not run by `make test`.
- Optional llama.cpp CMake selection with `-DGGML_SOC_GEMV=ON -DGGML_SOC_GEMV_FPGA=ON`. Runtime default remains `emul`; explicitly select `SOC_GEMV_BACKEND=fpga`.

The HAL's `to_device`/`to_cpu` hooks allow another verified allocator/coherency strategy later. The concrete adapter currently uses noncached write-combined mappings plus AArch64 barriers; it does not flush arbitrary cached heap pointers.

## Local verification

```sh
make -C soc/sw/hal2 test
python3 soc/hw/vivado/gemv/test_scripts.py
```

HAL tests use fake MMIO and clock callbacks. They cover stale DONE/valid, split 64-bit addresses, repeated calls, delayed idle, nonzero kernel status, invalid descriptors, auto-restart/busy rejection, timeout quarantine and missing status-valid. Arena checks cover alignment, allocation failure, CPU/device identity and free/coalescing. The fallback test deliberately uses `/dev/null/no-uio`, so it cannot access real hardware. Board test compilation alone does not validate DMA.

## Board prerequisites for this optional adapter

1. Matching sequential GEMV bitstream loaded, PS HP0/DDR initialized and 150 MHz (or the selected clock) actually configured. UIO map0 exposes the GEMV control bank, default physical base `0xA0000000`, at least 64 KiB, map offset zero. This code polls and does not require an interrupt.
2. Course-approved Linux UIO and **ikwzm u-dma-buf** driver, not the unrelated upstream Linux `/dev/udmabuf` API. Device/sysfs pairs must refer to the same device. The script does not install/load drivers or modify device trees.
3. Dedicated persistent contiguous pool, 4-KiB aligned and wholly below 4 GiB, with the reported physical address directly visible to PL (no unaccounted SMMU/IOMMU translation). Pool must not overlap OS-owned DDR or other DMA users.
4. `sync_mode=2` or `6` and `dma_coherent=0`, opened `O_SYNC` for noncached write-combined CPU access on AArch64. Cached/coherent mappings are refused for this HP0 strategy. Do not change pool cache policy or access it through a conflicting cached alias while this backend owns it.
5. One cooperating process owns both UIO and pool; advisory locks cannot exclude other drivers or software that ignores them. A small pool can support smoke tests; caching all selected Qwen weights needs roughly 525 MB plus padding/scratch, which has NOT been demonstrated on the board. P0.6/D0 remains required.

Discovery is read-only:

```sh
python3 soc/tools/board_preflight.py > board_preflight.json
```

Return this JSON before assuming the optional adapter matches your image. If only PYNQ or a course-specific allocator is supported, retain the common HAL and adapt the Linux layer rather than installing an unapproved dependency.

Defaults and overrides:

| Environment | Default / meaning |
|---|---|
| `SOC_GEMV_UIO_DEVICE` | `/dev/uio0` |
| `SOC_GEMV_UIO_SYSFS` | `/sys/class/uio/uio0/maps/map0` |
| `SOC_GEMV_EXPECTED_BASE` | `0xa0000000`; must match actual Vivado/UIO mapping |
| `SOC_GEMV_DMA_DEVICE` | `/dev/udmabuf0` |
| `SOC_GEMV_DMA_SYSFS` | `/sys/class/u-dma-buf/udmabuf0` |
| `SOC_GEMV_TIMEOUT_MS` | `10000`, allowed 1..600000 |
| `SOC_GEMV_LOCK_FILE` | `/tmp/soc-gemv-<control-base-in-hex>.lock`; all owners must use the same persistent path |

The marker file is set before every submitted job and cleared only after known completion/idle. A timeout or process death leaves it armed. Further jobs/buffer reuse are refused; the persistent DMA pool remains owned by its external driver even after userspace unmaps it. After such a failure, reset/reload the accelerator and ensure DMA is quiescent **before** clearing the marker or reusing/unloading that pool. Do not implement timeout recovery by freeing memory under an active AXI master. A marker file is not a kernel-enforced reset or memory isolation mechanism.

## Target smoke test and inference

After the prerequisites are confirmed and hardware is loaded:

```sh
make -C soc/sw/hal2 board_smoke
./soc/sw/hal2/board_smoke
```

Native compilation on the board uses `cc`; for cross-build use `CC=<AArch64 Linux compiler>`. The smoke test reserves enough output scratch for the supported maximum but uses only a 9-row × 1056-input matrix. Require its PASS before full inference.

For the normal target-toolchain CMake build, add `-DGGML_SOC_GEMV=ON -DGGML_SOC_GEMV_FPGA=ON` to the established build command. Then explicitly enable FPGA runtime selection and strict verification on a small model invocation:

```sh
export SOC_GEMV_BACKEND=fpga SOC_GEMV_VERIFY=1 SOC_GEMV_STATS=1 SOC_GEMV_STRICT=1
# Then run your established llama-cli model/prompt command.
```

CMake target integration, Linux compilation/MMIO/DMA behavior, full inference, performance and course-image acceptance remain pending target validation. No per-layer descriptor table is needed for v1: the existing weight cache already stores each matrix's quant/scale device addresses.

Sources: [Linux UIO mapping documentation](https://www.kernel.org/doc/html/latest/driver-api/uio-howto.html), [u-dma-buf driver documentation and cache modes](https://github.com/ikwzm/udmabuf). These describe an available adapter strategy, not evidence that the course image provides it.
