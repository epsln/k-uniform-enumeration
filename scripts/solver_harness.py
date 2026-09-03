#!/usr/bin/env python3
"""Shared, dependency-free support for solver regression tests and benchmarks."""

from __future__ import annotations

import collections
import ctypes
import ctypes.util
import hashlib
import os
import pathlib
import re
import sqlite3
import subprocess
import time
from dataclasses import dataclass
from typing import Iterable, Optional


REFERENCE_COUNTS = {1: 10, 2: 20, 3: 61, 4: 151, 5: 332, 6: 673,
                    7: 1472, 8: 2849}


@dataclass(frozen=True)
class RunResult:
    command: tuple[str, ...]
    output_dir: pathlib.Path
    output: str
    partials: Optional[int]
    solver_seconds: float
    pruner_seconds: float
    wall_seconds: float


def executable_path(value: str) -> str:
    path = pathlib.Path(value).expanduser().resolve()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise ValueError(f"not an executable file: {value}")
    return str(path)


def run_solver(executable: str, output_dir: pathlib.Path, max_k: int, workers: int,
               mode: str = "memory", extra_args: Iterable[str] = ()) -> RunResult:
    output_dir = output_dir.resolve()
    if output_dir.exists():
        raise ValueError(f"refusing to reuse output directory: {output_dir}")
    command = (executable_path(executable), "--max-polygons", str(max_k),
               "--workers", str(workers), "--mode", mode,
               "--output", str(output_dir), *extra_args)
    start = time.perf_counter()
    completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, errors="replace", check=False)
    wall = time.perf_counter() - start
    if completed.returncode != 0:
        raise RuntimeError(
            f"solver exited with status {completed.returncode}: {' '.join(command)}\n"
            f"{completed.stdout}")

    solver_matches = re.findall(r"^Solver phase:\s*([0-9.]+)s(?:\s*\((\d+) partials)?",
                                completed.stdout, re.MULTILINE)
    pruner_matches = re.findall(r"^Pruner phase:\s*([0-9.]+)s", completed.stdout,
                                re.MULTILINE)
    if len(solver_matches) != 1 or len(pruner_matches) != 1:
        raise RuntimeError(f"could not parse phase timings from {' '.join(command)}\n"
                           f"{completed.stdout}")
    solver_seconds, partials = solver_matches[0]
    return RunResult(command, output_dir, completed.stdout,
                     int(partials) if partials else None,
                     float(solver_seconds), float(pruner_matches[0]), wall)


class _Zstd:
    def __init__(self) -> None:
        name = ctypes.util.find_library("zstd")
        if not name:
            raise RuntimeError("libzstd is required to read canonical solution databases")
        self.lib = ctypes.CDLL(name)
        self.lib.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                             ctypes.c_void_p, ctypes.c_size_t]
        self.lib.ZSTD_decompress.restype = ctypes.c_size_t
        self.lib.ZSTD_isError.argtypes = [ctypes.c_size_t]
        self.lib.ZSTD_isError.restype = ctypes.c_uint
        self.lib.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]
        self.lib.ZSTD_getErrorName.restype = ctypes.c_char_p

    def decompress(self, data: bytes, size: int) -> bytes:
        source = ctypes.create_string_buffer(data)
        destination = ctypes.create_string_buffer(size)
        result = self.lib.ZSTD_decompress(destination, size, source, len(data))
        if self.lib.ZSTD_isError(result):
            message = self.lib.ZSTD_getErrorName(result).decode("ascii", "replace")
            raise RuntimeError(f"zstd decompression failed: {message}")
        if result != size:
            raise RuntimeError(f"zstd size mismatch: expected {size}, got {result}")
        return destination.raw[:result]


def canonical_solutions(output_dir: pathlib.Path) -> dict[int, frozenset[str]]:
    """Return exact content hashes of every canonical TES document, grouped by k."""
    database = output_dir / "wl" / "tilings.sqlite3"
    if not database.is_file():
        raise RuntimeError(f"canonical solution database is missing: {database}")
    connection = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
    zstd = _Zstd()
    solutions: dict[int, set[str]] = collections.defaultdict(set)
    entry_count = 0
    try:
        chunks = connection.execute(
            "SELECT id, record_count, uncompressed_size, data FROM chunks ORDER BY id")
        for chunk_id, record_count, raw_size, compressed in chunks:
            raw = zstd.decompress(compressed, raw_size)
            entries = list(connection.execute(
                "SELECT byte_offset, byte_length, combo FROM entries "
                "WHERE chunk_id=? ORDER BY ordinal", (chunk_id,)))
            if len(entries) != record_count:
                raise RuntimeError(f"chunk {chunk_id} record count is inconsistent")
            for offset, length, combo in entries:
                if offset < 0 or length < 0 or offset + length > len(raw):
                    raise RuntimeError(f"chunk {chunk_id} contains invalid entry bounds")
                try:
                    k = int(combo.split("_", 1)[0])
                except (ValueError, IndexError) as error:
                    raise RuntimeError(f"invalid canonical combo {combo!r}") from error
                digest = hashlib.sha256(raw[offset:offset + length]).hexdigest()
                if digest in solutions[k]:
                    raise RuntimeError(f"duplicate canonical document for k={k}: {digest}")
                solutions[k].add(digest)
                entry_count += 1
        database_count = connection.execute("SELECT count(*) FROM entries").fetchone()[0]
        if entry_count != database_count:
            raise RuntimeError(f"read {entry_count} of {database_count} canonical entries")
    finally:
        connection.close()
    return {k: frozenset(values) for k, values in solutions.items()}


def select_solutions(solutions: dict[int, frozenset[str]], max_k: int) -> set[tuple[int, str]]:
    return {(k, digest) for k, values in solutions.items() if k <= max_k for digest in values}


def parse_k_spec(spec: str, maximum: int = 8) -> list[int]:
    values: set[int] = set()
    for item in spec.split(","):
        item = item.strip()
        if not item:
            continue
        if "-" in item:
            first, last = item.split("-", 1)
            values.update(range(int(first), int(last) + 1))
        else:
            values.add(int(item))
    if not values or min(values) < 1 or max(values) > maximum:
        raise ValueError(f"k selection must be within 1..{maximum}")
    return sorted(values)


def describe_difference(expected: set[tuple[int, str]], actual: set[tuple[int, str]]) -> str:
    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    lines = [f"canonical sets differ: {len(missing)} missing, {len(extra)} extra"]
    lines.extend(f"  missing k={k} sha256={digest}" for k, digest in missing[:10])
    lines.extend(f"  extra   k={k} sha256={digest}" for k, digest in extra[:10])
    if len(missing) + len(extra) > 20:
        lines.append("  (difference list truncated)")
    return "\n".join(lines)
