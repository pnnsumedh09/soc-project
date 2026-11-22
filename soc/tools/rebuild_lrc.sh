#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."
stage=${1:?Use software, hls, bd, or bitstream}
export SOC_GEMV_HW_MHZ=100
export SOC_GEMV_HW_BURST=64
export SOC_GEMV_HW_OUTSTANDING=8
export SOC_GEMV_HW_ROW_INTERLEAVE=1
export SOC_GEMV_HW_PART=xczu3eg-sbva484-1-e
export SOC_GEMV_CONTROL_BASE=0xA0000000
unset SOC_GEMV_PS_CONFIG
export SOC_GEMV_BOARD_PART=avnet.com:ultra96v2:part0:1.2
export SOC_GEMV_BOARD_REPO=${SOC_GEMV_BOARD_REPO:-/misc/scratch/np26753/project/XilinxBoardStore/boards/Avnet}
logs=soc/docs/results/lrc_rebuild_100
mkdir -p "$logs"
git rev-parse HEAD | tee "$logs/source_commit.txt"

case "$stage" in
    software)
        toolchain=/home/projects/gerstl/ece382m/ultra96_toolchain.cmake
        test -f "$toolchain"
        /usr/bin/cmake -S . -B build-ultra96-bringup \
            -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
            -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
            -DGGML_NATIVE=OFF -DGGML_SOC_GEMV=ON -DGGML_SOC_GEMV_FPGA=ON \
            -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_SERVER=ON \
            -DLLAMA_OPENSSL=OFF -DLLAMA_BUILD_UI=OFF -DLLAMA_USE_PREBUILT_UI=OFF \
            2>&1 | tee "$logs/configure.log"
        /usr/bin/cmake --build build-ultra96-bringup \
            --target llama-cli llama-server -j 8 2>&1 | tee "$logs/software.log"
        file build-ultra96-bringup/bin/llama-cli build-ultra96-bringup/bin/llama-server
        ;;
    hls)
        if [[ -e soc/hw/hls_work/gemv/f100_b64_o8 ]]; then
            echo "Existing f100 HLS project: move it aside before a clean rebuild" >&2
            exit 1
        fi
        export SOC_GEMV_HW_STAGE=export
        vitis_hls -f soc/hw/gemv/build_hls.tcl 2>&1 | tee "$logs/hls_export.log"
        ;;
    bd|bitstream)
        export SOC_GEMV_BUILD_STAGE="$stage"
        vivado -mode batch -source soc/hw/vivado/gemv/build_vivado.tcl \
            2>&1 | tee "$logs/vivado_${stage}.log"
        ;;
    *) echo "Use software, hls, bd, or bitstream" >&2; exit 2 ;;
esac
