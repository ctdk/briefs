#!/usr/bin/env python3
"""Summarize a perf-vs-other-fs round.

Reads the raw fio JSONs and phase .txt files a round produced under
/xfstests/perf-results/<runid>/ and prints per-workload median + spread
for every filesystem, plus the shell-phase timings (W6/W7/W10).

Usage:  summarize-perf.py <runid-dir>
        (e.g. summarize-perf.py /xfstests/perf-results/run-20261001-010101)

Numbers are VM-relative (virtio disk, host cache=none on the perf disk);
see the plan file for ground rules.
"""

import json
import re
import statistics
import sys
from pathlib import Path


def workload_key(name):
    # "W3b.randread_4k_qd32.1.json" -> ("W3b.randread_4k_qd32", 1)
    m = re.match(r"^(W[^.]+(?:\.[^.]+)*?)\.\d+(?:\.job\d+)?$", name)
    return m.group(1) if m else name


def collect_jobs(path):
    """Return list of (label, side, bw_kibs, iops) from one fio JSON."""
    try:
        d = json.loads(path.read_text())
    except (json.JSONDecodeError, OSError):
        return []
    out = []
    for job in d.get("jobs", []):
        for side in ("read", "write"):
            s = job.get(side)
            if s and (s.get("iops") or s.get("bw")):
                out.append((side, s.get("bw", 0), s.get("iops", 0)))
    return out


def fmt(vals, unit, scale=1.0):
    if not vals:
        return "-"
    med = statistics.median(vals) * scale
    spread = (max(vals) - min(vals)) * scale
    if unit == "iops":
        return f"{med:,.0f} iops  (spread {spread:,.0f})"
    if med >= 1024 * 1024:
        return f"{med/1024/1024:.2f} GB/s  (spread {spread/1024/1024:.2f})"
    return f"{med/1024:.1f} MB/s  (spread {spread/1024:.1f})"


def parse_txt(path):
    """Parse W6/W7/W10 phase lines into {canonical-label: [fields]}."""
    rows = {}
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        label = workload_key(line.split()[0])
        toks = dict(t.split("=", 1) for t in line.split()[1:] if "=" in t)
        if "jobs" in toks:
            label += f" j{toks['jobs']}"
        rows.setdefault(label, []).append(toks)
    return rows


def main(run_dir):
    run_dir = Path(run_dir)
    if not run_dir.is_dir():
        sys.exit(f"not a directory: {run_dir}")

    # {fs}{workload}{side} -> [values]
    data = {}
    for fsdir in sorted(p for p in run_dir.iterdir() if p.is_dir()):
        fs = fsdir.name
        for jf in fsdir.glob("*.json"):
            wl = workload_key(jf.stem)
            for side, bw, iops in collect_jobs(jf):
                data.setdefault((fs, wl, side, "bw"), []).append(bw)
                data.setdefault((fs, wl, side, "iops"), []).append(iops)

    filesystems = sorted({k[0] for k in data})
    workloads = sorted({k[1] for k in data})

    print(f"== {run_dir.name}: fio workloads ==")
    for wl in workloads:
        print(f"\n-- {wl}")
        for fs in filesystems:
            shown = False
            for side in ("read", "write"):
                bws = data.get((fs, wl, side, "bw"), [])
                if bws:
                    iopss = data.get((fs, wl, side, "iops"), [])
                    print(f"  {fs:8s} {side:5s} {fmt(bws, 'bw')}")
                    if iopss and statistics.median(iopss) > 0:
                        print(f"  {'':8s} {'':5s} {fmt(iopss, 'iops')}")
                    shown = True
            if not shown:
                print(f"  {fs:8s} (no data)")

    print(f"\n== {run_dir.name}: shell phases (median of reps) ==")
    for fs in filesystems:
        print(f"\n-- {fs}")
        for txt in sorted((run_dir / fs).glob("*.txt")):
            for label, rows in sorted(parse_txt(txt).items()):
                secs = [float(r["seconds"]) for r in rows if "seconds" in r]
                if not secs:
                    continue
                extra = ""
                fcounts = [int(r["files"]) for r in rows if "files" in r]
                if fcounts:
                    rate = statistics.median(fcounts) / statistics.median(secs)
                    extra = f"  ~{rate:,.0f} files/s"
                print(f"  {label:22s} median {statistics.median(secs):8.3f}s"
                      f"  spread {max(secs)-min(secs):7.3f}s{extra}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])