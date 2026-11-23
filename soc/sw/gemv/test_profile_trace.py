#!/usr/bin/env python3

import contextlib
import io
import tempfile
from pathlib import Path

import profile_trace


def main():
    header = ",".join(profile_trace.FIELDS)
    rows = [
        "1,7,9,1056,backend,1,10098,1122,36,9504,5000,321,11220,36,9504",
        "2,0,9,1056,fallback,0,10098,1122,36,9504,7000,0,0,0,0",
    ]
    with tempfile.TemporaryDirectory(prefix="soc_gemv_profile_") as temp:
        path = Path(temp) / "trace.csv"
        path.write_text(header + "\n" + "\n".join(rows) + "\n", encoding="utf-8")
        records = profile_trace.read_trace(path)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            profile_trace.summarize(records, 0.001)
    text = output.getvalue()
    assert "backend=1 fallback=1" in text
    assert "hardware_pl_cycles=321" in text
    assert "7 9 1056 1" in text
    print("GEMV profile trace parser: PASS")


if __name__ == "__main__":
    main()
