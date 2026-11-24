# FPGA Design Versions

This document identifies the branches, commits, and immutable tags containing
the FPGA accelerator designs. Use a tag when reproducing or submitting a
validated design.

| Design | Development branch | Validated tag | Status |
|---|---|---|---|
| Q8_0 GEMM/GEMV | `gemm-compiletime-quant` | `ultra96-q8-gemm-n8-validated-v1` | Ultra96-V2 validated |
| Q4_0 GEMM/GEMV | `gemm-compiletime-quant` | Not tagged | HLS validated; board/software integration pending |
| Q4_K | TBD | TBD | Planned |
| Q4_K_M model support | TBD | TBD | Planned |

## Q8_0 GEMM/GEMV

Validated tag:

```text
ultra96-q8-gemm-n8-validated-v1
