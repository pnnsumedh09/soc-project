CC ?= cc
CFLAGS ?= -O2 -g
FLAGS = -std=c11 -Wall -Wextra -Werror -ffp-contract=off
INC = -I../repack -I../gemv
HEADERS = soc_gemv_hal.h soc_gemv_arena.h ../gemv/soc_gemv_backend.h ../repack/soc_repack.h

all: test_hal

test_hal: test_hal.c soc_gemv_hal.c soc_gemv_arena.c soc_gemv_hal.h soc_gemv_arena.h
	$(CC) $(CFLAGS) $(FLAGS) $(INC) test_hal.c soc_gemv_hal.c soc_gemv_arena.c -o $@

board_smoke: board_smoke.c soc_gemv_hal.c soc_gemv_arena.c soc_gemv_backend_fpga.c ../repack/soc_repack.c $(HEADERS)
	$(CC) $(CFLAGS) $(FLAGS) $(INC) -DSOC_GEMV_WITH_FPGA $(filter %.c,$^) -o $@

backend-check:
	$(CC) $(CFLAGS) $(FLAGS) $(INC) -fsyntax-only soc_gemv_backend_fpga.c board_smoke.c -DSOC_GEMV_WITH_FPGA

test_backend_fallback: test_backend_fallback.c soc_gemv_hal.c soc_gemv_arena.c soc_gemv_backend_fpga.c ../gemv/soc_gemv.c ../gemv/soc_gemv_backend_emul.c ../repack/soc_repack.c $(HEADERS)
	$(CC) $(CFLAGS) $(FLAGS) $(INC) -DSOC_GEMV_WITH_FPGA $(filter %.c,$^) -lm -lpthread -o $@

test: test_hal backend-check test_backend_fallback
	./test_hal
	./test_backend_fallback

.PHONY: all test backend-check
