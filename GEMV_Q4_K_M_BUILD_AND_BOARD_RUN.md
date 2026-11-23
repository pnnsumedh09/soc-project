# Q4_K_M GEMV: push, LRC build, Vitis/Vivado, and Ultra96 run

This is the Q4_K_M-specific replacement for the golden-branch build notes. Run each block in the shell named above it. Stop at a failed expected check; do not package an old Q8_0 bitstream with this branch.

The accelerated operation is decode-time `Q4_K x Q8_K` GEMV. A Q4_K_M GGUF also contains Q6_K and sometimes other tensor types; those deliberately remain on llama.cpp's CPU paths. The accelerator accepts at most 19 Q4_K superblocks (4864 columns) and 151936 rows. Larger Q4_K operations use the normal CPU vector-dot fallback.

The branch retains both golden profiling modes:

- `SOC_GEMV_PROFILE=1`: per-call XRT/HAL phase timing.
- `SOC_GEMV_STATS=1`: aggregate cache, repack, offload, fallback, and verification statistics.

The software and host simulation have been validated locally. Vitis HLS, Vivado timing closure, and the Ultra96 run must be completed on the LRC/board before this hardware is considered validated.

## 1. Review, commit, and push from Windows

Run in **PowerShell on your Windows machine**. The implementation is in the clean sibling worktree, not the interrupted original checkout.

```powershell
Set-Location 'C:\Niru-personal\Sem3\SoC Design\Project\soc-project-gemv-q4'
git branch --show-current
git merge-base HEAD 0157aed4bd14a471d97e523573d108a15c33f77a
git show -s --format='%H %s' 0157aed4bd14a471d97e523573d108a15c33f77a
git status --short
git diff --check
```

Expected:

- branch: `gemv-q4`
- both commit hashes printed by the middle commands: `0157aed4bd14a471d97e523573d108a15c33f77a`, the fetched `gemv-golden` tip used as this branch's base
- only the Q4_K source, build scripts, testbench, README, and this runbook are changed
- `git diff --check` reports no errors (line-ending warnings on Windows are not errors)

Review the full patch before staging:

```powershell
git diff -- ggml/src/ggml-cpu/ggml-cpu.c soc
git diff -- GEMV_Q4_K_M_BUILD_AND_BOARD_RUN.md
```

Stage only this work:

```powershell
git add -- ggml/src/ggml-cpu/ggml-cpu.c soc GEMV_Q4_K_M_BUILD_AND_BOARD_RUN.md
git status --short
git diff --cached --check
git diff --cached --stat
```

Create the commit yourself using your team's normal message and disclosure policy. The repository instructions require a human-written commit message; do not copy an AI-generated commit or PR description.

```powershell
git commit
git log -1 --oneline
git push -u origin gemv-q4
```

Expected push ending: a new remote branch named `gemv-q4`. Record the exact commit for the LRC comparison:

```powershell
git rev-parse HEAD
```

## 2. Fetch an isolated LRC copy

Run in **PowerShell**:

```powershell
ssh nkg557@mario.ece.utexas.edu
```

Run the rest of this section in the **LRC bash shell**. If your ECE login differs, change only `LRC_USER` and the scratch prefix.

```bash
set -euo pipefail
export LRC_USER=nkg557
export PROJECT_ROOT=/misc/scratch/$LRC_USER/SoC/Project
export RUN=$PROJECT_ROOT/results/gemv-q4-$(date +%Y%m%d-%H%M%S)
export REPO=$RUN/repo
mkdir -p "$RUN"
git clone --branch gemv-q4 --single-branch https://github.com/pnnsumedh09/soc-project.git "$REPO"
cd "$REPO"
git status --short --branch
git rev-parse HEAD | tee "$RUN/source-commit.txt"
ln -sfn "$RUN" "$PROJECT_ROOT/gemv-q4-latest"
```

Expected: a clean `gemv-q4` checkout. The hash must equal the PowerShell `git rev-parse HEAD` result. If the repository is private, use your approved SSH URL or credential method rather than putting a token in a command.

## 3. LRC host validation before FPGA tools

Run in the **LRC bash shell** from `$REPO`:

```bash
module load gcc/14
mkdir -p "$RUN/logs" "$RUN/host-test"
gcc -std=c11 -O2 -ffp-contract=off -Isoc/sw/repack \
  -c soc/sw/repack/soc_repack.c -o "$RUN/host-test/soc_repack.o"
g++ -std=c++11 -O2 -ffp-contract=off -DSOC_GEMV_HW_HOST \
  -Isoc/sw/repack -Isoc/hw/gemv \
  soc/hw/gemv/gemv.cpp soc/hw/gemv/tb.cpp "$RUN/host-test/soc_repack.o" \
  -o "$RUN/host-test/q4k_tb"
"$RUN/host-test/q4k_tb" | tee "$RUN/logs/host-csim.log"
```

Expected final line:

```text
Q4_K GEMV: PASS
```

Also build the real llama.cpp integration in emulation mode:

```bash
cmake -S . -B "$RUN/build-emul" \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_SOC_GEMV=ON \
  -DGGML_SOC_GEMV_FPGA=OFF \
  -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF \
  -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF -DGGML_BUILD_TESTS=OFF \
  -DBUILD_SHARED_LIBS=OFF
cmake --build "$RUN/build-emul" --target llama-cli llama-bench -j8 \
  2>&1 | tee "$RUN/logs/emul-build.log"
```

To exercise an actual model, point `MODEL` to a trusted GGUF whose filename/metadata identifies `Q4_K_M`. Do not requantize an already-quantized model; if conversion is necessary, start from F16/BF16.

```bash
export MODEL=/absolute/path/to/model.Q4_K_M.gguf
test -f "$MODEL"
SOC_GEMV=1 SOC_GEMV_BACKEND=emul SOC_GEMV_VERIFY=1 SOC_GEMV_STATS=1 \
  "$RUN/build-emul/bin/llama-cli" -m "$MODEL" -p 'Explain GEMV in one sentence.' -n 4 \
  2>&1 | tee "$RUN/logs/emul-model.log"
```

Expected: the emulation backend is enabled, eligible Q4_K calls are offloaded, verification reports no mismatches, and the program generates tokens. CPU execution for Q6_K tensors is normal.

## 4. Vitis HLS validation and export

Run in the **LRC bash shell**:

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

First run C simulation:

```bash
SOC_GEMV_HW_STAGE=csim vitis_hls -f soc/hw/gemv/build_hls.tcl \
  2>&1 | tee "$RUN/logs/vitis-csim.log"
grep -E 'Q4_K GEMV: PASS|Q4_K GEMV C simulation completed' "$RUN/logs/vitis-csim.log"
```

Then synthesize and RTL co-simulate. Co-simulation invokes synthesis first:

```bash
SOC_GEMV_HW_STAGE=cosim vitis_hls -f soc/hw/gemv/build_hls.tcl \
  2>&1 | tee "$RUN/logs/vitis-cosim.log"
grep -E 'Q4_K GEMV: PASS|Q4_K GEMV HLS stage cosim completed' "$RUN/logs/vitis-cosim.log"
```

Finally create the IP export used by Vivado:

```bash
SOC_GEMV_HW_STAGE=export vitis_hls -f soc/hw/gemv/build_hls.tcl \
  2>&1 | tee "$RUN/logs/vitis-export.log"
grep 'Q4_K GEMV HLS stage export completed' "$RUN/logs/vitis-export.log"
test -f soc/hw/hls_work/gemv/q4k_f100_b64_o8/solution/impl/ip/component.xml
```

Inspect the synthesis reports:

```bash
export HLS_DIR=$REPO/soc/hw/hls_work/gemv/q4k_f100_b64_o8/solution
cp "$HLS_DIR/syn/report/soc_gemv_hw_csynth.rpt" "$RUN/logs/"
grep -E 'Timing|Latency|Interval|BRAM|DSP|FF|LUT|read_quant_words|group_values' \
  "$HLS_DIR/syn/report/soc_gemv_hw_csynth.rpt" | tee "$RUN/logs/hls-summary.txt"
```

Expected: no HLS errors, part `xczu3eg-sbva484-1-e`, target clock 10 ns, Q4_K loops such as `read_quant_words` in the report, and a successful co-simulation. Treat timing/resource results as measured data; do not assume closure from C simulation alone.

For GUI schedule inspection, run on a graphical LRC session:

```bash
vitis_hls -p soc/hw/hls_work/gemv/q4k_f100_b64_o8
```

Open solution `solution`, then Analysis/Schedule Viewer. Check the feedback path in `group_values`, memory stalls in `read_quant_words`, initiation intervals, DSP/BRAM use, and the 10 ns target.

## 5. Vivado block design and bitstream

Run in the **same LRC bash shell**. The nkg557 path below comes from the existing team setup; change it only if the board store is elsewhere.

```bash
export SOC_GEMV_BOARD_REPO=/misc/scratch/nkg557/SoC/Project/XilinxBoardStore/boards/Avnet
export SOC_GEMV_BOARD_PART=avnet.com:ultra96v2:part0:1.2
export SOC_GEMV_CONTROL_BASE=0xA0000000
export SOC_GEMV_JOBS=8
test -d "$SOC_GEMV_BOARD_REPO"
```

Check the exported IP and ABI without building the design:

```bash
SOC_GEMV_BUILD_STAGE=check vivado -mode batch -source soc/hw/vivado/gemv/build_vivado.tcl \
  2>&1 | tee "$RUN/logs/vivado-check.log"
```

Expected: the Q4_K export is found, registers are accepted (including `w_metadata` at `0x20`), target clock matches 100 MHz, and no design is generated.

Build the block design, then the bitstream:

```bash
SOC_GEMV_BUILD_STAGE=bd vivado -mode batch -source soc/hw/vivado/gemv/build_vivado.tcl \
  2>&1 | tee "$RUN/logs/vivado-bd.log"
SOC_GEMV_BUILD_STAGE=bitstream vivado -mode batch -source soc/hw/vivado/gemv/build_vivado.tcl \
  2>&1 | tee "$RUN/logs/vivado-bitstream.log"
```

Expected output files:

```bash
ls -lh soc/hw/out/gemv_q4k_f100_b64_o8.bit \
       soc/hw/out/gemv_q4k_f100_b64_o8.hwh \
       soc/hw/out/gemv_q4k_f100_b64_o8.xsa
```

Check tool failures and timing explicitly:

```bash
if grep -E '(^|[[:space:]])(ERROR|CRITICAL WARNING):' "$RUN/logs/vivado-bitstream.log"; then
  echo 'Review Vivado errors/critical warnings before continuing' >&2
  exit 1
fi
find soc/hw/vivado/gemv_q4k_f100_b64_o8 -type f \
  \( -name '*timing_summary*.rpt' -o -name '*utilization*.rpt' \) -print \
  | tee "$RUN/logs/vivado-report-files.txt"
while IFS= read -r report; do
  cp "$report" "$RUN/logs/$(basename "$report")"
  grep -E 'WNS|TNS|WHS|THS|Timing constraints are not met|All user specified timing constraints are met' "$report" || true
done < "$RUN/logs/vivado-report-files.txt"
```

Do not proceed if setup/hold timing fails, reset validation fails, the control address is not `0xA0000000` with a 64 KiB range, or the artifacts retain a Q8_0 name.

## 6. Cross-compile the Ultra96 executable

Run in the **LRC bash shell**:

```bash
export TOOLCHAIN=/home/projects/gerstl/ece382m/ultra96_toolchain.cmake
export XRT_HEADERS=$REPO/soc/third_party/xrt/include/xrt
test -f "$TOOLCHAIN"
test -f "$XRT_HEADERS/xrt.h"
cmake -S "$REPO" -B "$RUN/build-ultra96" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_SOC_GEMV=ON \
  -DGGML_SOC_GEMV_FPGA=ON \
  -DGGML_SOC_GEMV_XRT=ON \
  -DSOC_GEMV_XRT_INCLUDE_DIR="$XRT_HEADERS" \
  -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DGGML_LLAMAFILE=OFF \
  -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF -DGGML_BUILD_TESTS=OFF \
  -DGGML_BUILD_EXAMPLES=ON -DBUILD_SHARED_LIBS=OFF
cmake --build "$RUN/build-ultra96" --target llama-cli llama-bench -j8 \
  2>&1 | tee "$RUN/logs/ultra96-build.log"
file "$RUN/build-ultra96/bin/llama-cli" "$RUN/build-ultra96/bin/llama-bench"
```

Expected: both files are AArch64/ARM64 Linux executables, not x86-64. This branch uses XRT/ZOCL for physically contiguous buffers and generic UIO for the accelerator control registers.

## 7. Package and copy results

Run in the **LRC bash shell**:

```bash
export DEPLOY=$RUN/deploy
mkdir -p "$DEPLOY/reports"
cp soc/hw/out/gemv_q4k_f100_b64_o8.{bit,hwh,xsa} "$DEPLOY/"
cp "$RUN/build-ultra96/bin/llama-cli" "$RUN/build-ultra96/bin/llama-bench" "$DEPLOY/"
cp "$RUN/source-commit.txt" "$DEPLOY/"
cp "$RUN/logs/"*timing_summary*.rpt "$DEPLOY/reports/" 2>/dev/null || true
(cd "$DEPLOY" && sha256sum gemv_q4k_f100_b64_o8.bit gemv_q4k_f100_b64_o8.hwh \
  gemv_q4k_f100_b64_o8.xsa llama-cli llama-bench > SHA256SUMS)
tar -C "$RUN" -czf "$RUN/gemv-q4-ultra96.tar.gz" deploy logs
ls -lh "$RUN/gemv-q4-ultra96.tar.gz"
```

Run in **Windows PowerShell** to download the package (replace the timestamped path with the one printed above):

```powershell
Set-Location 'C:\Niru-personal\Sem3\SoC Design\Project'
scp nkg557@mario.ece.utexas.edu:/misc/scratch/nkg557/SoC/Project/results/gemv-q4-YYYYMMDD-HHMMSS/gemv-q4-ultra96.tar.gz .
```

If the board cannot `scp` from LRC, unpack the archive on Windows and serve only the deploy directory with your team-approved HTTP/cloudflared method. Do not expose the repository or credentials.

## 8. Ultra96 programming and runtime validation

Run these commands in the **Ultra96 PYNQ Linux shell as root**. The prior golden environment used PYNQ 3.0.1, ZOCL/XRT, and 640 MiB CMA; verify rather than assuming those settings.

```bash
mkdir -p /root/gemv-q4
cd /root/gemv-q4
# Copy or download the five deploy files here, then:
sha256sum -c SHA256SUMS
chmod +x llama-cli llama-bench
uname -m
cat /proc/cmdline
grep -i CmaTotal /proc/meminfo
ls -l /dev/dri /dev/uio* 2>/dev/null
ldconfig -p | grep -E 'libxrt_core\.so|libxrt_coreutil\.so'
```

Expected: `aarch64`, hashes pass, ZOCL/XRT devices/libraries exist, and CMA matches the known board image. Load the Q4_K bitstream using the same reviewed PYNQ overlay/programming method used for `gemv-golden`, but with `gemv_q4k_f100_b64_o8.bit` and `.hwh`. In a PYNQ Python shell this is typically:

```python
from pynq import Overlay
ol = Overlay('/root/gemv-q4/gemv_q4k_f100_b64_o8.bit', download=True)
print(ol.ip_dict)
print(ol.clock_dict)
```

Expected: `soc_gemv_hw` is mapped at `0xA0000000`, its range is at least `0x10000`, and PL0 is 100 MHz. Stop if the overlay metadata differs.

Find the matching generic UIO device in the **board shell**:

```bash
export SOC_GEMV_UIO_DEVICE=
export SOC_GEMV_UIO_SYSFS=
for u in /sys/class/uio/uio*; do
  test -e "$u/maps/map0/addr" || continue
  addr=$(cat "$u/maps/map0/addr")
  if test "$((addr))" -eq "$((0xA0000000))"; then
    export SOC_GEMV_UIO_DEVICE=/dev/$(basename "$u")
    export SOC_GEMV_UIO_SYSFS=$u/maps/map0
    break
  fi
done
test -n "$SOC_GEMV_UIO_DEVICE"
echo "Using $SOC_GEMV_UIO_DEVICE with $SOC_GEMV_UIO_SYSFS"
```

Copy a real `Q4_K_M` model to the board and run a short strict correctness check first:

```bash
export MODEL=/root/models/model.Q4_K_M.gguf
test -f "$MODEL"
export LD_LIBRARY_PATH=/usr/lib:/usr/local/lib:${LD_LIBRARY_PATH:-}
SOC_GEMV=1 \
SOC_GEMV_BACKEND=fpga \
SOC_GEMV_UIO_DEVICE="$SOC_GEMV_UIO_DEVICE" \
SOC_GEMV_UIO_SYSFS="$SOC_GEMV_UIO_SYSFS" \
SOC_GEMV_VERIFY=1 \
SOC_GEMV_STRICT=1 \
SOC_GEMV_PROFILE=1 \
SOC_GEMV_STATS=1 \
taskset -c 0-3 ./llama-cli -m "$MODEL" -p 'Explain GEMV in one sentence.' -n 4 -t 4 \
  2>&1 | tee q4k-fpga-verify.log
```

Expected:

- backend reports `fpga` enabled
- eligible Q4_K offload calls and cache hits become nonzero
- XRT profile lines include activation copy/sync, quant and metadata sync, HAL submit/wait, output sync/copy
- strict verification completes without a mismatch
- Q6_K/other tensors execute on CPU and are not counted as eligible fallback calls
- generated text completes normally

If strict verification fails, save the complete log and stop. Do not benchmark a failing design. If initialization, ownership, timeout, or quarantine messages appear, reprogram the overlay before retrying; do not ignore a quarantined accelerator.

For a performance comparison, use the exact same model, prompt, token count, threads, and CPU affinity. Run a CPU baseline and then FPGA without per-call verification overhead:

```bash
SOC_GEMV=0 taskset -c 0-3 ./llama-cli -m "$MODEL" \
  -p 'Explain GEMV in one sentence.' -n 32 -t 4 2>&1 | tee q4k-cpu.log

SOC_GEMV=1 SOC_GEMV_BACKEND=fpga \
SOC_GEMV_UIO_DEVICE="$SOC_GEMV_UIO_DEVICE" SOC_GEMV_UIO_SYSFS="$SOC_GEMV_UIO_SYSFS" \
SOC_GEMV_VERIFY=0 SOC_GEMV_PROFILE=1 SOC_GEMV_STATS=1 \
taskset -c 0-3 ./llama-cli -m "$MODEL" \
  -p 'Explain GEMV in one sentence.' -n 32 -t 4 2>&1 | tee q4k-fpga.log
```

Optionally run `llama-bench` with the same CPU affinity and model. Report prompt-processing and token-generation results separately; this accelerator targets single-column decode GEMV, so token generation is the relevant comparison.

Archive the board evidence:

```bash
tar -czf q4k-board-logs.tar.gz q4k-fpga-verify.log q4k-cpu.log q4k-fpga.log
```

The Q4_K_M implementation is ready to merge only after all of these are true: host test passes, HLS C/RTL co-simulation passes, Vivado meets setup and hold timing, artifact hashes match, the Q4_K overlay has the expected address/clock, strict FPGA verification passes, and profiling/statistics show real Q4_K offloads.
