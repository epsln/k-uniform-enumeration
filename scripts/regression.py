#!/usr/bin/env python3
"""Run exact-set, reference-count, and memory/disk solver regressions."""

from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile

from solver_harness import (REFERENCE_COUNTS, canonical_solutions, describe_difference,
                            parse_k_spec, run_solver, select_solutions)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", default="./eusolver")
    parser.add_argument("--baseline", help="executable whose exact canonical set is expected")
    parser.add_argument("--max-k", type=int, default=1,
                        help="enumeration limit; 1 is fast, up to 8 has reference counts")
    parser.add_argument("--reference-k",
                        help="counts to check, e.g. 1-3,5 (default: 1 through max-k)")
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--exact-workers", type=int, default=1,
                        help="workers for deterministic baseline set comparison (default: 1)")
    parser.add_argument("--skip-mode-equivalence", action="store_true")
    parser.add_argument("--work-dir", type=pathlib.Path,
                        help="retain outputs under this new/empty directory")
    args = parser.parse_args()
    if not 1 <= args.max_k <= 8:
        parser.error("--max-k must be in 1..8")
    if args.workers < 1 or args.exact_workers < 1:
        parser.error("--workers and --exact-workers must be positive")
    try:
        reference_k = parse_k_spec(args.reference_k or f"1-{args.max_k}")
    except ValueError as error:
        parser.error(str(error))
    if max(reference_k) > args.max_k:
        parser.error("--reference-k cannot exceed --max-k")

    temporary = None
    if args.work_dir:
        root = args.work_dir.resolve()
        root.mkdir(parents=True, exist_ok=False)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="eusolver-regression-")
        root = pathlib.Path(temporary.name)

    try:
        candidate_run = run_solver(args.candidate, root / "candidate-memory",
                                   args.max_k, args.workers)
        candidate = canonical_solutions(candidate_run.output_dir)
        for k in reference_k:
            actual = len(candidate.get(k, ()))
            expected = REFERENCE_COUNTS[k]
            if actual != expected:
                raise AssertionError(f"reference count k={k}: expected {expected}, got {actual}")
            print(f"PASS reference count k={k}: {actual}")

        if args.baseline:
            if args.workers == args.exact_workers:
                exact_candidate = candidate
            else:
                exact_run = run_solver(args.candidate, root / "candidate-exact",
                                       args.max_k, args.exact_workers)
                exact_candidate = canonical_solutions(exact_run.output_dir)
            baseline_run = run_solver(args.baseline, root / "baseline-memory",
                                      args.max_k, args.exact_workers)
            baseline = select_solutions(canonical_solutions(baseline_run.output_dir), args.max_k)
            candidate_set = select_solutions(exact_candidate, args.max_k)
            if candidate_set != baseline:
                raise AssertionError(describe_difference(baseline, candidate_set))
            print(f"PASS baseline exact canonical set: {len(candidate_set)} solutions")
        else:
            print("SKIP baseline exact-set comparison (pass --baseline EXE to enable)")

        if not args.skip_mode_equivalence:
            disk_run = run_solver(args.candidate, root / "candidate-disk", 1, args.workers,
                                  mode="disk", extra_args=("--fanout", "25", "--chunks",
                                                          str(args.workers), "--no-spill"))
            memory_k1 = select_solutions(candidate, 1)
            disk_k1 = select_solutions(canonical_solutions(disk_run.output_dir), 1)
            if memory_k1 != disk_k1:
                raise AssertionError(describe_difference(memory_k1, disk_k1))
            print(f"PASS memory/disk exact canonical set at k=1: {len(disk_k1)} solutions")
    except (AssertionError, RuntimeError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        if args.work_dir:
            print(f"outputs retained in {root}", file=sys.stderr)
        return 1
    finally:
        if temporary is not None:
            temporary.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
