# k-uniform Euclidean Tiling Enumeration

Exhaustively enumerates all k-uniform tilings of the Euclidean plane by regular polygons. A k-uniform tiling uses k distinct vertex types (orbits under symmetry), where every vertex is incident to the same sequence of polygon shapes.

## Build & Run

```sh
make                                    # C++17, pthreads, zstd, SQLite, Boost headers
./eusolver --max-polygons 5 --workers 8 --output solutions
./eusolver --mode disk --max-polygons 10 --workers 8 --binary-solutions  # disk mode for k >= 10
./eusolver --mode disk --max-polygons 10 --workers 8 --binary-solutions --resume  # continue an interrupted run
```

Building against non-system dependency prefixes: `make EXTRA_CXXFLAGS='-isystem PREFIX/include' EXTRA_LDFLAGS='-LPREFIX/lib -Wl,-rpath,PREFIX/lib'`.

## Git workflow (Gitflow)

- `master` — stable releases only.
- `dev` — integration branch; features merge here, then `dev` → `master` for releases.
- `feature/<name>` — one branch per change, branched off `dev`, merged back into `dev` via PR/review.
- Keep commits small, atomic, and single-purpose with descriptive messages.
- Before merging a feature, run `scripts/regression.py` for counts/exact output and `scripts/benchmark.py` for partial-count and timing comparisons.

## Architecture

C++ sources live in `src/`.

| File | Role |
|------|------|
| `src/state.h` | Core `State`: a dart vector plus representative vertex catalogue types |
| `src/vertex_catalog.h/cpp` | 44 Euclidean vertex types (Conway symbols), per-slot geometry, symmetry reduction via `attachment_limit` |
| `src/solver.h/cpp` | `extend_into`: find tightest free edge, try gluing to existing edges or attaching new vertex types |
| `src/dfs_engine.h/cpp` | `DfsEngine`: in-place DFS with undo trail over the same tree as `extend_into`; incremental chain checks; donation and prune hooks |
| `src/canonical.h/cpp` | Exact colour-refinement canonical form (minimality test + 128-bit code hash), also for partial states |
| `src/transposition.h/cpp` | Shared bounded table of explored partial-state canonical hashes |
| `src/pruner.h/cpp` | Per-combo dedup pipeline (canonical default; WL/BFL optional) and parallel TES/Mortier output formatting |
| `src/disk_solver.h/cpp` | Disk mode: BFS fan-out, binary serialization, `WorkPool` (chunks, work sharing, `done_chunks.txt`), workers; legacy spill queue behind `--legacy-solver` |
| `src/mortier_geometry.h/cpp` | Exact Z4 affine development and translation-lattice derivation |
| `src/mortier_store.h/cpp` | Versioned Mortier codec, compressed SQLite storage, JSON conversion |
| `src/tes_store.h/cpp` | Compressed SQLite storage and safe extraction for TES documents |
| `src/zstd_stream.h` | Streaming zstd-decompressing `std::istream` + `compress_to_zst` helper |
| `src/main.cpp` | CLI, `SharedQueue`, progress display, mode dispatch |
| `euclidean_tiling.py` | Python reference implementation (1998 lines); written first |
| `visualize.py`, `visualize_geo.py` | Petal-diagram and geometric tiling visualizers |

## Key Concepts

- **Flags (edge slots)**: oriented edges of polygons at a vertex. Relations: rneig (σ), lneig (σ⁻¹), mirro (α), glue (edge pairing).
- **Polygon ring walk**: `a → rneig[a] → glue[rneig[a]] → rneig[glue[rneig[a]]] → …` traces a polygon perimeter. Slack = nominal size − segment count.
- **Extend step**: find edge with smallest slack, glue it to a matching free edge or attach a new vertex (only `attachment_limit(type)` slot positions tried).
- **Validity**: polygon sizes along any ring must be uniform; segment count ≤ nominal size; closed rings require nominal size to divide segment count.
- **Partial filtering**: forced closure of zero-slack rings, plus unique-partner propagation at k vertices. `DfsEngine` checks only chains touched by a glue (chain violations are monotone).
- **Leaf filter**: non-minimal (covering) solutions have bisimilar darts (non-discrete refinement) and are dropped at the source; the rest are deduplicated per worker on their canonical hash.
- **Transpositions**: the tilings below a node depend only on the type-preserving isomorphism class of its partial state, so nodes with <= k - `--tt-margin` vertices whose (discrete) canonical form was already seen are skipped.
- **Resume**: a chunk is appended to `done_chunks.txt` only after it and all subtrees donated from it finished and were flushed. `--resume` skips done chunks, repairs partial trailing records, appends to worker files and merges every `_worker_*` dir.
- **Pruner pipeline**: dedup is scoped to one combo (k + polygon-size set, an isomorphism invariant); `--dedup canon` (default) uses the exact canonical form, `wl`/`bfl` remain available.

## Operations

- `make` — build release binary `eusolver` (C++17, `-O3 -march=native`)
- `make debug` — build debug binary `eusolver_dbg`
- `make clean` — remove objects and binaries
- `make test` — safety, Mortier, engine-equivalence and canonical-form tests plus a k=1 regression
- `./eusolver --help` — full option list
- `./eusolver --canonical-hashes DIR` — print `k minimal hash` for every solution in raw text files (used by `scripts/regression.py` to compare runs up to isomorphism)
- Output on tmpfs `/tmp` fills quickly from k=13; benchmark on real disk. `--binary-solutions` is ~2.5x faster end to end than text output.

## Known Issues

- `propagate_forced` in `solver.cpp` is **fixed and enabled** (was broken); reduces search space ~23% at k=2, ~41% at k=3
- A second `propagate_forced` bug (open chains entered mid-way were treated as closed) is fixed; it removed ~2/3 of partial states.
- Memory mode still uses the legacy copy-per-child solver; disk mode uses `DfsEngine`.
- Pruned TES bytes depend on which labelled representative is kept first, so runs with different traversal orders are compared up to isomorphism, not byte for byte.
- Missing (4,8,8) vertex type from catalogue → k=1 count is 10 instead of 11.
- Counts through k=16 agree with the independent Tiling Atlas enumeration after fixing the `(4,4,4,4)A2` attachment orbit.
- The existing k=20 result database predates that fix, so its k=17 through k=20 counts are stale.

## Generated Counts

Counts through `k=16` agree with the Tiling Atlas reference. The catalogue still
omits `(4,8,8)`, accounting for the intentional difference at `k=1`.

| k | Unique tilings |
|---|---------------|
| 1 | 10 |
| 2 | 20 |
| 3 | 61 |
| 4 | 151 |
| 5 | 332 |
| 6 | 673 |
| 7 | 1,472 |
| 8 | 2,850 |
| 9 | 5,960 |
| 10 | 11,866 |
| 11 | 24,459 |
| 12 | 49,794 |
| 13 | 103,082 |
| 14 | 212,631 |
| 15 | 445,289 |
| 16 | 933,637 |
