#!/usr/bin/env python3
"""Dump Q8_0 weight tensors from a GGUF file into small binary files for tests.

Reads the GGUF read-only (memory-mapped); the .gguf file is never modified.

Output format (consumed by soc/sw/repack/test_repack and later HLS testbenches):
    char     magic[8]  = b"SOCQ8T1\\0"
    uint32   ne0       (input dimension, elements per row)
    uint32   rows      (number of rows written, <= ne1)
    uint64   row_stride (bytes per row, = ne0/32 * 34 for an unpadded tensor)
    char     name[64]  (tensor name, NUL padded)
    bytes    rows * row_stride of native Q8_0 blocks (2 B FP16 scale + 32 int8)

Usage (from the llama.cpp directory):
    PYTHONPATH=gguf-py python3 soc/tools/dump_q8_tensor.py MODEL.gguf OUTDIR \
        [--tensor NAME ...] [--rows N]
Default tensors: one of each matmul shape in layer 0 plus 512 rows of output.weight.
"""
import argparse
import os
import struct
import sys

from gguf import GGUFReader, GGMLQuantizationType

DEFAULT_TENSORS = [
    "blk.0.attn_q.weight",
    "blk.0.attn_k.weight",
    "blk.0.attn_v.weight",
    "blk.0.attn_output.weight",
    "blk.0.ffn_gate.weight",
    "blk.0.ffn_up.weight",
    "blk.0.ffn_down.weight",
    "output.weight",
]

Q8_0_BLOCK = 34
QK = 32


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("model")
    ap.add_argument("outdir")
    ap.add_argument("--tensor", action="append", help="tensor name (repeatable)")
    ap.add_argument("--rows", type=int, default=512, help="max rows per tensor (default 512; 0 = all)")
    args = ap.parse_args()

    reader = GGUFReader(args.model, "r")
    by_name = {t.name: t for t in reader.tensors}
    os.makedirs(args.outdir, exist_ok=True)

    for name in args.tensor or DEFAULT_TENSORS:
        t = by_name.get(name)
        if t is None:
            print(f"skip {name}: not found", file=sys.stderr)
            continue
        if t.tensor_type != GGMLQuantizationType.Q8_0:
            print(f"skip {name}: type {t.tensor_type.name}, not Q8_0", file=sys.stderr)
            continue
        ne0, ne1 = int(t.shape[0]), int(t.shape[1])
        stride = ne0 // QK * Q8_0_BLOCK
        raw = memoryview(t.data).cast("B")
        if len(raw) != ne1 * stride:
            print(f"skip {name}: size {len(raw)} != {ne1}*{stride}", file=sys.stderr)
            continue
        rows = ne1 if args.rows <= 0 else min(args.rows, ne1)
        path = os.path.join(args.outdir, name.replace(".", "_") + ".bin")
        with open(path, "wb") as f:
            f.write(b"SOCQ8T1\0")
            f.write(struct.pack("<IIQ", ne0, rows, stride))
            f.write(name.encode()[:63].ljust(64, b"\0"))
            f.write(raw[: rows * stride])
        print(f"{name:28s} in={ne0:<5d} rows={rows:<6d}/{ne1:<6d} -> {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
