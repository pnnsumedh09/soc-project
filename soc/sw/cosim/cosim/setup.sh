#!/bin/sh
set -eu

DEMO=${1:-}
if [ -z "$DEMO" ] || [ ! -f "$DEMO/zynqmp_demo.cc" ]; then
    echo "usage: $0 <systemctlm-cosim-demo>" >&2
    exit 1
fi
if [ ! -f "$DEMO/libsystemctlm-soc/soc/xilinx/zynqmp/xilinx-zynqmp.h" ]; then
    echo "missing libsystemctlm-soc; initialize the course checkout submodule" >&2
    exit 1
fi

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
COSIM=$(dirname "$HERE")
REPACK=$(CDPATH= cd -- "$COSIM/../repack" && pwd)

for file in gemv_accel.h gemv_accel.cpp gemv_regs.h; do
    cp "$COSIM/$file" "$DEMO/$file"
done
cp "$REPACK/soc_repack.h" "$REPACK/soc_repack.c" "$DEMO/"
cp "$HERE/zynqmp_gemv.cc" "$HERE/Makefile.gemv" "$DEMO/"

echo "Installed zynqmp_gemv sources into $DEMO"
echo "Build with: make -C $DEMO -f Makefile.gemv SYSTEMC=/usr/local/packages/systemc-2.3.4 zynqmp_gemv"
