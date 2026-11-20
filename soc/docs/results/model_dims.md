# Model dimensions: qwen2.5-0.5b-instruct-q8_0.gguf (task P0.4)

Measured 2026-11-18 with `gguf-py` (`GGUFReader`) on the file `qwen2.5-0.5b-instruct-q8_0.gguf` (675,710,816 bytes). Reproduce:

```bash
cd llama.cpp/gguf-py
PYTHONPATH=. python3 gguf/scripts/gguf_dump.py --no-tensors ../../qwen2.5-0.5b-instruct-q8_0.gguf
```

## Metadata

arch `qwen2`, 24 blocks, context 32768, embedding 896, FFN 4864, 14 heads, 2 KV heads (head_dim 64), rope base 1e6, rms eps 1e-6, `file_type = 7` (Q8_0), 291 tensors.

## Tensors

| Tensor | Type | Shape (in x out) | Bytes |
|---|---|---|---|
| `token_embd.weight` | Q8_0 | 896 x 151,936 | 144,643,072 (row lookup only, not a matmul) |
| `output.weight` | Q8_0 | 896 x 151,936 | 144,643,072 (LM head, **separate tensor, embeddings are NOT tied**) |
| `blk.N.attn_q.weight` | Q8_0 | 896 x 896 | 852,992 |
| `blk.N.attn_k.weight` | Q8_0 | 896 x 128 | 121,856 |
| `blk.N.attn_v.weight` | Q8_0 | 896 x 128 | 121,856 |
| `blk.N.attn_output.weight` | Q8_0 | 896 x 896 | 852,992 |
| `blk.N.ffn_gate.weight` | Q8_0 | 896 x 4864 | 4,630,528 |
| `blk.N.ffn_up.weight` | Q8_0 | 896 x 4864 | 4,630,528 |
| `blk.N.ffn_down.weight` | Q8_0 | 4864 x 896 | 4,630,528 |
| `blk.N.attn_{q,k,v}.bias` | F32 | 896 / 128 / 128 | 3,584 / 512 / 512 |
| `blk.N.attn_norm`, `ffn_norm`, `output_norm` | F32 | 896 | 3,584 each |

All 24 layers have identical shapes and types. Every matmul weight is Q8_0 (170 Q8_0 tensors = 669,476,864 B; 121 F32 tensors = 286,208 B).

## Derived numbers

- Matmul weight bytes read per decoded token (all Q8_0 except `token_embd`): per layer 15,841,280 x 24 = 380,190,720, plus `output.weight` 144,643,072 = **524,833,792 B (0.525 GB)**.
- Rows per token: 12,672 per layer x 24 + 151,936 = **456,064**.
- Row sizes in native Q8_0 blocks: 896-input row = 952 B (28 blocks x 34 B); 4864-input row = 5,168 B (152 blocks x 34 B). After repack the total bytes are the same (quants + scales split into two arrays) plus 4 KB alignment padding.
- Largest single matrix to place contiguously: `output.weight`, 144.6 MB. All other matmul tensors are at most 4.6 MB.
- Q/K/V projections have F32 biases (the engine or the CPU must add them).

## Consequences for the plan

- Embeddings are not tied: `token_embd` is used only for one-row lookup on the CPU and does not need to be repacked. Repack footprint is about 525 MB (the matmul tensors), not 525 MB plus anything extra for the LM head.
- Memory: the mmapped GGUF (676 MB, reclaimable page cache) plus the repacked copy (about 525 MB) fit in 2 GB, but the repacked copy needs contiguous allocations (see P0.6).
- All offloadable GEMVs are Q8_0 x Q8_0-activation, so one engine type covers every projection and the LM head.
