# Combination A r8, double buffering, and counters: end-to-end workflow

Date: 2026-11-22

Branch: `combination-a-feasible`

This is the authoritative workflow for the row-interleave-8 and counter-instrumented design. It uses `TEAMMATE_FPGA_BUILD_AND_BOARD_RUN.md` for the proven Xilinx 2022.2, Ultra96-V2, artifact, and board sequencing. The software in this branch uses UIO plus `ikwzm/u-dma-buf`; it does not use the XRT flags shown for the teammate's other runtime.

## 1. What changed

- Row interleave 8 with two 16-block ping-pong weight/scale tile banks is the default HLS build. The 16-block bank size makes a common 896-element/28-block Qwen input exercise two tiles. `SOC_GEMV_HW_DOUBLE_BUFFER=0` retains r8 without requested load/compute overlap; the sequential implementation remains selectable with row interleave 1 and double buffering 0.
- Successful HLS jobs publish logical read bytes, write bytes, and MACs at AXI-Lite offsets `0x70`, `0x80`, and `0x90`.
- Vivado adds a 32-bit AXI Timer at control-base offset `0x1000`, clocked from the same PL clock as GEMV.
- The HAL starts the timer immediately before GEMV, stops it after DONE and IDLE are observed, checks all HLS counter-valid bits, and rejects counter/shape mismatches.
- llama.cpp aggregate statistics and CSV traces include PL cycles and HLS logical counters.

`pl_cycles` measures the timer window from software timer-start through observed GEMV completion. It contains fixed AXI-Lite submission/observation overhead and is not a pure compute-loop counter. HLS byte counters are logical payload, not physical AXI beats. Use an AXI Performance Monitor later if burst efficiency, stalls, or physical DDR traffic is required.

## Profiling hook map and when results appear

| Stage | Hook | Result and location |
|---|---|---|
| HLS kernel | `soc/hw/gemv/gemv.cpp` | Logical read bytes, write bytes and MACs are written only after a successful job. Vitis schedule, latency, resource and DATAFLOW reports appear after the HLS command in Section 6. |
| Vivado design | `soc/hw/vivado/gemv/build_vivado.tcl` | AXI Timer at `0xA0001000`; routed timing, clock and utilization reports appear after Section 8. |
| HAL | `soc/sw/hal2/soc_gemv_hal.c` | `soc_gemv_hal_run_profiled` brackets a submission with the timer, reads HLS counter-valid/data registers and rejects inconsistent values. |
| FPGA backend | `soc/sw/hal2/soc_gemv_backend_fpga.c` | `last_counters` transfers the last hardware measurements to the llama hook. |
| Board smoke | `soc/sw/hal2/board_smoke.c` | Section 13 prints one validated job's `pl_cycles`, logical bytes and MACs. This is the first physical-hardware counter result. |
| llama integration | `soc/sw/gemv/soc_gemv.c` | `SOC_GEMV_STATS=1` prints aggregate counters at process exit; `SOC_GEMV_PROFILE=FILE.csv` writes one line per GEMV. |
| Experiment runner | `soc/sw/gemv/run_profile_experiment.sh` | Section 15 creates CPU/emulation/FPGA logs and `fpga_trace.csv`. |
| Analyzer | `soc/sw/gemv/profile_trace.py` | Summarizes time, PL cycles, logical GB/s and GMAC/s by tensor shape. |

Profiling does not require a separate bitstream after this counter-instrumented bitstream is built. It does add software CSV/logging overhead, so use traced runs to explain behavior and untraced repeated runs for final tokens/s comparisons.

## 2. Windows PowerShell: review, commit, and push

Run from:

```powershell
cd 'C:\Niru-personal\Sem3\SoC Design\Project\soc-project'
```

Confirm the branch and inspect every change:

```powershell
git branch --show-current
git status --short
git diff --check
git diff --stat
git diff -- soc/hw/gemv soc/hw/vivado/gemv soc/sw/hal2 soc/sw/gemv soc/docs
```

Expected branch:

```text
combination-a-feasible
```

Run the checks available on Windows:

```powershell
C:\msys64\mingw64\bin\python3.exe soc/hw/vivado/gemv/test_scripts.py
C:\msys64\mingw64\bin\python3.exe -m py_compile soc/sw/gemv/profile_trace.py
```

The Vivado script test must end with:

```text
GEMV Tcl checks: PASS (mocked; Vitis/Vivado not executed)
```

Stage only the files you reviewed. Do not use `git add .` if unrelated files are present:

```powershell
git add --sparse soc/hw/gemv
git add --sparse soc/hw/vivado/gemv
git add --sparse soc/sw/hal2
git add --sparse soc/sw/gemv
git add --sparse soc/docs
git diff --cached --check
git diff --cached --stat
git diff --cached
```

Write your own commit message after reviewing the implementation:

```powershell
git commit
git push -u origin combination-a-feasible
```

If GitHub rejects the push, the repository owner must add your GitHub account under repository `Settings -> Collaborators and teams`, or you must push the branch to your own fork:

```powershell
git remote -v
git remote add myfork https://github.com/YOUR_ACCOUNT/soc-project.git
git push -u myfork combination-a-feasible
```

Do not continue on LRC until the pushed commit is visible:

```powershell
git rev-parse HEAD
git ls-remote origin refs/heads/combination-a-feasible
```

## 3. LRC: synchronize the exact commit

Run on LRC, in Bash rather than tcsh:

```bash
ssh nkg557@mario.ece.utexas.edu
bash
set -euo pipefail
cd /misc/scratch/nkg557/SoC/Project/GEMV_QEMU/soc-project
git status --short
```

Require a clean checkout. Preserve any real work with `git stash push -u` before switching; do not delete it.

```bash
git fetch origin
git switch combination-a-feasible
git pull --ff-only origin combination-a-feasible
git status --short
git rev-parse HEAD
git rev-parse origin/combination-a-feasible
```

The two hashes must match and status must be empty. If you pushed to a fork, fetch that remote instead.

Create an external result directory so logs are not mixed with source:

```bash
export REPO=$PWD
export RUN=/misc/scratch/nkg557/SoC/Project/results/combination-a-r8-$(date +%Y%m%d-%H%M%S)
mkdir -p "$RUN"/{logs,hls,vivado,deploy,board-results}
git rev-parse HEAD | tee "$RUN/source_commit.txt"
git status --short | tee "$RUN/source_status.txt"
```

## 4. LRC host tests

These run on x86-64 and do not access the FPGA:

```bash
make -C soc/sw/hal2 test 2>&1 | tee "$RUN/logs/hal-test.log"
make -C soc/hw/gemv test 2>&1 | tee "$RUN/logs/gemv-native-r8.log"
make -C soc/hw/gemv script-test 2>&1 | tee "$RUN/logs/hls-script-test.log"
python3 soc/hw/vivado/gemv/test_scripts.py 2>&1 | tee "$RUN/logs/vivado-script-test.log"
python3 soc/sw/gemv/test_profile_trace.py 2>&1 | tee "$RUN/logs/profile-trace-test.log"
```

Require the HAL PASS, `GEMV HW: PASS`, HLS script PASS, and mocked Vivado PASS. The expected x86 FPGA-backend refusal in `make test` is a successful negative test.

Build and run the llama hook/emulation test:

```bash
gcc -O2 -std=c11 -Wall -Wextra -Werror -ffp-contract=off \
    -Isoc/sw/repack -Isoc/sw/gemv \
    -o /tmp/test_gemv \
    soc/sw/gemv/test_gemv.c soc/sw/gemv/soc_gemv.c \
    soc/sw/gemv/soc_gemv_backend_emul.c soc/sw/repack/soc_repack.c \
    -lm -lpthread
/tmp/test_gemv 2>&1 | tee "$RUN/logs/gemv-emulation.log"
```

Require `ALL TESTS PASSED`.

## 5. Load Xilinx 2022.2 and register Ultra96-V2

```bash
module load xilinx/2022
module load gcc/14
command -v vitis_hls
command -v vivado
vitis_hls -version | tee "$RUN/logs/vitis-version.txt"
vivado -version | tee "$RUN/logs/vivado-version.txt"
```

Use the release-matched Xilinx Board Store:

```bash
export BOARD_STORE=/misc/scratch/nkg557/SoC/Project/XilinxBoardStore
if ! test -d "$BOARD_STORE/.git"; then
    git clone --branch 2022.2 --depth 1 \
        https://github.com/Xilinx/XilinxBoardStore.git "$BOARD_STORE"
fi
export SOC_GEMV_BOARD_REPO="$BOARD_STORE/boards/Avnet"
export SOC_GEMV_BOARD_PART=avnet.com:ultra96v2:part0:1.2
test -f "$SOC_GEMV_BOARD_REPO/ultra96v2/1.2/board.xml"
```

## 6. Vitis HLS: C simulation, synthesis, RTL co-simulation, export

Use one fixed configuration:

```bash
export SOC_GEMV_HW_MHZ=100
export SOC_GEMV_HW_BURST=64
export SOC_GEMV_HW_OUTSTANDING=8
export SOC_GEMV_HW_ROW_INTERLEAVE=8
export SOC_GEMV_HW_DOUBLE_BUFFER=1
export SOC_GEMV_HW_PART=xczu3eg-sbva484-1-e
export SOC_GEMV_HW_STAGE=export
```

Preserve an old generated candidate instead of mixing it with this run:

```bash
if test -e soc/hw/hls_work/gemv/f100_b64_o8_r8_db; then
    mv soc/hw/hls_work/gemv/f100_b64_o8_r8_db \
       "$RUN/hls/f100_b64_o8_r8_db.before-build"
fi
vitis_hls -f soc/hw/gemv/build_hls.tcl \
    2>&1 | tee "$RUN/logs/hls-export-r8.log"
```

Require:

```text
GEMV HW: PASS
C/RTL co-simulation finished: PASS
P3.1 GEMV HLS stage export completed
```

Verify the new ABI from the generated header:

```bash
export HLS_DIR=soc/hw/hls_work/gemv/f100_b64_o8_r8_db/solution
test -s "$HLS_DIR/impl/ip/component.xml"
grep -E 'STATUS|READ_BYTES|WRITE_BYTES|MACS' \
    "$HLS_DIR"/impl/ip/drivers/*/src/xsoc_gemv_hw_hw.h \
    | tee "$RUN/logs/generated-registers.txt"
```

Required offsets are status `0x60/0x64`, read bytes `0x70/0x78`, write bytes `0x80/0x88`, and MACs `0x90/0x98`. Stop if `check_export.tcl` or the generated header disagrees.

Copy reports:

```bash
cp -a "$HLS_DIR/syn/report" "$RUN/hls/syn-report"
cp -a "$HLS_DIR/sim/report" "$RUN/hls/sim-report"
grep -R -E 'interleave_words|Initiation Interval|Utilization|Estimated' \
    "$HLS_DIR/syn/report" > "$RUN/logs/hls-summary-search.txt" || true
```

Confirm `interleave_words` achieved II=1. Inspect the DATAFLOW report to verify that next-tile loading overlaps current-tile compute; two source arrays alone are not proof. Compare total RTL transaction latency and resources with both saved r1 and r8-only results.

Vivado preflight must inspect the complete `solution/syn/report/*_csynth.rpt` set. Vitis can place `interleave_words` and the helper names in child-function reports instead of `soc_gemv_hw_csynth.rpt`; checking only the top report causes a false scalar-export rejection. If no `interleave_words` or `compute_r8_tile` marker exists anywhere in the report set, treat it as a genuinely wrong export and rebuild HLS.

The exact ABI-checker, report-discovery, Dataflow Viewer, r8-only A/B build, comparison commands and PASS/FAIL decision table are in the workspace-level `COMBINATION_A_R8_DOUBLE_BUFFER_PROFILING_WORKFLOW.md`, Section 7.1. At minimum, run `soc_gemv_check_registers`, require the Verilog co-simulation row to pass, and confirm overlapping `load_r8_tile`/`compute_r8_tile` activity before starting Vivado.

## 7. Vivado block design with AXI Timer

Use the same environment and select the board preset:

```bash
unset SOC_GEMV_PS_CONFIG
export SOC_GEMV_BUILD_STAGE=check
vivado -mode batch -source soc/hw/vivado/gemv/build_vivado.tcl \
    2>&1 | tee "$RUN/logs/vivado-check.log"
```

Then generate the block design:

```bash
export SOC_GEMV_BUILD_STAGE=bd
vivado -mode batch -source soc/hw/vivado/gemv/build_vivado.tcl \
    2>&1 | tee "$RUN/logs/vivado-bd.log"
```

Require:

- HLS tag `f100_b64_o8_r8_db`;
- generated register ABI matches the HAL;
- reset checks pass;
- GEMV control is at `0xA0000000/4K`;
- AXI Timer is at `0xA0001000/4K`;
- all four memory masters map to low DDR through HP0.

Inspect the generated map:

```bash
export VIVADO_DIR=soc/hw/vivado/gemv_f100_b64_o8_r8_db
cat "$VIVADO_DIR/address_map.csv" | tee "$RUN/logs/address-map.txt"
grep -E 'ERROR|CRITICAL WARNING' "$RUN/logs/vivado-bd.log" || true
```

Any unexplained error or critical warning is a stop condition.

## 8. Vivado implementation and artifacts

```bash
export SOC_GEMV_BUILD_STAGE=bitstream
vivado -mode batch -source soc/hw/vivado/gemv/build_vivado.tcl \
    2>&1 | tee "$RUN/logs/vivado-bitstream.log"
```

Require nonnegative setup and hold slack and a 10 ns PL0 period:

```bash
grep -E 'WNS|TNS|WHS|THS|Timing constraints are met' "$VIVADO_DIR/timing_summary.rpt"
cat "$VIVADO_DIR/clocks.rpt"
cp "$VIVADO_DIR"/{timing_summary.rpt,utilization.rpt,clocks.rpt,address_map.csv,soc_gemv_bd.tcl} "$RUN/vivado/"
```

Expected deployment files:

```bash
ls -lh soc/hw/out/gemv_f100_b64_o8_r8_db.bit \
       soc/hw/out/gemv_f100_b64_o8_r8_db.hwh \
       soc/hw/out/gemv_f100_b64_o8_r8_db.xsa
cp soc/hw/out/gemv_f100_b64_o8_r8_db.{bit,hwh,xsa} "$RUN/deploy/"
sha256sum "$RUN/deploy"/* | tee "$RUN/vivado/hardware-artifact-hashes.txt"
```

Never combine a `.bit` and `.hwh` from different runs.

## 9. Cross-build matching llama.cpp and board utilities

This branch uses UIO plus u-dma-buf, so do not add XRT flags:

```bash
export TOOLCHAIN=/home/projects/gerstl/ece382m/ultra96_toolchain.cmake
/usr/bin/cmake -S "$REPO" -B "$RUN/software" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DGGML_NATIVE=OFF \
    -DGGML_LLAMAFILE=OFF \
    -DGGML_CPU_REPACK=OFF \
    -DGGML_SOC_GEMV=ON \
    -DGGML_SOC_GEMV_FPGA=ON \
    -DLLAMA_BUILD_TESTS=OFF \
    -DGGML_BUILD_TESTS=OFF \
    -DLLAMA_BUILD_SERVER=ON \
    -DLLAMA_OPENSSL=OFF \
    -DLLAMA_BUILD_UI=OFF \
    -DLLAMA_USE_PREBUILT_UI=OFF \
    2>&1 | tee "$RUN/logs/software-configure.log"
/usr/bin/cmake --build "$RUN/software" \
    --target llama-cli llama-server llama-bench -j8 \
    2>&1 | tee "$RUN/logs/software-build.log"
```

Cross-build utilities with the compiler selected by the course toolchain. If it is `aarch64-linux-gnu-gcc`:

```bash
make -C soc/sw/hal2 CC=aarch64-linux-gnu-gcc board_registers board_smoke
file "$RUN/software/bin/llama-cli" "$RUN/software/bin/llama-bench" \
     soc/sw/hal2/board_registers soc/sw/hal2/board_smoke
```

All four must report AArch64. Copy them:

```bash
cp "$RUN/software/bin"/{llama-cli,llama-server,llama-bench} "$RUN/deploy/"
cp soc/sw/hal2/{board_registers,board_smoke} "$RUN/deploy/"
cp soc/sw/gemv/{profile_trace.py,run_profile_experiment.sh} "$RUN/deploy/"
(
    cd "$RUN/deploy"
    sha256sum gemv_f100_b64_o8_r8_db.bit gemv_f100_b64_o8_r8_db.hwh \
        gemv_f100_b64_o8_r8_db.xsa llama-cli llama-server llama-bench \
        board_registers board_smoke profile_trace.py run_profile_experiment.sh \
        > SHA256SUMS
)
tar -C "$RUN/deploy" -czf "$RUN/combination-a-r8-board.tar.gz" .
sha256sum "$RUN/combination-a-r8-board.tar.gz"
```

## 10. Transfer to the Ultra96

If the board is reachable by SSH from LRC:

```bash
export BOARD_SSH=root@REPLACE_WITH_BOARD_HOST
ssh "$BOARD_SSH" 'mkdir -p /root/combination-a-r8'
scp "$RUN/combination-a-r8-board.tar.gz" \
    "$BOARD_SSH":/root/combination-a-r8/
```

If direct SSH is unavailable, use the Mac HTTP/cloudflared transfer from `TEAMMATE_FPGA_BUILD_AND_BOARD_RUN.md`, but transfer this exact tarball and retain its SHA256. Do not reuse an expired tunnel URL.

## 11. Ultra96: unpack and load the overlay

Run on the board in the PYNQ environment:

```bash
cd /root/combination-a-r8
tar -xzf combination-a-r8-board.tar.gz
sha256sum -c SHA256SUMS
chmod +x llama-cli llama-server llama-bench board_registers board_smoke
uname -m
python3 -c 'import pynq; print(pynq.__version__)'
```

Require `aarch64`. Ensure no prior accelerator user is running:

```bash
pgrep -af 'llama-(cli|server|bench)' || true
```

Load the paired overlay:

```bash
python3 - <<'PY'
from pynq import Overlay, Clocks
ol = Overlay('/root/combination-a-r8/gemv_f100_b64_o8_r8_db.bit', download=True)
if not ol.is_loaded():
    raise SystemExit('overlay load failed')
print('PL0 MHz:', Clocks.fclk0_mhz)
print(ol.ip_dict)
if abs(float(Clocks.fclk0_mhz) - 100.0) > 0.1:
    raise SystemExit('unexpected PL0 clock')
PY
```

The HWH must expose GEMV at `0xA0000000` and the timer at `0xA0001000`.

## 12. Select UIO and u-dma-buf

Find the UIO map by address, never by assumed index:

```bash
export SOC_GEMV_UIO_DEVICE=
export SOC_GEMV_UIO_SYSFS=
for d in /sys/class/uio/uio*; do
    test -f "$d/maps/map0/addr" || continue
    addr=$(cat "$d/maps/map0/addr")
    if test "$((addr))" -eq "$((0xA0000000))"; then
        export SOC_GEMV_UIO_DEVICE="/dev/${d##*/}"
        export SOC_GEMV_UIO_SYSFS="$d/maps/map0"
    fi
done
test -n "$SOC_GEMV_UIO_DEVICE"
cat "$SOC_GEMV_UIO_SYSFS/addr" "$SOC_GEMV_UIO_SYSFS/size" "$SOC_GEMV_UIO_SYSFS/offset"
```

The map must start at `0xA0000000`, cover at least `0x10000`, and have zero offset.

Select an `ikwzm/u-dma-buf` device:

```bash
ls -l /dev/udmabuf* /sys/class/u-dma-buf/udmabuf* 2>/dev/null
export SOC_GEMV_DMA_DEVICE=/dev/udmabuf0
export SOC_GEMV_DMA_SYSFS=/sys/class/u-dma-buf/udmabuf0
grep -H . "$SOC_GEMV_DMA_SYSFS"/{phys_addr,size,sync_mode,dma_coherent}
```

This backend requires `sync_mode` 2 or 6, `dma_coherent=0`, a 4-KiB-aligned pool entirely below `0x80000000`, and enough contiguous capacity for cached weights. If this device does not exist, stop. The current code is not an XRT allocator and must not be run against a guessed device.

Export runtime settings:

```bash
export SOC_GEMV_EXPECTED_BASE=0xA0000000
export SOC_GEMV_TIMEOUT_MS=10000
export SOC_GEMV_LOCK_FILE=/tmp/soc-gemv-a0000000.lock
```

## 13. Board register and DMA smoke tests

Determine the selected UIO number:

```bash
UIO_NUMBER=${SOC_GEMV_UIO_DEVICE##*/uio}
./board_registers "$UIO_NUMBER" 0xa0000000
```

Require:

```text
GEMV BOARD REGISTERS: PASS
```

Then run DMA, r8 arithmetic, HLS-counter-valid, and AXI Timer validation:

```bash
./board_smoke 2>&1 | tee board-smoke.log
```

Require:

```text
FPGA board GEMV smoke: PASS
```

The HAL now treats a missing counter-valid bit, wrong logical counter, or timer overflow as a hard protocol failure.

## 14. Short llama.cpp correctness run

Use the model already validated by the teammate:

```bash
export MODEL=/lab1/qwen2.5-0.5b-instruct-q8_0.gguf
test -f "$MODEL"
SOC_GEMV=1 SOC_GEMV_BACKEND=fpga \
SOC_GEMV_VERIFY=1 SOC_GEMV_STATS=1 SOC_GEMV_STRICT=1 \
taskset -c 0 ./llama-cli -m "$MODEL" -t 1 -tb 1 -c 512 \
    --single-turn -p 'What is a system-on-chip?' -n 4 --temp 0 --seed 1 \
    > llama-verify.log 2>&1
echo $? | tee llama-verify.exit
```

Require exit 0, `backend=fpga`, nonzero offloaded GEMVs, zero fallback, acceptable verification error, nonzero PL cycles, and HLS byte/MAC totals matching the software logical totals.

## 15. CPU versus r8 performance

Use identical prompt, token, thread, affinity, model, and clock settings. First warm up, then retain at least three runs.

CPU:

```bash
SOC_GEMV=0 SOC_GEMV_VERIFY=0 SOC_GEMV_STATS=0 \
taskset -c 0 ./llama-cli -m "$MODEL" -t 1 -tb 1 -c 512 \
    --single-turn -p 'What is a system-on-chip?' -n 16 --temp 0 --seed 1 \
    > llama-cpu.log 2>&1
```

FPGA r8:

```bash
SOC_GEMV=1 SOC_GEMV_BACKEND=fpga \
SOC_GEMV_VERIFY=0 SOC_GEMV_STATS=1 SOC_GEMV_STRICT=1 \
taskset -c 0 ./llama-cli -m "$MODEL" -t 1 -tb 1 -c 512 \
    --single-turn -p 'What is a system-on-chip?' -n 16 --temp 0 --seed 1 \
    > llama-r8.log 2>&1
```

Benchmark matrix:

```bash
OUT=/root/combination-a-r8/profile-$(date +%Y%m%d-%H%M%S)
SOC_GEMV_RUN_FPGA=1 bash ./run_profile_experiment.sh \
    ./llama-bench "$MODEL" "$OUT" -p 32 -n 32 -r 5 -t 1
```

Analyze the trace after copying it back to LRC:

```bash
python3 soc/sw/gemv/profile_trace.py fpga_trace.csv
```

Use untraced runs for speed claims. Trace runs answer which tensor shapes dominate, their host duration, PL cycles, logical bytes, and MACs.

## 16. Preserve board evidence

```bash
uname -a > board-environment.txt
cat /proc/cmdline >> board-environment.txt
grep -E 'MemTotal|MemAvailable|CmaTotal|CmaFree' /proc/meminfo >> board-environment.txt
sha256sum gemv_f100_b64_o8_r8_db.bit gemv_f100_b64_o8_r8_db.hwh \
    llama-cli llama-bench > board-artifact-hashes.txt
tar -czf combination-a-r8-results.tar.gz \
    board-environment.txt board-artifact-hashes.txt board-smoke.log \
    llama-verify.log llama-verify.exit llama-cpu.log llama-r8.log profile-* 2>/dev/null || true
```

Transfer this archive back using SCP or the teammate's board-to-Mac tunnel procedure.

## 17. Decision after the run

Compare r8 with the teammate's r1 baseline using matching shapes and clocks:

- RTL transaction cycles and board `pl_cycles`;
- routed LUT/FF/BRAM/DSP and setup/hold slack;
- host backend time and tokens/s;
- zero fallbacks and verification error;
- logical payload equality.

Proceed to ping-pong weight tiles only if r8 phase/AXI evidence shows load time can overlap compute and HP0 has bandwidth headroom. Proceed to true PL descriptor chaining only if small-job command overhead is material. Do not infer physical DDR utilization from logical byte counters.
