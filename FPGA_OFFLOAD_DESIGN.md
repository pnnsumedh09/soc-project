# FPGA GEMV Offload for Qwen2.5-0.5B on Ultra96 (Option A)

Status: design draft. Numbers marked (est.) are back-of-envelope and must be
measured on the board. Model dimensions come from the published Qwen2.5-0.5B
config, not yet checked against the GGUF (run `gguf-dump` to confirm).

## 1. Goal

Offload the weight GEMVs of decode (batch-1) to the PL of the Ultra96
(Zynq UltraScale+ ZU3EG). The CPU keeps everything else and calls the FPGA
once per GEMV (Option A: GEMV-only offload).

## 2. Model facts (Qwen2.5-0.5B)

- 24 layers, d_model 896, 14 Q heads, 2 KV heads (GQA), head_dim 64
- FFN intermediate 4864 (SwiGLU), vocab 151,936, separate output.weight (NOT tied in our GGUF; see soc/docs/results/model_dims.md)
- Q8_0 file about 676 MB; about 0.52 GB of weights read per decoded token

## 3. GEMVs per decoded token

y = W x, W is (out x in).

| Op | out | in | Notes |
|---|---|---|---|
| Q proj | 896 | 896 | + bias |
| K proj | 128 | 896 | + bias |
| V proj | 128 | 896 | + bias |
| O proj | 896 | 896 | |
| FFN gate | 4864 | 896 | |
| FFN up | 4864 | 896 | same x as gate |
| FFN down | 896 | 4864 | x = SiLU(g) * u |
| LM head (once/token) | 151,936 | 896 | separate output.weight tensor |

Fusion options (concatenate weights at load time):
- QKV: one 1152 x 896 call
- gate+up: one 9728 x 896 call

About 7 x 24 + 1 = 169 calls per token (fewer with fusion: 3 x 24 + 1 = 73).
All `in` sizes are multiples of 32 (896 = 28 x 32, 4864 = 152 x 32).

## 4. Data flow and what stays on the CPU

CPU: tokenizer, embedding lookup, RMSNorm, RoPE, KV-cache append, attention
(QK^T, softmax, PV), SiLU*u, residual adds, final norm, sampling.

FPGA: only y = W x (plus optional bias add).

Per layer: RMSNorm(x) -> [FPGA QKV] -> RoPE + KV cache -> attention ->
[FPGA O] -> residual -> RMSNorm -> [FPGA gate+up] -> SiLU*u -> [FPGA down]
-> residual. After layer 24: final RMSNorm -> [FPGA LM head] -> sample.

## 5. What moves per call

- Weights: NOT sent. Per-layer weights differ but are constant across
  tokens. All layers are loaded once into DDR; each call just passes a
  different `weight_addr`. PS and PL share DDR.
- Sent per call: activation vector quantized to Q8_0 (about 1 KB for 896,
  about 5 KB for 4864) plus a few register writes.
- Returned: y vector written by the FPGA into a shared DDR buffer.

BRAM on the ZU3EG is under 1 MB, so weights cannot be resident. All
weights stream from DDR every token. DDR bandwidth is the limit.

## 6. Weight streaming

Use an AXI master inside the GEMV IP (HLS `m_axi` or RTL), reading through
HP ports (128-bit). INCR bursts up to 256 beats, several outstanding.
One 128-bit port at 250 MHz is about 4 GB/s (est.). A second port may be
needed to reach the DDR ceiling. Alternatives (AXI DMA MM2S, Data Mover)
add stream plumbing without a clear benefit for this access pattern.

### Q8_0 alignment: repack at load time

Q8_0 block = 34 B (2 B fp16 scale + 32 B int8), which does not align to
16 B beats. Repack each matrix into:

```
quants: rows x (in/32) x 32 B   contiguous int8 (2 beats per block)
scales: rows x (in/32) x 2 B    separate fp16 array
```

Row of 896: 56 beats of quants (896 B) + 56 B of scales.
Repacked buffers must be physically contiguous (CMA / udmabuf / reserved
memory). llama.cpp mmaps the GGUF, so copy once at load and keep only the
repacked copy for offloaded tensors (2 GB RAM total).

## 7. GEMV engine architecture

Core unit: a streaming vector dot-product lane.
- Per cycle: one 128-bit beat = 16 int8 weights x 16 int8 x values.
- 16 int8 multipliers (DSP48) -> adder tree -> int32 block sum.
- Per 32-block: multiply by (scale_w * scale_x), accumulate in fp32
  (or fixed point). After in/32 blocks the row finishes and y[i] is emitted.
- Rows are contiguous, so the lane moves straight to the next row.

x vector: read once per call into BRAM, broadcast to all lanes.

Scaling: one 16-wide lane at 250 MHz consumes about 4 GB/s, about one HP
port (est.). Decode has no weight reuse (each weight used once per token),
so a large tile x vector array is not needed. Add lanes (N rows in
parallel, shared x) only to:
1. use more than one weight port,
2. hide accumulator loop latency (interleave rows if II > 1),
3. compensate for a lower PL clock.

Tile x tile (GEMM) only pays off for prefill; out of scope unless profiling
shows prompts dominate.

## 8. Control interface (AXI-Lite)

Registers: `weight_addr`, `scale_addr`, `bias_addr`, `rows`, `in_dim`,
`x_addr`, `y_addr`, start, done/status (poll or interrupt).
The host precomputes a table {weight_addr, scale_addr, rows, in_dim} per
(layer, op) at load time.

Coherency: use the HPC port, or flush the x buffer before start and
invalidate the y buffer after done.

## 9. Expected performance (est., verify)

- Ceiling from weight streaming: 0.52 GB / (4 to 5 GB/s) = about 0.10 to
  0.13 s/token, i.e. about 8 to 10 tok/s for the GEMV part.
- Per-call overhead (register writes, polling through UIO or /dev/mem):
  likely microseconds to tens of microseconds x 73 to 169 calls. Measure.
- Attention and elementwise ops stay on the A53 and add to the total.

## 10. Plan

1. Baseline: run `llama-bench` / benchmark.py on CPU; profile per-op time.
2. Measure achievable PL read bandwidth with a trivial AXI read-only master
   (sets the tok/s ceiling).
3. Write the weight repack tool (GGUF Q8_0 -> quants + scales layout).
4. Build the Q8_0 GEMV engine (N = 1, 16-wide); verify one matrix against
   ggml's `ggml_vec_dot_q8_0_q8_0` result.
5. Hook into llama.cpp (candidate: `ggml_compute_forward_mul_mat` or a new
   ggml backend) with the descriptor table.
6. Try fusion (QKV, gate+up) and N > 1 lanes / second port.
7. Later (Option C): move SwiGLU and residual next to the FPGA to remove
   round trips; consider attention.

## 11. Open questions

- Actual PL clock and sustained AXI bandwidth on the board.
- fp32 vs fixed-point accumulation (accuracy vs resources).
- Whether to fuse bias into the engine.
- Hook point in ggml (op-level override vs backend).
