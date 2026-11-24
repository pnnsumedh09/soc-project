# FPGA Design Versions

Use immutable tags for grading and reproduction. Development branches may
continue changing after a design has been validated.

## Validated designs

| Design | Development branch | Validated tag | Status |
|---|---|---|---|
| Golden Q8_0 GEMV | `gemv-golden` | `ultra96-q8-gemv-golden-v1` | Validated |
| Q8_0 GEMM/GEMV, N≤8 | `gemm-compiletime-quant` | `ultra96-q8-gemm-n8-validated-v1` | Ultra96-V2 validated |
| Q4_0 GEMM/GEMV | `gemm-compiletime-quant` | Not tagged | HLS validated; software/board integration pending |

## Development branch index

| Branch | Purpose | Validation status |
|---|---|---|
| `soc-gemv-accel` | Default integration branch and design documentation | Active |
| `gemv-golden` | Stable golden GEMV design | Validated |
| `gemm-compiletime-quant` | Compile-time Q8_0/Q4_0 GEMM development | Q8_0 validated |
| `gemv-q4-0` | Q4_0 GEMV development | Needs status documentation |
| `gemv-q4` | Earlier Q4 development | Needs status documentation |
| `gemv-activation-quant` | Activation-quantization development | Experimental |
| `combination-a-feasible` | Earlier design exploration | Historical |
| `qemu-gemv-ng` | QEMU/GEMV development | Historical |

## Golden Q8_0 GEMV

- Branch: `gemv-golden`
- Tag: `ultra96-q8-gemv-golden-v1`
- Purpose: stable single-column FPGA GEMV implementation.

To reproduce:

```bash
git fetch origin --tags
git checkout ultra96-q8-gemv-golden-v1

```

## Q8_0 GEMM/GEMV

- Branch: `gemm-compiletime-quant`
- Tag: `ultra96-q8-gemm-n8-validated-v1`
- Commit: `e84dc04a305a3c9e3dbe9123520ab1638914a45b`
- Supports `N=1` GEMV and `N<=8` GEMM.
- Uses eight-column weight reuse, DATAFLOW prefetch, four HP ports, and a
  persistent FPGA weight cache.

To reproduce:

```bash
git fetch origin --tags
git checkout ultra96-q8-gemm-n8-validated-v1
```
