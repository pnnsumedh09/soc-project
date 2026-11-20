CC ?= cc
CXX ?= c++
CFLAGS ?= -O2 -g
CXXFLAGS ?= -O2 -g
COMMON_FLAGS = -Wall -Wextra -ffp-contract=off
INCLUDES = -I../../sw/repack -I../../lab3_ref/part3
MODEL ?= ../../../../qwen2.5-0.5b-instruct-q8_0.gguf
DUMPDIR ?= /tmp/soc_gemv_tensors

test_gemv_hw: gemv.cpp gemv.h tb.cpp soc_repack_ref.o dot_core.o
	$(CXX) $(CXXFLAGS) $(COMMON_FLAGS) -Wno-unused-label -std=c++11 -DSOC_GEMV_HW_HOST $(INCLUDES) gemv.cpp tb.cpp soc_repack_ref.o dot_core.o -lm -o $@

soc_repack_ref.o: ../../sw/repack/soc_repack.c ../../sw/repack/soc_repack.h
	$(CC) $(CFLAGS) $(COMMON_FLAGS) -std=c11 $(INCLUDES) -c $< -o $@

dot_core.o: ../../lab3_ref/part3/dot_q8_0_core.cpp ../../lab3_ref/part3/dot_q8_0_core.h
	$(CXX) $(CXXFLAGS) $(COMMON_FLAGS) -std=c++11 $(INCLUDES) -c $< -o $@

test: test_gemv_hw
	./test_gemv_hw

script-test:
	tclsh test_hls_script.tcl

test-real: test_gemv_hw
	cd ../../.. && PYTHONPATH=gguf-py PYTHONDONTWRITEBYTECODE=1 python3 soc/tools/dump_q8_tensor.py $(abspath $(MODEL)) $(DUMPDIR) --rows 64
	./test_gemv_hw $(DUMPDIR)/*.bin

.PHONY: test script-test test-real
