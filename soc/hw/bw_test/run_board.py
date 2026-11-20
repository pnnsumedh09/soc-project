#!/usr/bin/env python3
"""PYNQ runner for the P1.1 overlay; writes measured P1.2 samples as CSV."""

import argparse
import csv
import sys
import time

MAX_BYTES = 256 * 1024 * 1024
REG_CONTROL = 0x00
REG_MEM = 0x10
REG_NBYTES = 0x20
REG_CHECKSUM = 0x28
REG_STATUS = 0x30


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bitstream")
    parser.add_argument("--clock-mhz", type=float, required=True)
    parser.add_argument("--sizes", type=int, nargs="+", default=[16384, 1048576, 16777216])
    parser.add_argument("--repeats", type=int, default=10)
    parser.add_argument("--empty-repeats", type=int, default=1000)
    parser.add_argument("--timeout", type=float, default=10)
    args = parser.parse_args()
    if (any(n <= 0 or n % 16 or n > MAX_BYTES for n in args.sizes)
            or args.repeats < 1 or args.empty_repeats < 1
            or args.timeout <= 0 or not 0 < args.clock_mhz <= 200):
        parser.error("Use positive 16-byte-multiple sizes <= 256 MiB and positive repeat/timeout values")

    import numpy as np
    from pynq import Clocks, Overlay, allocate

    overlay = Overlay(args.bitstream)
    mhz = float(Clocks.fclk0_mhz)
    if abs(mhz - args.clock_mhz) > 0.01 * args.clock_mhz:
        raise RuntimeError(f"PL0 clock {mhz} MHz differs from requested {args.clock_mhz} MHz")
    kernel = overlay.bw_read_0.mmio
    clear = overlay.clear_0.mmio
    cycles = overlay.cycles_0.mmio
    buffer = allocate(shape=(max(args.sizes) // 4,), dtype=np.uint32)
    in_flight = False
    try:
        address = int(buffer.device_address)
        if address % 4096 or address + buffer.nbytes > 0x80000000:
            raise RuntimeError("Expected a 4 KiB-aligned DMA buffer inside the board's low 2 GiB DDR")
        buffer[:] = np.arange(buffer.size, dtype=np.uint32) ^ np.uint32(0xa5c37e19)
        buffer.flush()
        expected = {n: int(np.sum(buffer[:n // 4], dtype=np.uint64)) & 0xffffffff for n in args.sizes}
        expected[0] = 0
        writer = csv.writer(sys.stdout)
        writer.writerow(["bytes", "repeat", "clock_mhz", "read_cycles", "read_seconds",
                         "read_GB_s", "job_ns", "checksum", "device_address"])

        for nbytes in [0] + args.sizes:
            repeats = args.empty_repeats if nbytes == 0 else args.repeats
            for repeat in range(-1, repeats):
                # AP_DONE is clear-on-read; confirm the previous job is idle.
                if not kernel.read(REG_CONTROL) & 4:
                    raise RuntimeError("Kernel is not idle")
                clear.write(0, 1)
                clear.read(0)
                clear.write(0, 0)
                clear.read(0)
                begin = time.monotonic_ns()
                kernel.write(REG_MEM, address & 0xffffffff)
                kernel.write(REG_MEM + 4, address >> 32)
                kernel.write(REG_NBYTES, nbytes)
                in_flight = True
                kernel.write(REG_CONTROL, 1)
                deadline = begin + int(args.timeout * 1e9)
                while not kernel.read(REG_CONTROL) & 2:
                    if time.monotonic_ns() > deadline:
                        raise TimeoutError("Read test timed out")
                job_ns = time.monotonic_ns() - begin
                in_flight = False
                checksum = kernel.read(REG_CHECKSUM)
                if kernel.read(REG_STATUS) != 0 or checksum != expected[nbytes]:
                    raise RuntimeError(f"Transfer failed: bytes={nbytes} checksum={checksum:08x}")
                span = cycles.read(0) | (cycles.read(8) << 32)
                if (nbytes == 0 and span != 0) or (nbytes > 0 and span < nbytes // 16):
                    raise RuntimeError(f"Invalid read window: {span} cycles for {nbytes} bytes")
                seconds = span / (mhz * 1e6)
                if repeat >= 0:
                    writer.writerow([nbytes, repeat, mhz, span, seconds,
                                     nbytes / seconds / 1e9 if seconds else 0,
                                     job_ns, f"{checksum:08x}", hex(address)])
                    sys.stdout.flush()
    finally:
        # An interrupted or timed-out job must stop before its DMA buffer is freed.
        if in_flight:
            overlay.download()
        buffer.freebuffer()


if __name__ == "__main__":
    main()
