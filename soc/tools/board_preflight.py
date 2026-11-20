#!/usr/bin/env python3
"""Read-only board discovery: no MMIO access, allocations or configuration changes."""
import glob
import json
import os
import platform
from pathlib import Path


def read(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def main():
    data = {"machine": platform.machine(), "system": platform.system(), "kernel": platform.release(),
            "page_size": os.sysconf("SC_PAGE_SIZE"), "uio": [], "dma_buffers": []}
    text = read("/proc/meminfo") or ""
    data["memory"] = {line.split(":", 1)[0]: line.split(":", 1)[1].strip()
                      for line in text.splitlines()
                      if line.split(":", 1)[0] in ("MemTotal", "MemAvailable", "CmaTotal", "CmaFree")}
    for directory in sorted(glob.glob("/sys/class/uio/uio*")):
        p = Path(directory)
        item = {"sysfs": directory, "device": "/dev/" + p.name, "name": read(p / "name"),
                "accessible": os.access("/dev/" + p.name, os.R_OK | os.W_OK), "maps": []}
        for mapping in sorted(p.glob("maps/map*")):
            item["maps"].append({"path": str(mapping), **{k: read(mapping / k) for k in ("name", "addr", "size", "offset")}})
        data["uio"].append(item)
    for cls in ("u-dma-buf", "udmabuf"):
        for directory in sorted(glob.glob("/sys/class/" + cls + "/*")):
            p = Path(directory)
            data["dma_buffers"].append({"sysfs": directory, "device": "/dev/" + p.name,
                "accessible": os.access("/dev/" + p.name, os.R_OK | os.W_OK),
                **{k: read(p / k) for k in ("phys_addr", "size", "sync_mode", "dma_coherent")}})
    print(json.dumps(data, indent=2))


if __name__ == "__main__":
    main()
