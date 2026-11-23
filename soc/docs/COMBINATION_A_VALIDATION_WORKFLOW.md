# Combination A validation workflow

> Superseded for the r8 plus hardware-counter design by `soc/docs/COMBINATION_A_R8_COUNTERS_END_TO_END.md`. The older steps below describe the pre-instrumentation scalar ABI and must not be used with the new BIT/HWH/HAL.

Date: 2026-11-22. Branch: `combination-a-feasible`.

This workflow separates what can be proved on a Windows workstation, an x86-64 LRC host, the Xilinx tools and the physical Ultra96. Preserve the complete log and source commit for every vendor or board run.

## 1. Current scope

This branch adds:

- a bounded HAL compatibility chain over the existing scalar GEMV engine;
- logical byte/MAC and host-time counters;
- optional per-GEMV CSV tracing and analysis;
- tests and documentation.

It does not change `soc/hw/gemv/gemv.cpp`, the HLS register map, the Vivado block design or the BIT/HWH interface. A previously generated, matching scalar GEMV BIT/HWH remains compatible. Rebuild it only when reproducible artifacts are needed or the existing artifact cannot be tied to the same hardware source/configuration.

The compatibility chain issues one scalar hardware start per job. It is not a PL descriptor-fetch controller, does not retain x in BRAM across jobs and does not implement QKV/FFN fusion. The newer r8 candidate does request double-buffered tile load/compute overlap; follow `COMBINATION_A_R8_COUNTERS_END_TO_END.md` and require Vitis DATAFLOW evidence before claiming that overlap. Do not infer the other features from a successful board run.

## 2. What each environment can validate

| Environment | Valid evidence | Not validated there |
|---|---|---|
| Windows/PowerShell | Git identity, file presence, review | Linux builds, HLS, MMIO, DMA |
| LRC x86-64 shell | Native C/C++ tests, fallback behavior, script checks, trace analysis | FPGA backend initialization or physical hardware |
| Vitis HLS on LRC | C simulation, synthesis schedule/resources, generated RTL co-simulation, IP export | PS integration, routed timing, board coherency |
| Vivado on LRC | Block design, address map, implementation, timing, BIT/HWH/XSA | Physical register/DMA behavior |
| Ultra96 Linux/AArch64 | UIO mapping, DMA/coherency, hardware arithmetic, llama.cpp correctness/performance | HLS simulation coverage by itself |

QEMU/SystemC checks a software/model protocol. It does not execute the generated HLS RTL and is not a replacement for RTL co-simulation or board DMA tests.

## 3. Source identity gate

Run on Windows before pushing and on LRC after pulling:

```bash
git branch --show-current
git status --short
git rev-parse HEAD
git rev-parse origin/combination-a-feasible
git rev-list --left-right --count HEAD...origin/combination-a-feasible
```

Require branch `combination-a-feasible`, a clean status and `0 0` from `rev-list`. Record the commit:

```bash
mkdir -p soc/docs/results/combination_a
git rev-parse HEAD | tee soc/docs/results/combination_a/source_commit.txt
```

On LRC, also record tools:

```bash
uname -a | tee soc/docs/results/combination_a/lrc_environment.txt
cc --version | head -1 | tee -a soc/docs/results/combination_a/lrc_environment.txt
python3 --version | tee -a soc/docs/results/combination_a/lrc_environment.txt
```

## 4. LRC host tests: no FPGA access

Run from the repository root on an x86-64 LRC host:

```bash
set -o pipefail
make -C soc/sw/hal2 clean
make -C soc/sw/hal2 test 2>&1 | tee soc/docs/results/combination_a/hal_host_test.log
```

Expected passes include:

```text
HAL control/chain/profiling/ownership and arena tests: PASS
HOST NEGATIVE TEST: the next FPGA initialization refusal is expected
soc_gemv fpga: Linux/AArch64 board required
soc_gemv: backend 'fpga' init failed
HOST NEGATIVE TEST: unavailable FPGA backend uses CPU fallback: PASS
```

The refusal is correct because the LRC host is not the Ultra96. This target tests safe CPU fallback and must not initialize UIO or DMA.

Run the cache/emulation test:

```bash
cd soc/sw
gcc -O2 -std=c11 -Wall -Wextra -Werror -ffp-contract=off \
    -Irepack -Igemv -o /tmp/test_gemv \
    gemv/test_gemv.c gemv/soc_gemv.c \
    gemv/soc_gemv_backend_emul.c repack/soc_repack.c \
    -lm -lpthread
/tmp/test_gemv 2>&1 | tee ../docs/results/combination_a/gemv_emulation_test.log
cd ../..
```

Require `ALL TESTS PASSED`, 89 verified GEMVs and 100% bit-exact verified rows.

Run the native HLS arithmetic and script guards:

```bash
make -C soc/hw/gemv test 2>&1 | tee soc/docs/results/combination_a/gemv_native_test.log
make -C soc/hw/gemv script-test 2>&1 | tee soc/docs/results/combination_a/hls_script_test.log
python3 soc/hw/vivado/gemv/test_scripts.py \
    2>&1 | tee soc/docs/results/combination_a/vivado_script_test.log
```

Require `GEMV HW: PASS` with zero failures. Mocked script tests validate control flow and guards only; they do not constitute Vitis, Vivado or board evidence.

## 5. Decide whether to rebuild hardware

No HLS/Vivado source changed on this branch. Choose one path:

1. Reuse an existing BIT/HWH only if its commit, HLS configuration, control base and implemented clock are recorded and match this scalar interface.
2. Run a clean reproducibility build when artifact provenance is missing or the course submission needs artifacts from the final source revision.

Never mix a BIT from one build with an HWH from another.

## 6. Complete HLS validation on LRC

Initialize the teammate-verified Xilinx 2022.2 environment:

```bash
module load xilinx/2022
source /usr/local/packages/Xilinx_2022.2/Vivado/2022.2/settings64.sh
module load gcc/14
vitis_hls -version
vivado -version
```

The maintained rebuild script selects the reviewed scalar configuration: 100 MHz, burst 64, outstanding 8, row interleave 1 and target `xczu3eg-sbva484-1-e`.

For a clean rebuild, the script requires that its generated HLS directory not already exist. Preserve an existing generated directory before intentionally rebuilding it:

```bash
test ! -e soc/hw/hls_work/gemv/f100_b64_o8 || \
    mv soc/hw/hls_work/gemv/f100_b64_o8 \
       soc/hw/hls_work/gemv/f100_b64_o8.saved.$(date +%Y%m%d-%H%M%S)
bash soc/tools/rebuild_lrc.sh hls
```

Expected log:

```text
GEMV HW: PASS; ... failures=0
P3.1 GEMV HLS stage export completed
```

The `export` stage runs C simulation, synthesis, Verilog RTL co-simulation and IP export. Preserve:

```text
soc/docs/results/lrc_rebuild_100/hls_export.log
soc/hw/hls_work/gemv/f100_b64_o8/solution/syn/report/
soc/hw/hls_work/gemv/f100_b64_o8/solution/sim/report/
soc/hw/hls_work/gemv/f100_b64_o8/solution/impl/ip/
```

Review the achieved loop schedule, port widths, burst inference, resources and clock estimate. A C-simulation pass alone is insufficient.

## 7. Vivado block design and bitstream on LRC

Set the board repository that the teammate has already verified for this account. Do not copy another user's scratch path without confirming it exists:

```bash
export SOC_GEMV_BOARD_REPO=/absolute/path/to/XilinxBoardStore/boards/Avnet
test -d "$SOC_GEMV_BOARD_REPO"
```

Run block-design validation first:

```bash
bash soc/tools/rebuild_lrc.sh bd
```

Require:

```text
Post-validation reset checks PASS
P4.1 block design saved; control base 0xA0000000
```

Review:

```bash
cat soc/hw/vivado/gemv_f100_b64_o8/address_map.csv
grep -E 'ERROR|CRITICAL WARNING' \
    soc/docs/results/lrc_rebuild_100/vivado_bd.log
```

The expected control segment is `0xA0000000/64KiB`; all four GEMV masters must map to low DDR through HP0. Stop on an error or unexplained critical warning.

Generate implementation artifacts only after the BD review:

```bash
bash soc/tools/rebuild_lrc.sh bitstream
```

Require the final message identifying matching BIT/HWH/XSA and verify:

```bash
ls -lh soc/hw/out/gemv_f100_b64_o8.bit \
       soc/hw/out/gemv_f100_b64_o8.hwh \
       soc/hw/out/gemv_f100_b64_o8.xsa
grep -E 'WNS|WHS|Timing constraints are met' \
    soc/hw/vivado/gemv_f100_b64_o8/timing_summary.rpt
cat soc/hw/vivado/gemv_f100_b64_o8/clocks.rpt
sha256sum soc/hw/out/gemv_f100_b64_o8.{bit,hwh,xsa}
```

Vivado must report the implemented PL0 period as 10 ns and nonnegative setup/hold slack. A generated `.bit` without these checks is not a passing implementation.

## 8. Cross-build board software on LRC

Use the maintained build:

```bash
bash soc/tools/rebuild_lrc.sh software
/usr/bin/cmake --build build-ultra96-bringup \
    --target llama-cli llama-server llama-bench -j 8
make -C soc/sw/hal2 CC=aarch64-linux-gnu-gcc board_registers board_smoke
file build-ultra96-bringup/bin/llama-cli \
     build-ultra96-bringup/bin/llama-bench \
     soc/sw/hal2/board_registers soc/sw/hal2/board_smoke
```

Require AArch64 executables. The CMake configure log must contain `SoC GEMV offload enabled` and include `GGML_SOC_GEMV_FPGA=ON`.

The repository target is `llama-cli`; there is no target named `command-cli` in this branch.

## 9. Transfer with artifact identity

From LRC, replace `BOARD` with the teammate's established SSH destination:

```bash
BOARD=user@board-host
scp soc/hw/out/gemv_f100_b64_o8.bit \
    soc/hw/out/gemv_f100_b64_o8.hwh \
    build-ultra96-bringup/bin/llama-cli \
    build-ultra96-bringup/bin/llama-bench \
    soc/sw/hal2/board_registers \
    soc/sw/hal2/board_smoke \
    "$BOARD":/root/combination-a/
```

Copy the model only if it is not already on the board. Compare `sha256sum` on LRC and the board before loading hardware.

Use a fresh board boot before replacing an overlay. A prior board attempt had a stalled control read and USB failure; a successful build alone does not authorize ignoring that condition.

## 10. Board gates in safe order

Run these commands on the Ultra96, not on LRC.

First record the board and inspect devices before loading/replacing an overlay:

```bash
uname -a
uname -m
free -h
ls -l /dev/uio* /dev/udmabuf* 2>/dev/null
grep -H . /sys/class/uio/uio*/maps/map0/{name,addr,size,offset} 2>/dev/null
grep -H . /sys/class/u-dma-buf/udmabuf*/{phys_addr,size,sync_mode,dma_coherent} 2>/dev/null
```

Require `aarch64`. Load the matching BIT/HWH using the teammate's already validated PYNQ/course procedure. Do not invent a second overlay-loading method.

After loading, identify the UIO entry whose map is `0xA0000000` and run the no-START register test. For example, if it is `uio0`:

```bash
cd /root/combination-a
./board_registers 0 0xa0000000
```

Expected:

```text
GEMV BOARD REGISTERS: PASS; no START or DMA submitted by this test
```

This test writes/restores parameter registers but submits no accelerator job. Stop if the first register access stalls or destabilizes the board.

Only after register access and the approved DMA-visible allocator are confirmed:

```bash
./board_smoke
```

Expected:

```text
FPGA board GEMV smoke: PASS
```

This proves repeated small GEMVs, changed weights, numerical equality, error-output quarantine and post-error recovery through the real MMIO/DMA path. It does not prove full-model memory capacity or competitive speed.

## 11. llama.cpp correctness on the board

Start with a short strict verification run:

```bash
MODEL=/absolute/path/to/qwen2.5-0.5b-instruct-q8_0.gguf
env SOC_GEMV=1 \
    SOC_GEMV_BACKEND=fpga \
    SOC_GEMV_VERIFY=1 \
    SOC_GEMV_STATS=1 \
    SOC_GEMV_STRICT=1 \
    taskset -c 0 ./llama-cli \
    -m "$MODEL" -t 1 -tb 1 -n 8 \
    -p "Describe a system-on-chip."
```

Require `backend=fpga`, no fallback, exact/accepted verification statistics and normal text completion. Verification recomputes results on the CPU and is not a speed run.

Then run without verification for performance:

```bash
env SOC_GEMV=1 \
    SOC_GEMV_BACKEND=fpga \
    SOC_GEMV_VERIFY=0 \
    SOC_GEMV_STATS=1 \
    SOC_GEMV_STRICT=1 \
    taskset -c 0 ./llama-bench \
    -m "$MODEL" -p 32 -n 32 -r 5 -t 1
```

Record prompt-processing and generation tokens/s, total backend time, logical payloads, fallback count, CPU clock/core state, PL clock, bitstream hash and commit.

For per-GEMV diagnosis, add a trace in a separate run:

```bash
env SOC_GEMV=1 \
    SOC_GEMV_BACKEND=fpga \
    SOC_GEMV_VERIFY=0 \
    SOC_GEMV_STATS=1 \
    SOC_GEMV_STRICT=1 \
    SOC_GEMV_PROFILE=/root/combination-a/fpga_trace.csv \
    taskset -c 0 ./llama-bench \
    -m "$MODEL" -p 32 -n 32 -r 1 -t 1
```

CSV writes perturb timing. Use the untraced run for speed claims and the trace only to locate hot shapes/cache behavior. Analyze the copied trace with `soc/sw/gemv/profile_trace.py` on LRC.

## 12. Pass/fail ladder

Advance only when the current boundary passes:

1. Clean, identical Git commit.
2. LRC host tests pass; expected non-board refusal is labeled PASS.
3. HLS C simulation and generated RTL co-simulation match the golden model.
4. Synthesis reports are reviewed for schedule/resources/interfaces.
5. Vivado BD address/reset topology passes.
6. Implemented clock and setup/hold timing pass; BIT/HWH/XSA match.
7. Fresh-board UIO mapping and register test pass.
8. DMA allocator/coherency checks and `board_smoke` pass.
9. Strict short llama.cpp verification passes with zero fallback.
10. Untraced performance and official accuracy benchmark are recorded.

If a boundary fails, preserve the first complete log and do not reinterpret a later-stage result as proof of the failed boundary.
