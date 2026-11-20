SYSTEMC ?= /usr/local/packages/systemc-2.3.4
CXX ?= g++
CC ?= gcc

SC_LIBDIR := $(firstword $(wildcard $(SYSTEMC)/lib-linux64 $(SYSTEMC)/lib))
CPPFLAGS += -I. -I../repack -I$(SYSTEMC)/include
CXXFLAGS += -std=c++14 -g -O0 -Wall -Wextra -Werror -Wno-overloaded-virtual -ffp-contract=off
CFLAGS += -std=c11 -g -O0 -Wall -Wextra -Werror -ffp-contract=off
LDFLAGS += -L$(SC_LIBDIR) -Wl,-rpath,$(SC_LIBDIR)
LDLIBS += -lsystemc -lpthread

TARGET := tb_gemv_accel
OBJS := tests/tb_gemv_accel.o gemv_accel.o soc_repack.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

tests/tb_gemv_accel.o: tests/tb_gemv_accel.cpp gemv_accel.h gemv_regs.h ../repack/soc_repack.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

gemv_accel.o: gemv_accel.cpp gemv_accel.h gemv_regs.h ../repack/soc_repack.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

soc_repack.o: ../repack/soc_repack.c ../repack/soc_repack.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all test clean
