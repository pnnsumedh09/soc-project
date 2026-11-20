CXX ?= c++
CXXFLAGS ?= -O2 -g

test_bw: bw-read.cpp bw-read.h tb.cpp
	$(CXX) $(CXXFLAGS) -std=c++11 -Wall -Wextra -Wno-unused-label -DSOC_BW_HOST bw-read.cpp tb.cpp -o $@

test: test_bw
	./test_bw

rtl-test:
	iverilog -g2012 -s tb_read_window -o /tmp/soc_bw_rtl_test read-window.v tb-read-window.v
	vvp /tmp/soc_bw_rtl_test

.PHONY: test rtl-test
