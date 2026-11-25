# LRC baseline profiling

Purpose: native function counts and caller/callee analysis.

Models:
- Qwen2.5-0.5B-Instruct, Q8_0
- LFM2-350M, Q8_0
- LFM2-700M, Q8_0

Workload:
- Prompt: Describe a system-on-chip in two sentences.
- Requested generated tokens: 10
- One inference thread and one batch thread
- Context: 512
- Batch and microbatch: 128
- Flash Attention disabled
- Warmup disabled
- Accelerator integration disabled

Interpretation:
- gprof: optimized CPU build with profiling instrumentation.
- Callgrind: portable diagnostic build with several CPU optimizations disabled.
- Reports cover the whole process, including initialization.
- Ten requested generated tokens do not guarantee ten decode evaluations.
- These are LRC results, not Ultra96 performance measurements.

See environment.txt and CMakeCache files for build and host details.
Only treat runs that completed successfully as valid profiles.
