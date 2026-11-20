#!/bin/bash
# Manual build of llama-simple (plain and SOC_GEMV variants) WITHOUT cmake.
# For Claude sandbox sessions only (the sandbox has gcc but no cmake; it is aarch64,
# so -mcpu=cortex-a53 builds run natively there). On the LRC use cmake as usual.
# Outputs in /tmp/nb: llama-simple-plain, llama-simple-soc.
set -e
R="$(cd "$(dirname "$0")/../.." && pwd)"   # llama.cpp root
O=/tmp/nb/obj; mkdir -p $O /tmp/nb/gen
printf '#pragma once\n#define LLAMA_VERSION "0"\n#define LLAMA_COMMIT "soc"\n' > /tmp/nb/gen/llama-version.h
printf "#pragma once\n#define GGML_VERSION \"0.0.0\"\n#define GGML_COMMIT \"soc\"\n" > /tmp/nb/gen/ggml-version.h
DEFS="-D_GNU_SOURCE -DGGML_SCHED_MAX_COPIES=4 -DGGML_USE_CPU -DGGML_USE_LLAMAFILE=1"
INC="-I$R/ggml/include -I$R/ggml/src -I$R/ggml/src/ggml-cpu -I$R/include -I$R/src -I/tmp/nb/gen -I$R/soc/sw/repack -I$R/soc/sw/gemv"
CF="-O2 -mcpu=cortex-a53 -pthread -w $DEFS $INC"
srcs=()
for f in ggml.c ggml.cpp ggml-alloc.c ggml-backend.cpp ggml-backend-meta.cpp ggml-opt.cpp ggml-threading.cpp ggml-quants.c gguf.cpp ggml-backend-dl.cpp ggml-backend-reg.cpp; do srcs+=($R/ggml/src/$f); done
for f in $R/ggml/src/ggml-cpu/*.c $R/ggml/src/ggml-cpu/*.cpp; do [ "$(basename $f)" = ggml-cpu.c ] || srcs+=($f); done
srcs+=($R/ggml/src/ggml-cpu/arch/arm/quants.c $R/ggml/src/ggml-cpu/arch/arm/repack.cpp $R/ggml/src/ggml-cpu/llamafile/sgemm.cpp $R/ggml/src/ggml-cpu/tiled/tiled.cpp $R/ggml/src/ggml-cpu/tiled/tiled-kernel.cpp)
for f in $R/src/*.cpp $R/src/models/*.cpp; do srcs+=($f); done
srcs+=($R/examples/simple/simple.cpp)
compile() { f=$1; o=$O/$(echo ${f#$R/} | tr '/' '_').o
  [ -f $o ] && [ $o -nt $f ] && return 0
  case $f in *.c) gcc -std=gnu11 $CF -c $f -o $o;; *) g++ -std=c++17 $CF -c $f -o $o;; esac || { echo "FAILED $f"; return 1; } }
export -f compile; export R O CF
printf '%s\n' "${srcs[@]}" | xargs -P4 -I{} bash -c 'compile {}'
# two variants of ggml-cpu.c + soc sources
gcc -std=gnu11 $CF -c $R/ggml/src/ggml-cpu/ggml-cpu.c -o /tmp/nb/ggml-cpu.plain.o
gcc -std=gnu11 $CF -DGGML_SOC_GEMV -c $R/ggml/src/ggml-cpu/ggml-cpu.c -o /tmp/nb/ggml-cpu.soc.o
for f in repack/soc_repack.c gemv/soc_gemv.c gemv/soc_gemv_backend_emul.c; do gcc -std=gnu11 $CF -ffp-contract=off -c $R/soc/sw/$f -o /tmp/nb/$(basename $f .c).o; done
g++ -pthread -o /tmp/nb/llama-simple-plain $O/*.o /tmp/nb/ggml-cpu.plain.o -lm
g++ -pthread -o /tmp/nb/llama-simple-soc $O/*.o /tmp/nb/ggml-cpu.soc.o /tmp/nb/soc_repack.o /tmp/nb/soc_gemv.o /tmp/nb/soc_gemv_backend_emul.o -lm
echo BUILD OK
