#!/usr/bin/env python3
"""Compare repeat samples from baseline and candidate solver executables."""

from __future__ import annotations

import argparse
import pathlib
import statistics
import sys
import tempfile

from solver_harness import RunResult, run_solver


def median(samples: list[RunResult], field: str) -> float:
    return statistics.median(float(getattr(sample, field)) for sample in samples)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", default="./eusolver")
    parser.add_argument("--max-k", type=int, default=2)
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--mode", choices=("memory", "disk"), default="memory")
    parser.add_argument("solver_args", nargs=argparse.REMAINDER,
                        help="solver arguments after --, for example -- --binary-solutions")
    parser.add_argument("--max-slowdown", type=float,
                        help="fail if any candidate median time / baseline exceeds this ratio")
    parser.add_argument("--work-dir", type=pathlib.Path,
                        help="retain each benchmark output under this new/empty directory")
    args = parser.parse_args()
    if args.max_k < 1 or args.workers < 1 or args.repeats < 1:
        parser.error("--max-k, --workers, and --repeats must be positive")
    if args.max_slowdown is not None and args.max_slowdown <= 0:
        parser.error("--max-slowdown must be positive")
    solver_args = args.solver_args
    if solver_args and solver_args[0] == "--":
        solver_args = solver_args[1:]

    temporary = None
    if args.work_dir:
        root = args.work_dir.resolve()
        root.mkdir(parents=True, exist_ok=False)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="eusolver-benchmark-")
        root = pathlib.Path(temporary.name)

    samples: dict[str, list[RunResult]] = {"baseline": [], "candidate": []}
    try:
        # Alternate order to reduce systematic cache/temperature bias.
        for repeat in range(args.repeats):
            order = ("baseline", "candidate") if repeat % 2 == 0 else ("candidate", "baseline")
            for name in order:
                executable = args.baseline if name == "baseline" else args.candidate
                result = run_solver(executable, root / f"{repeat + 1}-{name}", args.max_k,
                                    args.workers, mode=args.mode, extra_args=solver_args)
                samples[name].append(result)
                partials = str(result.partials) if result.partials is not None else "n/a"
                print(f"{name:9} sample {repeat + 1}: partials={partials} "
                      f"solver={result.solver_seconds:.3f}s "
                      f"pruner={result.pruner_seconds:.3f}s wall={result.wall_seconds:.3f}s")

        print("\nmedians")
        ratios: list[tuple[str, float]] = []
        for field, label in (("solver_seconds", "solver"),
                             ("pruner_seconds", "pruner"),
                             ("wall_seconds", "end-to-end")):
            base = median(samples["baseline"], field)
            candidate = median(samples["candidate"], field)
            ratio = candidate / base if base > 0 else float("inf")
            ratios.append((label, ratio))
            print(f"{label:10} baseline={base:.3f}s candidate={candidate:.3f}s "
                  f"ratio={ratio:.3f}x")
        for name in ("baseline", "candidate"):
            values = [sample.partials for sample in samples[name]]
            rendered = ", ".join("n/a" if value is None else str(value) for value in values)
            print(f"{name:10} partial counts: {rendered}")

        baseline_partials = {sample.partials for sample in samples["baseline"]
                             if sample.partials is not None}
        candidate_partials = {sample.partials for sample in samples["candidate"]
                              if sample.partials is not None}
        if baseline_partials and candidate_partials and baseline_partials != candidate_partials:
            print("FAIL baseline and candidate partial counts differ", file=sys.stderr)
            return 1

        if args.max_slowdown is None:
            print("No failure threshold requested; benchmark is informational.")
            return 0
        failures = [(label, ratio) for label, ratio in ratios if ratio > args.max_slowdown]
        if failures:
            for label, ratio in failures:
                print(f"FAIL {label} slowdown {ratio:.3f}x exceeds "
                      f"{args.max_slowdown:.3f}x", file=sys.stderr)
            return 1
        print(f"PASS all median time ratios <= {args.max_slowdown:.3f}x")
        return 0
    except (RuntimeError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        if temporary is not None:
            temporary.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
