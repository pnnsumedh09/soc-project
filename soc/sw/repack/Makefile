# soc/sw/repack - Q8_0 repack library and unit test (plan task P2.1)
#
#   make            build test_repack for the host
#   make test       build and run synthetic tests
#   make test-real  also dump real Qwen tensors and test them (needs numpy + the model)
#   make CC=aarch64-linux-gnu-gcc   cross-build for the Ultra96 (run test_repack there)
#
# -ffp-contract=off: forbid fused multiply-add so the reference arithmetic is
# reproducible across compilers/targets (same flag the Lab 3 build used).

CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -ffp-contract=off
LDLIBS  += -lm

MODEL   ?= ../../../../qwen2.5-0.5b-instruct-q8_0.gguf
DUMPDIR ?= /tmp/soc_q8_tensors

all: test_repack

test_repack: test_repack.c soc_repack.c soc_repack.h
	$(CC) $(CFLAGS) -o $@ test_repack.c soc_repack.c $(LDLIBS)

test: test_repack
	./test_repack

test-real: test_repack
	cd ../../.. && PYTHONPATH=gguf-py PYTHONDONTWRITEBYTECODE=1 python3 soc/tools/dump_q8_tensor.py $(abspath $(MODEL)) $(DUMPDIR)
	./test_repack $(DUMPDIR)/*.bin

clean:
	rm -f test_repack

.PHONY: all test test-real clean
