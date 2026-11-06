#!/usr/bin/env python3
"""Stream canonical tilings and summarize combinatorial result statistics."""

from __future__ import annotations

import argparse
import csv
import math
import os
import re
import sqlite3
from concurrent.futures import ProcessPoolExecutor
from collections import Counter, defaultdict
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from solver_harness import iter_tes_entries  # noqa: E402


COMBO_RE = re.compile(r"^(\d+)_")


def base_symbols(signature: str) -> list[str]:
    """Return distinct vertex configurations, ignoring catalogue variants."""
    return sorted(set(signature_symbols(signature)))


def signature_symbols(signature: str) -> list[str]:
    """Return vertex configurations in order, ignoring catalogue variants."""
    symbols = set()
    start = 0
    result = []
    while True:
        start = signature.find("(", start)
        if start < 0:
            return result
        end = signature.find(")", start + 1)
        if end < 0:
            raise ValueError(f"malformed vertex signature {signature!r}")
        result.append(signature[start : end + 1])
        start = end + 1


def multiplicity_profile(signature: str) -> str:
    """Return repeated-configuration multiplicities, e.g. ``2+1`` or ``1+1+1``."""
    symbols = signature_symbols(signature)
    counts = Counter(symbols)
    return "+".join(map(str, sorted(counts.values(), reverse=True)))


def read_rows(database: Path, id_range: tuple[int, int] | None = None):
    for _, combo, signature, _, _ in iter_tes_entries(
            database, with_documents=False, id_range=id_range):
        match = COMBO_RE.match(combo)
        if not match:
            raise ValueError(f"cannot extract k from combo {combo!r}")
        yield int(match.group(1)), combo, signature


def analyse_rows(rows):
    distributions: dict[int, Counter[int]] = defaultdict(Counter)
    profiles: dict[int, Counter[str]] = defaultdict(Counter)
    configuration_frequency: Counter[str] = Counter()
    total = Counter()
    configuration_by_k: dict[int, Counter[str]] = defaultdict(Counter)
    entropy_sum = Counter()
    max_multiplicity = defaultdict(Counter)

    polygon_signatures: dict[int, Counter[str]] = defaultdict(Counter)
    for k, combo, signature in rows:
        multiplicities = Counter(signature_symbols(signature))
        symbols = multiplicities.keys()
        distributions[k][len(symbols)] += 1
        profiles[k]["+".join(map(str, sorted(multiplicities.values(), reverse=True)))] += 1
        configuration_frequency.update(symbols)
        configuration_by_k[k].update(symbols)
        polygon_signatures[k][combo.split("_", 1)[1]] += 1
        entropy = -sum((count / k) * math.log2(count / k)
                       for count in multiplicities.values())
        entropy_sum[k] += entropy
        max_multiplicity[k][max(multiplicities.values())] += 1
        total[k] += 1

    return (distributions, profiles, configuration_frequency, total,
            polygon_signatures, configuration_by_k, entropy_sum, max_multiplicity)


def analyse_range(task):
    database, start, end = task
    return analyse_rows(read_rows(Path(database), (start, end)))


def analyse(database: Path, jobs: int = 1):
    if jobs <= 1:
        return analyse_rows(read_rows(database))

    connection = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
    try:
        version = connection.execute(
            "SELECT value FROM metadata WHERE key='schema_version'").fetchone()[0]
        if version != "2":
            return analyse_rows(read_rows(database))
        first, last = connection.execute("SELECT min(id), max(id) FROM chunks").fetchone()
    finally:
        connection.close()

    if first is None:
        return analyse_rows(())
    jobs = min(jobs, last - first + 1)
    span = last - first + 1
    tasks = [(str(database), first + span * i // jobs,
              first + span * (i + 1) // jobs) for i in range(jobs)]
    parts = []
    with ProcessPoolExecutor(max_workers=jobs) as pool:
        for result in pool.map(analyse_range, tasks):
            parts.append(result)

    distributions = defaultdict(Counter)
    profiles = defaultdict(Counter)
    configuration_frequency = Counter()
    total = Counter()
    polygon_signatures = defaultdict(Counter)
    configuration_by_k = defaultdict(Counter)
    entropy_sum = Counter()
    max_multiplicity = defaultdict(Counter)
    for part in parts:
        (part_dist, part_profiles, part_frequency, part_total,
         part_polygons, part_configurations, part_entropy, part_maximum) = part
        for k, counts in part_dist.items():
            distributions[k].update(counts)
        for k, counts in part_profiles.items():
            profiles[k].update(counts)
        configuration_frequency.update(part_frequency)
        total.update(part_total)
        for k, counts in part_polygons.items():
            polygon_signatures[k].update(counts)
        for k, counts in part_configurations.items():
            configuration_by_k[k].update(counts)
        entropy_sum.update(part_entropy)
        for k, counts in part_maximum.items():
            max_multiplicity[k].update(counts)
    return (distributions, profiles, configuration_frequency, total,
            polygon_signatures, configuration_by_k, entropy_sum, max_multiplicity)


def print_report(distributions, profiles, configuration_frequency, total,
                 polygon_signatures, configuration_by_k, entropy_sum,
                 max_multiplicity):
    print("m-Archimedean distribution")
    print("k,total,m=1,m=2,m=3,m=4,m=5,m=6,...")
    for k in sorted(total):
        values = ",".join(str(distributions[k][m]) for m in sorted(distributions[k]))
        print(f"{k},{total[k]},{values}")

    print("\nMultiplicity profiles")
    print("k,profile,count")
    for k in sorted(profiles):
        for profile, count in sorted(profiles[k].items()):
            print(f"{k},{profile},{count}")

    print("\nMost frequent vertex configurations")
    print("configuration,count")
    for configuration, count in configuration_frequency.most_common(20):
        print(f"{configuration},{count}")

    print("\nPolygon-size signatures")
    print("k,signature,count")
    for k in sorted(polygon_signatures):
        for signature, count in polygon_signatures[k].most_common():
            print(f"{k},{signature},{count}")

    print("\nConfiguration prevalence")
    print("k,configuration,tilings,prevalence")
    for k in sorted(configuration_by_k):
        for configuration, count in configuration_by_k[k].most_common(10):
            print(f"{k},{configuration},{count},{count / total[k]:.6f}")

    print("\nReuse statistics")
    print("k,mean_entropy,maximum_multiplicity_distribution")
    for k in sorted(total):
        maximums = ";".join(f"{m}:{n}" for m, n in sorted(max_multiplicity[k].items()))
        print(f"{k},{entropy_sum[k] / total[k]:.6f},{maximums}")

    print("\nGrowth ratios")
    print("k,total,ratio_to_previous")
    previous = None
    for k in sorted(total):
        ratio = "-" if previous is None else f"{total[k] / previous:.3f}"
        print(f"{k},{total[k]},{ratio}")
        previous = total[k]


def write_metric_csvs(outdir: Path, distributions, profiles, total,
                      polygon_signatures, configuration_by_k, entropy_sum,
                      max_multiplicity):
    outdir.mkdir(parents=True, exist_ok=True)

    with (outdir / "m_distribution.csv").open("w", newline="") as output:
        max_m = max((m for values in distributions.values() for m in values), default=0)
        writer = csv.writer(output)
        writer.writerow(["k", *[f"m={m}" for m in range(1, max_m + 1)], "total"])
        for k in sorted(total):
            writer.writerow([k, *[distributions[k][m] for m in range(1, max_m + 1)], total[k]])

    with (outdir / "multiplicity_profiles.csv").open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(["k", "profile", "count"])
        for k in sorted(profiles):
            for profile, count in sorted(profiles[k].items()):
                writer.writerow([k, profile, count])

    with (outdir / "configuration_prevalence.csv").open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(["k", "configuration", "tilings", "prevalence"])
        for k in sorted(configuration_by_k):
            for configuration, count in configuration_by_k[k].items():
                writer.writerow([k, configuration, count, count / total[k]])

    with (outdir / "polygon_signatures.csv").open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(["k", "signature", "count"])
        for k in sorted(polygon_signatures):
            for signature, count in polygon_signatures[k].items():
                writer.writerow([k, signature, count])

    with (outdir / "reuse_statistics.csv").open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(["k", "total", "mean_entropy", "max_multiplicity", "count"])
        for k in sorted(total):
            for maximum, count in sorted(max_multiplicity[k].items()):
                writer.writerow([k, total[k], entropy_sum[k] / total[k], maximum, count])

    with (outdir / "growth.csv").open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(["k", "total", "ratio_to_previous"])
        previous = None
        for k in sorted(total):
            writer.writerow([k, total[k], "" if previous is None else total[k] / previous])
            previous = total[k]


def write_csv(path: Path, distributions, total):
    max_m = max((m for values in distributions.values() for m in values), default=0)
    with path.open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(["k", *[f"m={m}" for m in range(1, max_m + 1)], "total"])
        for k in sorted(total):
            writer.writerow([k, *[distributions[k][m] for m in range(1, max_m + 1)], total[k]])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("database", type=Path)
    parser.add_argument("--csv", type=Path, help="write the k/m table to CSV")
    parser.add_argument("--outdir", type=Path, help="write all metric CSVs here")
    parser.add_argument("--jobs", type=int, default=min(16, os.cpu_count() or 1),
                        help="parallel database scan workers")
    args = parser.parse_args()

    (distributions, profiles, frequencies, total, polygon_signatures,
     configuration_by_k, entropy_sum, max_multiplicity) = analyse(args.database, args.jobs)
    print_report(distributions, profiles, frequencies, total, polygon_signatures,
                 configuration_by_k, entropy_sum, max_multiplicity)
    if args.csv:
        write_csv(args.csv, distributions, total)
    if args.outdir:
        write_metric_csvs(args.outdir, distributions, profiles, total,
                          polygon_signatures, configuration_by_k,
                          entropy_sum, max_multiplicity)


if __name__ == "__main__":
    main()
