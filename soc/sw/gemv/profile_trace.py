#!/usr/bin/env python3
"""Summarize a SOC_GEMV_PROFILE CSV trace."""

import argparse
import csv
import math
import sys
from collections import defaultdict


FIELDS = (
    "call",
    "tensor_id",
    "rows",
    "n",
    "path",
    "cache_hit",
    "weight_bytes",
    "activation_bytes",
    "output_bytes",
    "macs",
    "elapsed_ns",
    "pl_cycles",
    "hw_read_bytes",
    "hw_write_bytes",
    "hw_macs",
)
INTEGER_FIELDS = tuple(field for field in FIELDS if field != "path")


def nonnegative(row, field, line):
    try:
        value = int(row[field])
    except (KeyError, ValueError) as exc:
        raise ValueError("line {} has invalid {}".format(line, field)) from exc
    if value < 0:
        raise ValueError("line {} has negative {}".format(line, field))
    return value


def read_trace(path):
    records = []
    with open(path, "r", newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        if tuple(reader.fieldnames or ()) != FIELDS:
            raise ValueError("unexpected CSV header")
        for line, row in enumerate(reader, 2):
            record = {field: nonnegative(row, field, line) for field in INTEGER_FIELDS}
            record["path"] = row["path"]
            if record["path"] not in ("backend", "fallback", "invalid"):
                raise ValueError("line {} has invalid path".format(line))
            records.append(record)
    return records


def rate(numerator, elapsed_ns):
    return numerator / elapsed_ns if elapsed_ns else 0.0


def summarize(records, wall_seconds):
    valid = [record for record in records if record["path"] != "invalid"]
    backend = [record for record in valid if record["path"] == "backend"]
    elapsed_ns = sum(record["elapsed_ns"] for record in backend)
    weight_bytes = sum(record["weight_bytes"] for record in backend)
    macs = sum(record["macs"] for record in backend)
    pl_cycles = sum(record["pl_cycles"] for record in backend)
    hw_read_bytes = sum(record["hw_read_bytes"] for record in backend)
    print("calls={} backend={} fallback={} invalid={}".format(
        len(records), len(backend),
        sum(record["path"] == "fallback" for record in records),
        sum(record["path"] == "invalid" for record in records)))
    print("backend_time_s={:.6f} weight_GB={:.6f} work_GMAC={:.6f}".format(
        elapsed_ns / 1e9, weight_bytes / 1e9, macs / 1e9))
    print("effective_weight_GB_s={:.3f} effective_GMAC_s={:.3f}".format(
        rate(weight_bytes, elapsed_ns), rate(macs, elapsed_ns)))
    if pl_cycles:
        print("hardware_pl_cycles={} hardware_read_GB={:.6f}".format(
            pl_cycles, hw_read_bytes / 1e9))
    if wall_seconds is not None:
        if wall_seconds <= 0 or not math.isfinite(wall_seconds):
            raise ValueError("--wall-seconds must be finite and positive")
        share = min(elapsed_ns / 1e9 / wall_seconds, 1.0)
        limit = math.inf if share == 1.0 else 1.0 / (1.0 - share)
        print("backend_wall_share={:.2f}% zero_cost_backend_speedup_limit={:.3f}x".format(
            100.0 * share, limit))

    groups = defaultdict(lambda: [0, 0, 0, 0, 0])
    for record in backend:
        key = (record["tensor_id"], record["rows"], record["n"])
        group = groups[key]
        group[0] += 1
        group[1] += record["elapsed_ns"]
        group[2] += record["weight_bytes"]
        group[3] += record["macs"]
        group[4] += record["pl_cycles"]
    print("\nper_tensor_backend:")
    print("tensor rows n calls total_ms avg_us pl_cycles weight_GB_s GMAC_s")
    ordered = sorted(groups.items(), key=lambda item: item[1][1], reverse=True)
    for (tensor_id, rows, n), (calls, ns, weights, group_macs, cycles) in ordered:
        print("{} {} {} {} {:.3f} {:.3f} {} {:.3f} {:.3f}".format(
            tensor_id, rows, n, calls, ns / 1e6, ns / calls / 1e3,
            cycles, rate(weights, ns), rate(group_macs, ns)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", help="CSV produced by SOC_GEMV_PROFILE")
    parser.add_argument("--wall-seconds", type=float,
                        help="measured end-to-end interval for an Amdahl bound")
    args = parser.parse_args()
    try:
        summarize(read_trace(args.trace), args.wall_seconds)
    except (OSError, ValueError) as exc:
        print("profile_trace.py: {}".format(exc), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
