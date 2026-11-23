# Combination A: current state and implementation plan

Date: 2026-11-22. Source reviewed: branch `soc-gemv-accel` at `f9197b979`.

This document compares the current SoC project with Combination A:

- Q8 GEMV.
- Direct AXI weight reads.
- Descriptor chaining.
- Double buffering.
- Activation reuse across QKV and gate/up projections.
- Cycle and byte counters.

It also covers the proposed QKV bias path, fused FFN path and command queue. Statements below distinguish source implementation, simulation evidence, integration evidence and real-board evidence.

The ordered LRC, Vitis, Vivado and Ultra96 execution gates are in `soc/docs/COMBINATION_A_VALIDATION_WORKFLOW.md`.

## 1. Executive summary

The current branch implements and verifies a single-job Q8_0 GEMV engine. The CPU quantizes one activation vector, the accelerator loads that vector into BRAM once for the job, reads repacked Q8_0 weights directly from DDR through AXI masters, computes one output vector and writes it to DDR. The software submits one job through AXI-Lite registers and polls for completion.

Combination A requires a controller above that engine. The controller must fetch and validate a bounded command list, retain activation and intermediate state across commands, schedule grouped projections, overlap tile loading with computation and record counters. None of that controller exists yet.

Headline status:

| Combination A item | Status | Evidence and limitation |
|---|---|---|
| Q8 GEMV | Implemented and bounded C/RTL verified | `soc/hw/gemv/gemv.cpp` implements Q8_0 GEMV. Sequential and optional eight-row variants passed Vitis C and RTL co-simulation. No current real-board GEMV result. |
| Direct AXI weight reads | Implemented in source and block design | HLS uses `m_axi` weight/scales masters. Vivado maps all four GEMV masters to PS HP0 DDR. Bursts are inferred. Sustained board bandwidth is not measured. |
| Descriptor chaining | Missing | HAL writes one descriptor into scalar AXI-Lite registers, starts one job and waits. There is no descriptor DMA master, queue, producer/consumer index or multi-job execution. |
| Double buffering | Missing | The optional r8 kernel has one quant/scale tile. Tile loading and computation are separate phases. The default sequential kernel has no weight tile. |
| Activation reuse across QKV and gate/up | Partial only inside one GEMV | One GEMV loads x once and reuses it across all rows. The backend copies x and hardware reloads it for every separate GEMV call. Q, K, V, gate and up are separate jobs today. |
| Cycle and byte counters | Missing in hardware | Software records host elapsed time and calculates weight payload bytes. HLS reports contain offline latency. There are no runtime hardware cycle/read-byte/write-byte registers or per-descriptor results. |
| QKV grouping and bias add | Missing | Current kernel has one weight/scales/output tuple and no bias pointer. Bias remains on the CPU graph path. |
| Gate/up grouping and fused FFN | Missing | No on-chip gate/up buffers, SiLU, elementwise multiply, activation quantizer, down projection chaining or residual add exists. |
| General command queue | Missing | No opcode decoder or command memory format exists. |

Of the six headline Combination A items, two are present, one is partial and three are missing. The larger QKV and FFN grouped operations are missing.

## 2. What the current SoC does

### 2.1 llama.cpp hook

The guarded hook is in `ggml/src/ggml-cpu/ggml-cpu.c` after activation conversion and the thread barrier in `ggml_compute_forward_mul_mat()`.

It accepts only:

- Q8_0 weights.
- F32 input and output.
- One activation column.
- One weight matrix with no view or extra backend state.
- A weight tensor stored in a weight-classified backend buffer.

Thread 0 calls `soc_gemv_q8_0()` and the other threads return from that operation. Multi-column prefill and unsupported cases use the normal CPU implementation.

This hook sees one matrix multiplication at a time. It does not see a complete transformer layer schedule and cannot currently combine Q/K/V or gate/up/down nodes into one hardware command chain.

### 2.2 Weight layout and cache

`soc/sw/repack/` converts each native 34-byte Q8_0 weight block into:

- 32 contiguous quant bytes.
- One separate FP16 scale.

`soc/sw/gemv/soc_gemv.c` caches the repacked matrix by source pointer, row stride and shape. The cache asks the selected backend for engine-visible memory and retains the quant and scale device addresses. This cache can be reused by Combination A.

The cache is a weight cache, not a hardware descriptor schedule. It stores no QKV grouping, layer ordering, bias address or activation lifetime information.

### 2.3 Current backend and HAL

`soc/sw/gemv/soc_gemv_backend.h` exposes a single `run()` operation:

```text
run(repacked W, quant address, scale address, one Q8_0 x, one F32 y)
```

The optional FPGA backend in `soc/sw/hal2/soc_gemv_backend_fpga.c`:

1. maps one UIO control bank;
2. maps one persistent u-dma-buf pool;
3. allocates reusable x and y scratch buffers;
4. copies one x vector into DMA memory;
5. writes one scalar job through the HAL;
6. polls until DONE and IDLE;
7. copies one y vector back only after successful status.

`soc/sw/hal2/soc_gemv_hal.c` validates four non-overlapping buffers, writes the four addresses plus rows and blocks, starts one job and waits. Timeout or an unknown completion poisons the HAL so memory is not reused under a possibly active AXI master.

These ownership, timeout and fallback rules should be retained in the chained design.

### 2.4 Current HLS engine

`soc/hw/gemv/gemv.cpp` implements one job:

```text
load one native Q8_0 x into local storage
for every output row:
    read row scales
    read row quant beats directly from DDR
    compute ordered Q8_0 dot product
    write one FP32 y value
```

The maximum activation requires approximately:

- 4,864 bytes of quant storage;
- 608 bytes of converted FP32 activation scales.

The default sequential design reads weights directly in the compute loop. The optional r8 design buffers a 32-block tile for eight rows and interleaves the eight FP32 accumulators. The r8 tile loader and compute loop do not overlap.

The selected Vivado integration accepts only the sequential variant. The r8 variant has C/synthesis/RTL evidence but is not the currently integrated IP.

### 2.5 Current AXI and Vivado topology

The HLS kernel has four logical memory masters:

- `m_axi_weights`: 128-bit quant path.
- `m_axi_scales`: generated physical port is 32 bits in the reviewed report.
- `m_axi_activation`: generated physical port is 32 bits in the reviewed report.
- `m_axi_output_r`: 32-bit output path.

The Vivado block design connects all four through a memory SmartConnect to PS `S_AXI_HP0_FPD`. AXI-Lite control reaches the IP from `M_AXI_HPM0_FPD`. The control region is `0xA0000000/64 KiB`. Interrupt is wired, although the current HAL polls.

The block design validates. Implementation, routed timing, bitstream execution, DMA coherency and sustained board bandwidth are not established by that validation.

### 2.6 Existing counters are software estimates

`soc_gemv_stats` records:

- successful calls and rows;
- CPU fallbacks;
- calculated weight payload bytes;
- host monotonic time spent in the backend;
- cache and verification statistics.

These are useful application statistics. They are not accelerator cycle counters or observed AXI byte counters.

## 3. Combination A target

The recommended target contains five blocks:

```text
AXI-Lite doorbell/status
          |
          v
bounded descriptor fetch and validation
          |
          v
command controller ---------------------- counter/result writer
   |             |              |
   v             v              v
activation BRAM  Q8 GEMV core   QKV/FFN post-processing
                     |
                     v
             ping/pong weight tiles
                     |
                     v
                  HP0 DDR
```

Preserve the verified one-job GEMV as a leaf operation and add the command controller around it. Do not replace the golden GEMV and queue controller simultaneously without intermediate tests.

## 4. Descriptor chaining design

### 4.1 Implement only needed opcodes first

Reserve opcode values for future work, but initially implement only:

```text
END
GEMV_Q8
QKV_GROUP
GATE_UP_GROUP
FFN_BLOCK
```

Do not implement `GEMV_Q4`, `LFM_CONV_BLOCK` or `ATTENTION_HEAD` in Combination A. They require different arithmetic, formats and verification. Reserving an enum value is enough.

`RMS_QUANT` and `RESIDUAL_ADD` can be added after chained Q8 GEMV, QKV and gate/up pass. The first queue version may still receive an already normalized and quantized activation from the CPU.

### 4.2 Use a bounded linear descriptor array first

Use a fixed-size, aligned descriptor, for example 256 bytes. The first version should use:

- descriptor base address;
- descriptor count;
- sequential descriptor index;
- explicit END opcode;
- maximum descriptor count checked in hardware and software.

Avoid arbitrary linked-list physical pointers in v1. A bounded array cannot form an accidental cycle and is easier to validate, prefetch and test. It still lets the ARM prepare one layer schedule and ring one doorbell.

A descriptor should include:

- ABI version, opcode, flags and descriptor size;
- quant/scales addresses for up to three matrices;
- activation, output, bias and residual addresses;
- rows and blocks for each matrix;
- result-slot address or index;
- optional activation generation/tag;
- reserved zero fields for future compatible extension.

All addresses are guest/board device addresses, not CPU virtual pointers.

### 4.3 Backward-compatible MMIO

Keep the existing one-job register bank during bring-up. Add queue registers at unused offsets, for example:

| Register | Purpose |
|---|---|
| `QUEUE_BASE_LO/HI` | Descriptor array device address |
| `QUEUE_COUNT` | Number of descriptors, bounded by a compile-time maximum |
| `DOORBELL` | Snapshot queue registers and begin execution |
| `CONSUMER_INDEX` | Last completed descriptor |
| `ERROR_INDEX` | First failed descriptor |
| `ERROR_CODE` | Validation, arithmetic, timeout or unsupported-op error |
| `CYCLES_LO/HI` | Total queue cycles |
| `READ_BYTES_LO/HI` | Logical payload bytes requested |
| `WRITE_BYTES_LO/HI` | Logical payload bytes written |
| `COMPLETED_COUNT` | Successful descriptors |

The legacy register path provides a known fallback and a way to compare chained and unchained execution.

### 4.4 Descriptor validation

Treat descriptor memory as untrusted. Before starting a command, validate:

- ABI version, descriptor size, opcode and flags;
- descriptor count and index bounds;
- dimensions and all size products before multiplication can overflow;
- address alignment and complete DDR range below `0x80000000`;
- buffer non-overlap when writes could corrupt later reads;
- opcode-specific required and forbidden fields;
- activation tag validity before REUSE_X;
- output and result slots before any write.

Snapshot each descriptor before executing it. Do not reread mutable command fields during a job.

On error, stop at a known descriptor index, leave later descriptors untouched, report the error and return to IDLE. The software must discard all outputs whose completion state is not committed.

## 5. Double buffering

The r8 candidate is the practical starting point because it already has explicit row/block tiles and eight independent accumulators. The current default sequential kernel has no weight tile to ping-pong.

Proposed change:

```text
quant_tile[2][8][64 beats]
scale_tile[2][8][32 blocks]

load tile N+1 into buffer B
while compute consumes tile N from buffer A
swap A and B
```

Refactor tile load and tile compute into separate functions connected by ping/pong ownership or HLS streams. Apply `DATAFLOW` only after dependencies and buffer ownership are explicit. Keep each row's FP block accumulation order unchanged.

Verification must compare:

- bit-exact output against `soc_q8r_gemv_ref`;
- achieved load and compute II;
- total transaction latency for identical shapes;
- inferred burst length and outstanding reads;
- BRAM, LUT, FF and DSP changes;
- RTL co-simulation with full and tail tiles.

Double buffering may not improve end-to-end time if HP0 bandwidth is already saturated. Report measured total cycles, not only loop II.

## 6. QKV activation reuse and bias

Qwen Q/K/V use the same normalized, quantized activation and the same input length. A `QKV_GROUP` command should:

1. validate all three matrix descriptions;
2. load and convert x once into activation BRAM;
3. run Q, K and V weight streams consecutively without invalidating x;
4. add optional FP32 Q/K/V bias in the output stage;
5. write three separate output regions;
6. commit completion only after all three outputs are valid.

For the selected model, the logical output storage is small:

```text
Q: 896 floats = 3,584 bytes
K: 128 floats =   512 bytes
V: 128 floats =   512 bytes
total          = 4,608 bytes
```

The group does not need to retain all outputs on chip unless a later attention opcode consumes them. Streaming each result through a bias adder into its output region is sufficient for Combination A.

Add tests for absent bias, all three biases, nonfinite weight scale in each projection, different output addresses, tail row groups and failure without partial completion publication.

## 7. Gate/up reuse and fused FFN

For Qwen2.5-0.5B:

```text
g = Wg x       4,864 FP32 values
u = Wu x       4,864 FP32 values
z = SiLU(g) * u
y = Wd z         896 FP32 values
```

The two FP32 intermediate vectors require:

```text
gate: 4,864 * 4 = 19,456 bytes
up:   4,864 * 4 = 19,456 bytes
total             38,912 bytes
```

A native Q8_0 z vector requires 152 blocks or 5,168 bytes before conversion into the existing internal activation representation. These logical capacities fit comfortably within the device's total BRAM capacity, but banking, port count, double-buffer tiles, FIFOs and FP operator storage must still be synthesized together.

Recommended sequence:

1. `GATE_UP_GROUP`: retain x and compute gate and up into separate BRAM banks.
2. `SWIGLU`: compute SiLU(g) times u and produce one FP32 value at a time.
3. Quantize z in groups of 32 using the exact selected Q8_0 quantization contract.
4. Run the down projection from the on-chip z buffer.
5. Optionally add the FP32 residual while writing the final 896-value output.

The SiLU approximation and Q8_0 quantizer are new numerical blocks. They need independent golden references and error limits before they are combined with the down projection. Do not claim bit-exactness unless the hardware implements the same exponential, rounding and FP16-scale conversion behavior as the chosen software reference.

An incremental fallback is useful: first group gate and up only, reuse x, and write both FP32 outputs to DDR. This proves grouped scheduling and activation reuse before adding SiLU, quantization, down projection and residual logic.

## 8. Cycle and byte counters

Add 64-bit counters with explicit definitions:

- queue cycles: doorbell acceptance through final completion;
- descriptor cycles: descriptor start through committed completion;
- logical read payload bytes;
- logical write payload bytes;
- descriptors completed;
- accelerator errors.

Logical byte counters can increment from validated loop transfers. For one Q8 GEMV, expected payload is:

```text
weight reads = rows * nb * (32 + 2)
x read       = nb * 34, unless x is reused
bias read    = rows * 4 when enabled
y write      = rows * 4
```

For QKV and gate/up groups, count the activation read once. Count descriptor fetch and result writes separately or document that they are excluded.

These counters describe requested payload, not necessarily physical AXI beats, retries or bus occupancy. If true bus utilization is required, add an AXI Performance Monitor in Vivado or expose handshake counters in an RTL wrapper. Do not label calculated payload counters as measured DDR bandwidth.

Per-descriptor results should be written to a separate result array or fixed result slot so the command fields remain immutable while running.

## 9. Software scheduling change

The existing `ggml_compute_forward_mul_mat()` hook is sufficient for one GEMV but not for layer grouping. At the Q call it does not yet have a safe command containing K and V, and each ggml node is expected to be complete when its compute function returns. The same issue applies to gate, up, SiLU, multiply and down nodes.

Therefore Combination A needs a higher-level integration point that can recognize and submit a supported subgraph. Options are:

1. a guarded project-specific subgraph planner before CPU node execution;
2. a small project backend that accepts supported Qwen subgraphs and falls back for everything else;
3. explicit fused graph nodes created in the Qwen graph builder.

Option 2 is the cleanest long-term separation but has the largest initial implementation cost. Option 1 is the smallest private-project change if it remains behind `GGML_SOC_GEMV`, validates the exact pattern and always retains CPU fallback. Option 3 is model-specific and should be avoided unless the schedule cannot be expressed safely elsewhere.

Do not infer QKV or gate/up grouping only from call order or pointer equality. Verify tensor roles, dimensions, shared activation identity, output layout and layer ownership.

The ARM can build a reusable descriptor template after weights have device addresses. Per token it patches dynamic activation, residual and output addresses, performs required cache synchronization, rings one doorbell and waits once per supported layer group.

## 10. Required source changes

### 10.1 New shared ABI

Add one shared C-compatible header, for example:

```text
soc/include/soc_accel_abi.h
```

It should define descriptor/result structs, opcode and flag enums, MMIO offsets, limits and static size/alignment checks. Hardware, HAL, emulation and tests must use the same definitions.

### 10.2 Hardware and HLS

| File or area | Required change |
|---|---|
| `soc/hw/gemv/gemv.cpp`, `gemv.h` | Extract the verified Q8 compute leaf so it can run with a retained activation buffer and caller-selected weight/output addresses. Preserve the old top during transition. |
| New command-controller source under `soc/hw/gemv/` or a focused `soc/hw/combination_a/` directory | Fetch/validate descriptors, hold activation tags, dispatch Q8/QKV/FFN operations, maintain counters and write results. |
| `soc/hw/gemv/build_hls.tcl` | Select a distinct project/top for Combination A, add descriptor/result AXI interfaces, keep strict FP options and preserve old generated projects. |
| `soc/hw/gemv/tb.cpp` plus new controller tests | Add chain, malformed descriptor, QKV, bias, activation reuse, counter, double-buffer, SiLU/quantization and residual cases. |
| `soc/hw/gemv/check_export.tcl` | Validate the new queue/counter ABI without weakening the existing legacy-register checks. |
| `soc/hw/gemv/registers/` | Extend generated-register RTL tests for queue registers, 64-bit counter coherency, doorbell, error index and interrupts. |

Keep the verified sequential and r8 projects unchanged as comparison baselines. Use a new HLS work tag for Combination A.

### 10.3 Golden models

| File or area | Required change |
|---|---|
| `soc/sw/repack/` | Reuse the weight layout and GEMV golden arithmetic. Add exact activation quantization helpers only when the FFN path reaches on-chip z quantization. |
| New Combination A emulation module | Execute descriptor arrays with the same validation, activation-tag and error semantics as hardware. This becomes the software oracle for chain tests. |

### 10.4 HAL and Linux backend

| File | Required change |
|---|---|
| `soc/sw/hal2/soc_gemv_hal.h/.c` | Add queue submission, descriptor/result validation, doorbell, consumer/error polling and counter reads. Keep legacy single-job submission. |
| `soc/sw/hal2/soc_gemv_backend_fpga.c` | Allocate descriptor/result buffers, retain grouped scratch buffers, synchronize the entire command/result ranges and quarantine all live buffers after unknown completion. |
| `soc/sw/hal2/soc_gemv_arena.*` | Reuse existing allocation checks; add no new allocator unless descriptor/result lifetime requires it. |
| HAL tests | Cover overflow, invalid opcode/flags, count limits, descriptor mutation, timeout at each index, partial completion and counter wrap/read consistency. |

### 10.5 llama.cpp integration

| File or area | Required change |
|---|---|
| `soc/sw/gemv/soc_gemv_backend.h` | Add an optional chain/group submission interface while retaining `run()` for fallback and legacy GEMV. |
| `soc/sw/gemv/soc_gemv.c` | Build descriptors from cached weight addresses, track reusable schedule templates and retain strict CPU fallback. |
| `ggml/src/ggml-cpu/ggml-cpu.c` or a new guarded project backend | Move from one-node interception to validated supported-subgraph submission. Keep upstream edits minimal and fully guarded. |
| `soc/sw/gemv/ggml-soc-gemv.cmake` | Add only the new project sources when the feature option is enabled. |

### 10.6 Vivado integration

| File | Required change |
|---|---|
| `soc/hw/vivado/gemv/build_vivado.tcl` | Accept the new exported top and additional descriptor/result masters, update SmartConnect SI count, preserve explicit DDR mappings, optionally add AXI Performance Monitor, and remove the sequential-only guard only after new evidence exists. |
| `soc/hw/vivado/gemv/test_scripts.py` | Check all new interfaces, address mappings, counter/interrupt wiring and rejection of a mismatched legacy export. |

The current `SOC_GEMV_JOBS` Vivado setting controls Vivado implementation parallelism. It is not an accelerator command count or descriptor queue.

## 11. Staged execution plan

### Stage A0: freeze baselines

- Preserve sequential and r8 C/synthesis/RTL reports and generated projects.
- Record the current register ABI and resource numbers.
- Do not overwrite baseline HLS work directories.

Acceptance: current tests still pass with no source behavior change.

### Stage A1: descriptor ABI and software oracle

- Define fixed descriptor/result formats and queue registers.
- Implement descriptor validation and execution in a host emulation model.
- Support `END` and `GEMV_Q8` only.

Acceptance: valid chains match repeated golden GEMVs; malformed chains fail at the exact descriptor with no out-of-range memory access.

### Stage A2: hardware command controller, no optimization

- Add descriptor/result AXI access.
- Dispatch repeated Q8 GEMVs through the verified compute leaf.
- Add cycle and logical byte counters.
- Retain the legacy one-job path.

Acceptance: C simulation and RTL co-simulation pass one-, multi- and error-descriptor chains. One interrupt/poll completion covers the whole chain.

### Stage A3: QKV group

- Retain x across three matrices.
- Add three optional bias streams and separate outputs.
- Verify activation bytes are counted once.

Acceptance: Q/K/V match CPU references with and without bias; one group is measurably cheaper than three legacy submissions.

### Stage A4: r8 plus double buffering

- Promote the verified r8 compute organization into the Combination A leaf.
- Add ping/pong quant/scale tiles and overlap load/compute.
- Compare identical descriptors with buffering on and off.

Acceptance: bit-exact RTL PASS, no regression on tail tiles, lower total cycles on at least one realistic shape, and reviewed resource/timing cost.

### Stage A5: gate/up group

- Retain x and compute gate/up into separate BRAM banks.
- Initially write both results to DDR for a simple integration checkpoint.

Acceptance: both vectors match CPU references and x is loaded once.

### Stage A6: fused FFN

- Add SiLU and elementwise multiply.
- Add Q8_0 z quantization.
- Chain down projection and optional residual add.
- Return only final output.

Acceptance: standalone fused-block error is within the chosen bound for synthetic and real Qwen layer data; model score and generated output remain acceptable.

### Stage A7: llama.cpp subgraph integration

- Recognize only validated Qwen QKV and FFN patterns.
- Build reusable layer descriptor templates from cached device addresses.
- Patch dynamic buffers per token and submit one chain.
- Preserve CPU fallback for every unsupported or failed case.

Acceptance: emulation backend, QEMU/SystemC protocol test if retained, single-layer board test, one-token run and benchmark all pass in order.

### Stage A8: implementation and board measurement

- Run HLS synthesis/RTL co-simulation for the selected configuration.
- Export IP, validate Vivado BD, implement, route and check setup/hold timing.
- Run board smoke before full inference.
- Compare counters with software-calculated bytes and external elapsed time.

Acceptance: matching BIT/HWH/XSA, nonnegative routed timing, no DMA/coherency errors, measured speedup over the legacy one-job path and recorded power/resource data.

## 12. Verification matrix

| Boundary | Required checks |
|---|---|
| Descriptor parser | version/opcode/flags, count bounds, multiplication overflow, address range/alignment, overlap, immutable snapshot, exact error index |
| Q8 leaf | existing finite FP16 sweep, numerical patterns, real Qwen slices, max/tail dimensions, nonfinite scale behavior |
| Chain controller | zero/one/max descriptors, END, first/middle/last failure, completion ordering, one doorbell, timeout quarantine |
| Counters | known tiny descriptors, activation reuse counted once, 64-bit rollover/read consistency, explicit payload definition |
| Double buffer | full/tail tiles, ping/pong ownership assertions, output equality, total-cycle comparison |
| QKV | Q/K/V dimensions, bias/no-bias, separate output regions, shared x, partial-failure publication rules |
| Gate/up | shared x, two complete BRAM outputs, real layer vectors |
| SiLU/quantize | special FP values policy, rounding, scale encoding, zero blocks, max error and model-level accuracy |
| Fused FFN | gate/up/down real tensors, residual alias rules, final output comparison |
| HAL/backend | descriptor/result DMA sync, locks, timeout poison, process death, reset recovery, CPU fallback |
| Vivado/board | all masters mapped to low DDR, routed timing, UIO map, DMA allocator, coherency, counters versus external measurements |

## 13. Main risks and decisions needed

1. **The current default is not the best base for double buffering.** Sequential GEMV is integrated, while r8 has the tile structure and II=1 evidence. Decide whether Combination A promotes r8 before adding the queue.
2. **Full FFN fusion is much larger than descriptor chaining.** SiLU, quantization and graph integration are new numerical and software subsystems. Keep them behind separate acceptance gates.
3. **The current hook is at the wrong abstraction level for safe grouping.** A layer/subgraph scheduler is required; call-order guessing is not sufficient.
4. **Board DMA is still unverified.** A command queue increases the amount of persistent device-visible memory and the consequence of stale cache lines. Resolve the allocator/coherency path before full-model board execution.
5. **Logical bytes are not AXI utilization.** Add external bus instrumentation if competitive claims depend on measured bandwidth.
6. **One HP port may limit overlap.** Double buffering can hide compute or memory latency only when the other phase has spare bandwidth/time. Measure before adding a second port.
7. **Do not mix Q4, convolution and attention into the first controller.** Their inclusion would delay validation of the required Q8 path.

## 14. Recommended first implementation boundary

Start with Stage A1 and A2 only:

```text
bounded linear descriptor array
END and GEMV_Q8 opcodes
legacy-compatible MMIO plus doorbell
total/per-descriptor cycles
logical read/write payload counters
software oracle and malformed-descriptor tests
```

This boundary proves descriptor chaining and counters without changing Q8 arithmetic. After it passes, add QKV activation reuse, then double buffering, then gate/up and the fused FFN. That ordering isolates failures and preserves the verified GEMV as a reference throughout the work.

## 15. What double buffering means here

Double buffering is a hardware dataflow pattern, not an intrinsic optimization that HLS inserts by itself. The original r8 candidate had one local weight-quant tile and one local scale tile. Section 21 records the later two-bank implementation; the explanation and risks here remain the promotion criteria.

A ping-pong implementation would allocate two quant/scale tile banks. While the compute stage consumes bank A for tile k, a load stage fills bank B with tile k+1. The banks swap roles at the next boundary. HLS pragmas such as `DATAFLOW`, pipelining and streams can schedule the overlap only after the source is divided into independent load and compute stages with explicit ownership. A pragma alone does not remove the array dependency or create the second bank.

The ideal tiled time changes from approximately:

```text
sum(load_tile + compute_tile)
```

to:

```text
initial_load + sum(max(load_tile, compute_tile)) + final_compute
```

Challenges for this design are:

- the second tile bank consumes more BRAM;
- the four masters currently share one PS HP port, so overlap may expose bandwidth contention instead of hiding latency;
- load and compute must use independent ports and bank ownership without an HLS scheduling cycle;
- scale validation, tail tiles and early errors need deterministic drain/stop behavior;
- the verified FP block order must remain unchanged;
- r8 is the natural tiled base, but the currently integrated default is the sequential kernel;
- C simulation success does not prove DATAFLOW overlap; synthesis and RTL co-simulation reports must show it.

Original decision: defer double buffering until a phase breakdown showed that tile loading was material. Section 21 supersedes that choice at the user's request by implementing it as a separately tagged experiment. Keep it only if Vitis proves overlap and board profiling shows enough gain; otherwise select r8 with double buffering disabled.

## 16. Feasible implementation on `combination-a-feasible`

The separate branch implements a measurement and compatibility foundation without changing HLS arithmetic:

1. `soc_gemv_hal_run_chain()` accepts 1 to 256 existing scalar jobs.
2. It prevalidates the complete array before the first start.
3. It stops at the first runtime failure and reports its exact index/status.
4. It records per-job and total host elapsed time, logical read/write bytes and MACs.
5. It reports both activation bytes requested by scalar jobs and unique bytes for consecutive jobs sharing the same activation address and shape. The difference quantifies the maximum payload opportunity for later QKV or gate/up reuse.
6. The llama.cpp hook statistics now separate weight, activation and output bytes, MACs, cache hits, fallback time and backend latency range.
7. `SOC_GEMV_PROFILE=<path.csv>` adds an optional per-GEMV trace.
8. `soc/sw/gemv/profile_trace.py` aggregates the trace by cached tensor and shape and can calculate an Amdahl bound from a separately measured wall time.
9. `soc/sw/gemv/run_profile_experiment.sh` creates separate timing and trace runs with identical forwarded `llama-bench` arguments.

This chain still performs one AXI-Lite start/poll sequence per job. It does not fetch descriptors in PL, reduce interrupts, retain x in BRAM or overlap jobs. Calling it descriptor chaining in performance results would be incorrect; it is a software compatibility chain and profiling oracle for the future hardware ABI.

No QKV bias, SiLU, on-chip requantization, residual add or graph-pattern fusion was added. The current ggml hook cannot safely observe a complete QKV or FFN subgraph, so adding those operations below the hook would rely on unsafe call-order guesses.

## 17. Profiling and experiment plan

Use two kinds of runs. Timing runs have tracing disabled. Diagnostic runs enable the CSV trace.

### 17.1 Controlled end-to-end matrix

Keep model, prompt, generated tokens, thread count, affinity, CPU governor/clock and board bitstream fixed. Run at least three repetitions after one warm-up:

| Run | Configuration | Purpose |
|---|---|---|
| CPU | `SOC_GEMV=0` | End-to-end software baseline |
| Emulation | `SOC_GEMV=1 SOC_GEMV_BACKEND=emul` | Hook/cache/repack overhead and correctness, not acceleration |
| FPGA scalar | `SOC_GEMV_BACKEND=fpga` | Existing one-job hardware baseline |
| FPGA scalar plus trace | Add `SOC_GEMV_PROFILE=gemv-trace.csv` | Shape/hot-tensor diagnosis; do not use as competitive timing |

For each timing run record prompt-processing tokens/s, generation tokens/s, total wall time, first-token/warm-up behavior, correctness/score, CPU utilization and the final `SOC_GEMV_STATS=1` block. On FPGA also record the implemented PL clock, temperature if available and exact bitstream/commit.

### 17.2 Trace analysis

Run the controlled experiment from the repository root on LRC or the board:

```bash
MODEL=/absolute/path/to/qwen2.5-0.5b-instruct-q8_0.gguf
BENCH=./build/bin/llama-bench
OUT=./profile-results/combination-a-$(date +%Y%m%d-%H%M%S)

bash soc/sw/gemv/run_profile_experiment.sh \
    "$BENCH" "$MODEL" "$OUT" -p 32 -n 32 -r 5 -t 1
```

This creates:

| File | Meaning |
|---|---|
| `cpu.stdout.txt` | llama-bench CPU baseline result |
| `cpu.wall.txt` | complete CPU process wall time |
| `emul.stdout.txt` | llama-bench result through the emulation backend |
| `emul.stderr.txt` | banner and aggregate GEMV statistics |
| `emul.wall.txt` | complete emulation process wall time |
| `emul_trace.csv` | one diagnostic row per GEMV |
| `emul_trace.wall.txt` | wall time for the exact traced interval |

The emulation backend proves control flow and produces a software overhead baseline. It is not an FPGA-speed estimate.

After `board_smoke` passes and the correct bitstream/UIO/DMA setup is active, add FPGA cases:

```bash
SOC_GEMV_RUN_FPGA=1 \
bash soc/sw/gemv/run_profile_experiment.sh \
    "$BENCH" "$MODEL" "$OUT-fpga" -p 32 -n 32 -r 5 -t 1
```

The runner sets `SOC_GEMV_STRICT=1` for offloaded cases. A backend error therefore fails the experiment instead of silently mixing FPGA and CPU results.

Inspect the aggregate output:

```bash
cat "$OUT/emul.stderr.txt"
cat "$OUT/cpu.wall.txt" "$OUT/emul.wall.txt" "$OUT/emul_trace.wall.txt"
```

Extract the trace wall time and calculate an Amdahl bound:

```bash
WALL=$(sed -n 's/^wall_seconds=//p' "$OUT/emul_trace.wall.txt")
python3 soc/sw/gemv/profile_trace.py "$OUT/emul_trace.csv" --wall-seconds "$WALL"
```

The reported elapsed values are host monotonic-clock measurements around the backend. The byte counts are validated logical payloads. Neither is a hardware cycle count or physical AXI traffic measurement.

Each CSV row contains:

| Field | Interpretation |
|---|---|
| `call` | Process-local GEMV sequence number |
| `tensor_id` | Stable ID assigned to the cached weight tensor in this process |
| `rows`, `n` | GEMV output and input dimensions |
| `path` | `backend`, `fallback` or `invalid` |
| `cache_hit` | Weight tensor was already present in the repack cache |
| `weight_bytes` | Logical repacked Q8 weight plus scale payload |
| `activation_bytes` | Native Q8_0 input-vector payload |
| `output_bytes` | FP32 output-vector payload |
| `macs` | `rows * n` logical MACs |
| `elapsed_ns` | Host time around the selected backend or CPU fallback |

The analyzer sorts tensors by total backend time. This shows which shapes dominate execution, whether repeated cache hits are working, effective logical weight rate, effective GMAC/s, fallbacks and the maximum possible end-to-end gain from eliminating the measured backend interval. Compare `cpu.stdout.txt`, `emul.stdout.txt` and later `fpga.stdout.txt` for generation tokens/s; use the untraced timing cases for performance claims.

### 17.3 Hardware measurements needed before optimization choices

- Use HLS/RTL reports or a PL free-running counter for kernel cycles.
- Add explicit phase counters for activation load, weight load, compute and output only if those counters do not alter the critical schedule.
- Use an AXI Performance Monitor or handshake counters for actual read/write beats, stalls and utilization.
- Compare scalar-chain activation bytes with unique activation bytes for QKV and gate/up microbenchmarks.
- Compare r1 and r8 latency/resources first. Implement ping-pong tiles only if the measured load/compute split predicts a useful gain.
- Measure command overhead with tiny GEMVs. If it is negligible beside weight streaming, true descriptor DMA should follow QKV/FFN work rather than precede it.

### 17.4 Interpretation rules

- Do not compare emulation-backend time with FPGA time as an accelerator result.
- Do not sum per-call host times and call them PL cycles.
- Do not call logical Q8 payload divided by time measured DDR bandwidth.
- Report median and range, not only the best run.
- Re-run model accuracy whenever arithmetic, quantization, SiLU or fusion changes.
- Use the Amdahl bound to reject optimizations whose best possible end-to-end gain is too small.

The requested `profiling.txt` was not present under the local project workspace when this branch was created. Its experiments must be reconciled with this matrix when the file is supplied; no missing proposal is claimed as implemented.

## 18. Implemented files and verification status

Changed implementation files:

- `soc/sw/hal2/soc_gemv_hal.h` and `.c`: bounded compatibility chain and results.
- `soc/sw/hal2/test_hal.c`: success, partial-failure, validation and counter tests.
- `soc/sw/gemv/soc_gemv.h` and `.c`: expanded statistics and optional CSV trace.
- `soc/sw/gemv/test_gemv.c`: logical-counter and cache-hit assertions.
- `soc/sw/gemv/profile_trace.py`: offline aggregation and Amdahl calculation.
- `soc/sw/gemv/run_profile_experiment.sh`: reproducible separated timing/trace runs.
- GEMV/HAL READMEs: semantics and commands.

Local verification completed on 2026-11-22:

- strict C compilation of the changed HAL and GEMV control sources passed with GCC;
- mock-MMIO HAL control, chain, profiling-counter, ownership and arena tests passed;
- the cache/emulation suite passed 89 offloaded GEMVs and 48,256 verified rows bit-exactly, including the new logical counters and cache-hit assertions;
- a real 90-row CSV trace from that suite was accepted and aggregated by `profile_trace.py`;
- Python syntax, synthetic aggregation and Bash syntax checks passed;
- `git diff --check` passed.

The local MinGW installation did not provide the UBSan runtime, so a sanitizer-linked run was not completed here. Run the existing Linux tests with the project sanitizer flags on LRC if sanitizer evidence is required.

Still required on LRC/Linux before using this branch as evidence:

```bash
make -C soc/sw/hal2 test
cd soc/sw
gcc -O2 -std=c11 -Wall -Wextra -Werror -ffp-contract=off -Irepack -Igemv \
    -o /tmp/test_gemv gemv/test_gemv.c gemv/soc_gemv.c \
    gemv/soc_gemv_backend_emul.c repack/soc_repack.c -lm -lpthread
/tmp/test_gemv
```

No Vitis, Vivado, QEMU/SystemC or Ultra96 result is claimed for these changes.

## 19. Expected return versus implementation cost

Activation reuse should not be justified as a major DDR-byte optimization by itself. For the current Qwen dimensions:

### QKV

```text
x payload                    = 28 blocks * 34 B = 952 B
Q weight payload             = 896 * 28 * 34 B = 852,992 B
K or V weight payload        = 128 * 28 * 34 B = 121,856 B
total QKV weight payload     = 1,096,704 B
three scalar x reads         = 2,856 B
one grouped x read           = 952 B
x bytes eliminated           = 1,904 B
```

The eliminated x payload is about 0.17% of QKV weight payload. QKV grouping may still help by removing two activation-load phases, two start/poll sequences and intermediate graph scheduling. Bias addition can be folded into output with little extra cost. Measure those fixed costs before claiming a useful speedup.

### Gate/up

```text
one gate or up weight matrix = 4,864 * 28 * 34 B = 4,630,528 B
gate plus up weights         = 9,261,056 B
x bytes eliminated           = 952 B
```

The direct x-byte reduction is about 0.01% of gate/up weights. Gate/up grouping alone is therefore a control/activation-load optimization, not a bandwidth solution.

Full FFN fusion has a stronger reason than input reuse alone. It can avoid publishing the 38,912-byte FP32 gate/up pair to DDR, reading it on the CPU for SiLU/multiply, writing a 5,168-byte Q8_0 z vector and starting the down projection separately. Even this traffic is small compared with the three FFN weight matrices, so the measured CPU SiLU/quantization time and synchronization cost must justify the numerical and BRAM complexity.

Recommended priority after the current profiling foundation:

| Priority | Work | Gate before implementation |
|---|---|---|
| 1 | Run CPU/emulation/FPGA scalar profiles | Establish end-to-end and per-shape baselines |
| 2 | Compare verified r1 and r8 kernel cycles/resources | Select the compute base from measurements |
| 3 | Add phase or AXI counters | Identify load, compute, output and control shares |
| 4 | True PL descriptor queue | Proceed if AXI-Lite/poll overhead is material, especially for small K/V jobs |
| 5 | Ping-pong weight tiles | Proceed if load time is hideable and HP0 has bandwidth headroom |
| 6 | QKV group plus bias | Proceed with a safe subgraph scheduler and measured fixed-overhead opportunity |
| 7 | Gate/up then fused FFN | Proceed only if CPU post-processing/synchronization is a material remainder |

Cycle/byte counters remain high value even when they do not directly accelerate inference because they prevent incorrect optimization claims and guide every later stage.

## 20. First 100 MHz HLS result and immediate decision

The LRC `f100_b64_o8` sequential export has now passed C simulation and Verilog RTL co-simulation bit-exactly. HLS estimates 7 BRAM18K, 12 DSP, 6,058 FF and 9,395 LUT, with a 7.300 ns estimated clock path against the 10.00 ns target. The complete evidence and claim limits are recorded in `soc/docs/results/combination_a_hls_f100_r1_review.md`.

The report identifies two scheduling limits: `load_x` achieves II=42 and the main `read_words` loop achieves II=3. Therefore the next compute experiment is the existing row-interleave 8 candidate at the same 100 MHz configuration. Combination A now adds a separately tagged ping-pong tile experiment to that r8 base; the generated DATAFLOW schedule, rather than the existence of two source arrays, decides whether load/compute overlap was realized.

The initial decision was to retain the sequential default until a board baseline existed. Section 21 supersedes that selection because the teammate has supplied a comparable baseline. The combined design uses the distinct `f100_b64_o8_r8_db` tag and must pass C simulation, synthesis/dataflow review, RTL co-simulation, Vivado timing and board correctness gates. The r8-only form remains selectable with `SOC_GEMV_HW_DOUBLE_BUFFER=0`. This comparison is performed on `combination-a-feasible`; a new branch is not required. Separately named artifacts provide the experiment boundary.

## 21. r8, ping-pong tile buffering and counter instrumentation

The teammate has already established a comparable scalar board baseline, so this branch now selects row interleave 8 and double-buffered weight/scale tiles by default and instruments that design for the next board comparison. The r8 kernel preloads tile 0, computes from one on-chip bank while requesting the next tile into the other bank, and alternates the banks. Each bank holds 16 Q8 blocks. Qwen's common 896-element input is 28 blocks, so QKV and gate/up use two tiles and can exercise the overlap; two 16-block banks retain the former single 32-block tile's total logical storage. `SOC_GEMV_HW_DOUBLE_BUFFER=0` retains r8 with sequential load/compute phases; row interleave 1 remains available as the scalar reference and requires double buffering 0.

Double buffering here is an HLS architecture, not an automatic consequence of using HLS. Two explicit BRAM banks remove the storage conflict; a DATAFLOW region requests concurrent load and compute tasks. C simulation proves numerical equivalence but cannot prove concurrency. Promotion requires the Vitis dataflow/schedule report to show the load and compute processes overlapping, RTL co-simulation to remain bit-exact, and resource/timing results to remain acceptable. If Vitis serializes the tasks, the design is only ping-pong storage and should not be reported as a performance optimization.

The HLS IP publishes successful-job logical read bytes, write bytes and MACs through new read-only AXI-Lite outputs. Vivado adds a 32-bit AXI Timer at `control_base + 0x1000`, clocked from PL0. The HAL measures from timer start immediately before GEMV submission until DONE and IDLE are observed, checks every HLS output-valid flag, and rejects a successful job if hardware logical counters disagree with its validated shape.

The timer reports the real PL-clock submission/completion window, including fixed AXI-Lite start and observation overhead. It is called `pl_cycles`, not pure kernel cycles. The HLS byte counters are logical payload, not AXI beat or DDR-traffic counters. llama.cpp aggregate statistics and CSV traces carry both these hardware values and host elapsed nanoseconds.

The full Git, LRC, Vitis HLS, Vivado, cross-build, Ultra96, smoke-test, llama.cpp and evidence workflow is `soc/docs/COMBINATION_A_R8_COUNTERS_END_TO_END.md`.
