#!/usr/bin/env python3
"""Build, run, and validate the eusolver against reference counts.

Also measures the search-space size (partials processed) so a propagation /
pruning change can be compared against a saved baseline.

Usage:
    python3 scripts/compare.py [--k N] [--workers W] [--save BASELINE.json]
                               [--compare BASELINE.json] [--no-build]

Examples:
    python3 scripts/compare.py --k 6                    # validate k=1..6
    python3 scripts/compare.py --k 6 --save base.json   # record a baseline
    python3 scripts/compare.py --k 6 --compare base.json  # compare vs baseline

Exit code 0 iff every k's unique-tiling count matches the reference table.
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BINARY = ROOT / "eusolver"

# Ground-truth unique tiling counts (k=8 omitted from --k checks by default
# because it is slow; the table is authoritative regardless).
REFERENCE = {1: 10, 2: 20, 3: 61, 4: 151, 5: 332, 6: 673, 7: 1472, 8: 2849}

# The catalogue omits (4,8,8); the program itself notes k=1 should be 10.
PARTIALS_RE = re.compile(r"\((\d+) partials,")
COUNT_RE = re.compile(r"k=(\d+) -> (\d+) unique tilings?")


def build() -> None:
    print(f"[build] make in {ROOT}")
    subprocess.run(["make"], cwd=ROOT, check=True)


def run_solver(k: int, workers: int) -> dict:
    outdir = tempfile.mkdtemp(prefix=f"eu_k{k}_")
    try:
        t0 = time.monotonic()
        proc = subprocess.run(
            [str(BINARY), "--max-polygons", str(k),
             "--workers", str(workers), "--output", outdir],
            cwd=ROOT, capture_output=True, text=True,
        )
        wall = time.monotonic() - t0
        output = proc.stdout + "\n" + proc.stderr

        m = PARTIALS_RE.search(output)
        partials = int(m.group(1)) if m else None

        counts = {int(a): int(b) for a, b in COUNT_RE.findall(output)}
        return {"k": k, "partials": partials, "counts": counts,
                "wall_s": round(wall, 2), "rc": proc.returncode}
    finally:
        shutil.rmtree(outdir, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--k", type=int, default=5, help="max k to run (default 5)")
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--save", metavar="JSON", help="save results as a baseline")
    ap.add_argument("--compare", metavar="JSON", help="compare against a baseline")
    ap.add_argument("--no-build", action="store_true")
    args = ap.parse_args()

    if not args.no_build:
        build()

    baseline = None
    if args.compare:
        baseline = json.loads(Path(args.compare).read_text())
        print(f"[baseline] loaded {args.compare}")

    results = {}
    ok = True
    print(f"{'k':>2}  {'partials':>10}  {'unique':>7}  {'ref':>4}  {'match':>5}  "
          f"{'time':>6}  {'P vs base':>11}")
    for k in range(1, args.k + 1):
        r = run_solver(k, args.workers)
        results[k] = r
        ref = REFERENCE.get(k)
        got = r["counts"].get(k)
        match = got == ref
        if not match:
            ok = False
        delta = ""
        if baseline and k in baseline and r["partials"] is not None:
            bp = baseline[k].get("partials")
            if bp:
                delta = f"{r['partials'] / bp:>8.3f}x"
        p = r["partials"] if r["partials"] is not None else -1
        print(f"{k:>2}  {p:>10}  {got:>7}  {ref:>4}  "
              f"{('OK' if match else 'FAIL'):>5}  {r['wall_s']:>5.1f}s  {delta}")

    if args.save:
        Path(args.save).write_text(json.dumps(results, indent=2))
        print(f"[baseline] saved {args.save}")

    if not ok:
        print("\nFAIL: one or more counts diverge from the reference table.",
              file=sys.stderr)
        return 1
    print("\nOK: all counts match the reference table.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
