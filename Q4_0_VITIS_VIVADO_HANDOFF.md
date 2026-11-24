# Q4_0 LRC, Vitis, Vivado, and Ultra96 build handoff

This handoff starts with the Windows branch, validates the focused Q4_0 components on LRC, generates the HLS IP and Ultra96 hardware, cross-compiles llama.cpp, and packages the deployment artifacts. Run commands in the shell named by each section. Stop on the first failed expected marker.

## Why this order

| Stage | What it produces or confirms |
|---|---|
| Host kernel test | Confirms Q4_0 packing, Q8_0 activation layout, arithmetic, and status handling before using an FPGA tool license |
| HAL test | Confirms register offsets, pointer programming, start/completion polling, and buffer alignment |
| Dispatcher and driver checks | Confirms cache/fallback behavior and that the emulation, UIO, and XRT sources compile |
| Vitis C simulation | Confirms the same testbench builds and runs with the Vitis 2022.2 C compiler and HLS types |
| Vitis synthesis | Converts the C++ kernel to RTL and reports latency, loop II, estimated timing, and device resources |
| RTL co-simulation | Runs the C testbench against generated RTL and catches C-to-RTL behavior differences |
| HLS export | Packages the passing RTL as Vivado IP and generates the software register header |
| Vivado check/block design | Confirms the IP ABI and connects control, DDR AXI masters, clock, reset, and interrupt to the Ultra96 processing system |
| Vivado bitstream | Places and routes the full design, checks real setup/hold timing, and creates `.bit`, `.hwh`, and `.xsa` |
| AArch64 llama build | Builds the board executable with the Q4_0 ggml hook, XRT DMA allocator, and UIO register driver |
| Packaging | Binds the exact source commit, hardware, executable, reports, and hashes into one deployable set |

The AArch64 llama build is not a hardware build and does not generate the FPGA. It is technically independent of Vivado, but doing it after the bitstream is recommended here: the component and hardware gates fail faster, and the final executable is then built from the same frozen commit and register ABI as the generated hardware.

## 1. Push the sparse branch from Windows

Run in Windows PowerShell:

```powershell
git config --global --add safe.directory 'C:/Niru-personal/Sem3/SoC Design/Project/soc-project-gemv-q4-0'

Set-Location 'C:\Niru-personal\Sem3\SoC Design\Project\soc-project-gemv-q4-0'
git branch --show-current
git status --short
git diff --check
git diff --stat gemv-q4
```

Expected branch: `gemv-q4-0`. The branch base is the passing Q4_K commit `28308437c`; the diff should contain only Q4_0-specific source, tests, build scripts, and handoff documentation.

The `safe.directory` entry is needed only because the worktree was created by the isolated Codex Windows account and is now being used by your normal Windows account. It trusts only this exact repository path. Do not use a wildcard safe-directory entry.

Review and stage the work:

```powershell
git diff -- ggml/src/ggml-cpu/ggml-cpu.c soc
git diff -- Q4_0_VITIS_VIVADO_HANDOFF.md handoff-file-organization.md
git add -- ggml/src/ggml-cpu/ggml-cpu.c soc Q4_0_VITIS_VIVADO_HANDOFF.md handoff-file-organization.md
git diff --cached --check
git diff --cached --stat
```

Write your own commit message, then push:

```powershell
git commit
git push -u origin gemv-q4-0
git rev-parse HEAD
```

Record the last hash. The LRC checkout must match it.

## 2. Create a clean LRC run directory

Connect from PowerShell:

```powershell
ssh nkg557@mario.ece.utexas.edu
```

Run in LRC bash:

```bash
set -euo pipefail
export LRC_USER=nkg557
export PROJECT_ROOT=/misc/scratch/$LRC_USER/SoC/Project
export RUN=$PROJECT_ROOT/results/gemv-q4-0-$(date +%Y%m%d-%H%M%S)
export REPO=$RUN/repo
mkdir -p "$RUN/logs"
git clone --branch gemv-q4-0 --single-branch https://github.com/pnnsumedh09/soc-project.git "$REPO"
cd "$REPO"
git status --short --branch
git rev-parse HEAD | tee "$RUN/source-commit.txt"
```

Expected: a clean checkout and the same commit hash recorded in PowerShell.

## 3. Repeat the small host checks on LRC

Run in LRC bash from `$REPO`:

```bash
module load gcc/14
mkdir -p "$RUN/host-test"

g++ -std=c++11 -O2 -Wall -Wextra -Werror -ffp-contract=off \
  -DSOC_GEMV_HW_HOST -Isoc/sw/repack -Isoc/hw/gemv \
  soc/hw/gemv/gemv.cpp soc/hw/gemv/tb.cpp soc/hw/gemv/tb_repack.cpp \
  -o "$RUN/host-test/q4_0_kernel_test"

"$RUN/host-test/q4_0_kernel_test" | tee "$RUN/logs/q4_0-kernel-host.log"

gcc -std=c11 -O2 -Wall -Wextra -Werror \
  -Isoc/sw/repack -Isoc/sw/hal2 \
  soc/sw/hal2/soc_gemv_hal.c soc/tests/q4_0_hal_test.c \
  -o "$RUN/host-test/q4_0_hal_test"

"$RUN/host-test/q4_0_hal_test" | tee "$RUN/logs/q4_0-hal-host.log"

gcc -std=c11 -O2 -Wall -Wextra -Werror -ffp-contract=off \
  -Isoc/sw/repack -Isoc/sw/gemv -Isoc/sw/hal2 \
  soc/sw/repack/soc_repack.c \
  soc/sw/gemv/soc_gemv.c \
  soc/sw/gemv/soc_gemv_backend_emul.c \
  soc/tests/q4_0_dispatch_test.c \
  -lpthread -lm -o "$RUN/host-test/q4_0_dispatch_test"

SOC_GEMV_BACKEND=emul SOC_GEMV_VERIFY=1 SOC_GEMV_STATS=1 \
  "$RUN/host-test/q4_0_dispatch_test" \
  2>&1 | tee "$RUN/logs/q4_0-dispatch-host.log"
```

Expected final markers:

```text
Q4_0 GEMV: PASS
Q4_0 HAL: PASS
Q4_0 dispatcher and cache: PASS
```

The dispatcher statistics must show two offloaded GEMVs, eight verified rows, one intentional oversized-shape CPU fallback, and 100% bit-exact verified rows.

Compile-check both FPGA driver variants before HLS:

```bash
export CHECK_FLAGS='-std=c11 -Wall -Wextra -Werror -ffp-contract=off -fsyntax-only -Isoc/sw/repack -Isoc/sw/gemv -Isoc/sw/hal2'

gcc $CHECK_FLAGS soc/sw/repack/soc_repack.c
gcc $CHECK_FLAGS soc/sw/gemv/soc_gemv.c
gcc $CHECK_FLAGS soc/sw/gemv/soc_gemv_backend_emul.c
gcc $CHECK_FLAGS soc/sw/hal2/soc_gemv_hal.c
gcc $CHECK_FLAGS soc/sw/hal2/soc_gemv_arena.c
gcc $CHECK_FLAGS soc/sw/hal2/soc_gemv_backend_fpga.c
gcc $CHECK_FLAGS -DSOC_GEMV_XRT_TEST \
  -Isoc/third_party/xrt/include/xrt \
  soc/sw/hal2/soc_gemv_backend_fpga_xrt.c

echo 'Q4_0 HAL/UIO/XRT compile checks: PASS' \
  | tee "$RUN/logs/q4_0-driver-compile.log"
```

Do not start HLS unless all three tests and all driver compile checks pass.

## 4. Set the Vitis environment

Run in the same LRC bash shell:

```bash
module load xilinx/2022
source /usr/local/packages/Xilinx_2022.2/Vivado/2022.2/settings64.sh
cd "$REPO"

export SOC_GEMV_HW_MHZ=100
export SOC_GEMV_HW_BURST=64
export SOC_GEMV_HW_OUTSTANDING=8
export SOC_GEMV_HW_ROW_INTERLEAVE=1
export SOC_GEMV_HW_PART=xczu3eg-sbva484-1-e
```

The expected configuration tag is `q4_0_f100_b64_o8` and the target clock period is 10 ns.

## 5. Run Vitis C simulation

```bash
SOC_GEMV_HW_STAGE=csim vitis_hls -f soc/hw/gemv/build_hls.tcl \
  2>&1 | tee "$RUN/logs/vitis-q4_0-csim.log"

grep -E 'Q4_0 GEMV: PASS|Q4_0 GEMV C simulation completed' \
  "$RUN/logs/vitis-q4_0-csim.log"
```

Expected: both lines are present. This validates the Vitis compiler, repacking, scale handling, Q4_0 nibble mapping, error status, and kernel arithmetic.

## 6. Run synthesis

```bash
SOC_GEMV_HW_STAGE=synth vitis_hls -f soc/hw/gemv/build_hls.tcl \
  2>&1 | tee "$RUN/logs/vitis-q4_0-synth.log"

grep 'Q4_0 GEMV HLS stage synth completed' "$RUN/logs/vitis-q4_0-synth.log"
```

Inspect the reports:

```bash
export HLS_SOLUTION=$REPO/soc/hw/hls_work/gemv/q4_0_f100_b64_o8/solution
test -f "$HLS_SOLUTION/syn/report/soc_gemv_hw_csynth.rpt"
cp "$HLS_SOLUTION/syn/report/soc_gemv_hw_csynth.rpt" "$RUN/logs/"
grep -E 'Timing|Latency|Interval|BRAM|DSP|FF|LUT|dot_values' \
  "$HLS_SOLUTION/syn/report/soc_gemv_hw_csynth.rpt" \
  | tee "$RUN/logs/q4_0-hls-summary.txt"
```

Expected:

- part `xczu3eg-sbva484-1-e`
- target period 10 ns
- loop `dot_values` appears
- no synthesis error
- resource use fits the Ultra96 target

Do not infer timing closure from C simulation. Record the achieved clock estimate, loop II, latency, and resources from this report.

## 7. Run RTL co-simulation

```bash
SOC_GEMV_HW_STAGE=cosim vitis_hls -f soc/hw/gemv/build_hls.tcl \
  2>&1 | tee "$RUN/logs/vitis-q4_0-cosim.log"

grep -E 'Q4_0 GEMV: PASS|Q4_0 GEMV HLS stage cosim completed' \
  "$RUN/logs/vitis-q4_0-cosim.log"
```

Expected: RTL co-simulation completes and the testbench prints `Q4_0 GEMV: PASS`. Stop if C simulation passes but RTL co-simulation differs.

## 8. Export the Q4_0 IP

```bash
SOC_GEMV_HW_STAGE=export vitis_hls -f soc/hw/gemv/build_hls.tcl \
  2>&1 | tee "$RUN/logs/vitis-q4_0-export.log"

grep 'Q4_0 GEMV HLS stage export completed' "$RUN/logs/vitis-q4_0-export.log"
test -f "$HLS_SOLUTION/impl/ip/component.xml"
```

The generated register ABI must contain:

| Register | Offset |
|---|---:|
| `w_quants` | `0x10` |
| `w_scales` | `0x20` |
| `x_native` | `0x30` |
| `y` | `0x40` |
| `rows` | `0x50` |
| `nb` | `0x58` |
| `status` | `0x60` |
| `status_valid` | `0x64` |

The Vivado preflight checks these offsets automatically.

## 9. Optional Vitis GUI review

Run in a graphical LRC session:

```bash
vitis_hls -p soc/hw/hls_work/gemv/q4_0_f100_b64_o8
```

Open `solution`, then Analysis and Schedule Viewer. Inspect `dot_values`, the integer accumulator dependency, the FP block accumulator, memory bursts, achieved period, and BRAM/DSP use.

## 10. Build the Vivado design and bitstream

Run in LRC bash:

```bash
export SOC_GEMV_BOARD_REPO=/misc/scratch/nkg557/SoC/Project/XilinxBoardStore/boards/Avnet
export SOC_GEMV_BOARD_PART=avnet.com:ultra96v2:part0:1.2
export SOC_GEMV_CONTROL_BASE=0xA0000000
export SOC_GEMV_JOBS=8
test -d "$SOC_GEMV_BOARD_REPO"

SOC_GEMV_BUILD_STAGE=check vivado -mode batch \
  -source soc/hw/vivado/gemv/build_vivado.tcl \
  2>&1 | tee "$RUN/logs/vivado-q4_0-check.log"

SOC_GEMV_BUILD_STAGE=bd vivado -mode batch \
  -source soc/hw/vivado/gemv/build_vivado.tcl \
  2>&1 | tee "$RUN/logs/vivado-q4_0-bd.log"

SOC_GEMV_BUILD_STAGE=bitstream vivado -mode batch \
  -source soc/hw/vivado/gemv/build_vivado.tcl \
  2>&1 | tee "$RUN/logs/vivado-q4_0-bitstream.log"
```

Expected artifacts:

```bash
ls -lh soc/hw/out/gemv_q4_0_f100_b64_o8.bit \
       soc/hw/out/gemv_q4_0_f100_b64_o8.hwh \
       soc/hw/out/gemv_q4_0_f100_b64_o8.xsa
```

Vivado must report a 10 ns PL0 period and nonnegative setup and hold slack. The control block must be at `0xA0000000` with a 64 KiB range. Do not package inherited `gemv_f100_b64_o8.*` or Q4_K artifacts.

Verify the implementation reports explicitly:

```bash
export VIVADO_WORK=$REPO/soc/hw/vivado/gemv_q4_0_f100_b64_o8
test -f "$VIVADO_WORK/timing_summary.rpt"
test -f "$VIVADO_WORK/utilization.rpt"
test -f "$VIVADO_WORK/clocks.rpt"

grep -E 'WNS|TNS|WHS|THS|All user specified timing constraints are met|Timing constraints are not met' \
  "$VIVADO_WORK/timing_summary.rpt" \
  | tee "$RUN/logs/q4_0-vivado-timing-summary.txt"

grep -E 'CLB LUTs|CLB Registers|Block RAM Tile|DSPs' \
  "$VIVADO_WORK/utilization.rpt" \
  | tee "$RUN/logs/q4_0-vivado-utilization-summary.txt"

grep -E 'pl_clk0|10\.000|100\.000' "$VIVADO_WORK/clocks.rpt" \
  | tee "$RUN/logs/q4_0-vivado-clocks-summary.txt"

cp "$VIVADO_WORK/timing_summary.rpt" "$RUN/logs/"
cp "$VIVADO_WORK/utilization.rpt" "$RUN/logs/"
cp "$VIVADO_WORK/clocks.rpt" "$RUN/logs/"
```

Stop if either setup or hold slack is negative. The Tcl build also checks this and should terminate before copying deployment artifacts when timing fails.

## 11. Cross-compile llama-cli and llama-bench for Ultra96

Run in the same LRC bash shell. Use the full LRC clone, not the sparse Windows worktree:

```bash
cd "$REPO"
module load gcc/14

export TOOLCHAIN=/home/projects/gerstl/ece382m/ultra96_toolchain.cmake
export XRT_HEADERS=$REPO/soc/third_party/xrt/include/xrt

test -f "$TOOLCHAIN"
test -f "$XRT_HEADERS/xrt.h"
cmake --version

cmake -S "$REPO" -B "$RUN/build-ultra96" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_SOC_GEMV=ON \
  -DGGML_SOC_GEMV_FPGA=ON \
  -DGGML_SOC_GEMV_XRT=ON \
  -DSOC_GEMV_XRT_INCLUDE_DIR="$XRT_HEADERS" \
  -DGGML_NATIVE=OFF \
  -DGGML_OPENMP=OFF \
  -DGGML_LLAMAFILE=OFF \
  -DGGML_CPU_REPACK=OFF \
  -DLLAMA_CURL=OFF \
  -DLLAMA_OPENSSL=OFF \
  -DLLAMA_BUILD_UI=OFF \
  -DLLAMA_USE_PREBUILT_UI=OFF \
  -DLLAMA_BUILD_TESTS=OFF \
  -DGGML_BUILD_TESTS=OFF \
  -DGGML_BUILD_EXAMPLES=ON \
  -DBUILD_SHARED_LIBS=OFF \
  2>&1 | tee "$RUN/logs/ultra96-configure.log"

cmake --build "$RUN/build-ultra96" --target llama-cli llama-bench -j8 \
  2>&1 | tee "$RUN/logs/ultra96-build.log"
```

Confirm that the intended sources and architecture were selected:

```bash
grep -E 'SoC GEMV offload enabled|CMAKE_SYSTEM_PROCESSOR|GGML_SYSTEM_ARCH' \
  "$RUN/logs/ultra96-configure.log"

file "$RUN/build-ultra96/bin/llama-cli"
file "$RUN/build-ultra96/bin/llama-bench"
test -x "$RUN/build-ultra96/bin/llama-cli"
test -x "$RUN/build-ultra96/bin/llama-bench"
```

Expected:

- configure log contains `SoC GEMV offload enabled`
- executables are AArch64/ARM64 Linux, not x86-64
- the build does not download UI assets
- both executable checks succeed

If `file` reports x86-64, delete only `$RUN/build-ultra96`, verify the toolchain path, and configure again. Do not deploy the wrong architecture.

## 12. Verify and package all deployment artifacts

Run in LRC bash:

```bash
cd "$REPO"
export DEPLOY=$RUN/deploy
mkdir -p "$DEPLOY/reports"

cp soc/hw/out/gemv_q4_0_f100_b64_o8.bit "$DEPLOY/"
cp soc/hw/out/gemv_q4_0_f100_b64_o8.hwh "$DEPLOY/"
cp soc/hw/out/gemv_q4_0_f100_b64_o8.xsa "$DEPLOY/"
cp "$RUN/build-ultra96/bin/llama-cli" "$DEPLOY/"
cp "$RUN/build-ultra96/bin/llama-bench" "$DEPLOY/"
cp "$RUN/source-commit.txt" "$DEPLOY/"
cp "$VIVADO_WORK/timing_summary.rpt" "$DEPLOY/reports/"
cp "$VIVADO_WORK/utilization.rpt" "$DEPLOY/reports/"
cp "$VIVADO_WORK/clocks.rpt" "$DEPLOY/reports/"
cp "$HLS_SOLUTION/syn/report/soc_gemv_hw_csynth.rpt" "$DEPLOY/reports/"

(cd "$DEPLOY" && sha256sum \
  gemv_q4_0_f100_b64_o8.bit \
  gemv_q4_0_f100_b64_o8.hwh \
  gemv_q4_0_f100_b64_o8.xsa \
  llama-cli llama-bench > SHA256SUMS)

(cd "$DEPLOY" && sha256sum -c SHA256SUMS)

tar -C "$RUN" -czf "$RUN/gemv-q4-0-ultra96.tar.gz" deploy logs source-commit.txt
ls -lh "$RUN/gemv-q4-0-ultra96.tar.gz"
find "$DEPLOY" -maxdepth 2 -type f -printf '%P\n' | sort
```

Expected deployment contents:

```text
SHA256SUMS
gemv_q4_0_f100_b64_o8.bit
gemv_q4_0_f100_b64_o8.hwh
gemv_q4_0_f100_b64_o8.xsa
llama-bench
llama-cli
source-commit.txt
reports/clocks.rpt
reports/soc_gemv_hw_csynth.rpt
reports/timing_summary.rpt
reports/utilization.rpt
```

This deployment directory is the teammate handoff. The four required runtime files are:

```text
gemv_q4_0_f100_b64_o8.bit
gemv_q4_0_f100_b64_o8.hwh
gemv_q4_0_f100_b64_o8.xsa
llama-cli
```

Also send `SHA256SUMS` and `source-commit.txt`. They let your teammate prove that the hardware and executable came from the same source revision and were not corrupted during transfer. `llama-bench` and the reports are recommended evidence but are not required to launch `llama-cli`.

## 13. Copy the package back to Windows

First print the exact LRC path:

```bash
echo "$RUN/gemv-q4-0-ultra96.tar.gz"
```

Then exit LRC and run in Windows PowerShell, replacing the timestamp with the printed directory:

```powershell
Set-Location 'C:\Niru-personal\Sem3\SoC Design\Project'

scp nkg557@mario.ece.utexas.edu:/misc/scratch/nkg557/SoC/Project/results/gemv-q4-0-YYYYMMDD-HHMMSS/gemv-q4-0-ultra96.tar.gz .

Get-Item .\gemv-q4-0-ultra96.tar.gz
```

Verify the downloaded archive without extracting it:

```powershell
tar -tzf .\gemv-q4-0-ultra96.tar.gz
Get-FileHash .\gemv-q4-0-ultra96.tar.gz -Algorithm SHA256
```

Send `gemv-q4-0-ultra96.tar.gz` and its PowerShell SHA256 value to your teammate. After extraction on Linux, your teammate should run this from the extracted `deploy` directory before programming the board:

```bash
sha256sum -c SHA256SUMS
file llama-cli
```

Expected: every hash says `OK`, and `llama-cli` reports an AArch64/ARM64 Linux executable.

## 14. Completion gate before programming Ultra96

Do not move to board execution until all of these are true:

- host kernel says `Q4_0 GEMV: PASS`
- HAL test says `Q4_0 HAL: PASS`
- dispatcher/cache test passes with the intentional single fallback
- UIO and XRT driver warning-as-error checks pass
- Vitis C simulation passes
- Vitis RTL co-simulation passes
- HLS report targets 10 ns and fits the device
- generated register ABI matches `w_scales` at `0x20`
- Vivado setup and hold timing both pass
- artifact names contain `q4_0`
- `llama-cli` and `llama-bench` are AArch64 executables
- deployment SHA256 verification passes

After these steps pass, the generated hardware and executables are ready for Ultra96 programming and strict verification with a real Q4_0 GGUF.
