# k-uniform Euclidean Tiling Enumeration

Exhaustively enumerates all k-uniform tilings of the Euclidean plane by regular polygons. A k-uniform tiling uses k distinct vertex types (orbits under symmetry), where every vertex is incident to the same sequence of polygon shapes.

## Build & Run

```sh
make                                    # C++17, pthreads
./eusolver --max-polygons 5 --workers 8 --output solutions
./eusolver --mode disk --max-polygons 10 --workers 8  # disk mode for k >= 10
```

## Git workflow (Gitflow)

- `master` — stable releases only.
- `dev` — integration branch; features merge here, then `dev` → `master` for releases.
- `feature/<name>` — one branch per change, branched off `dev`, merged back into `dev` via PR/review.
- Keep commits small, atomic, and single-purpose with descriptive messages.
- Before merging a feature, run `scripts/compare.py` to confirm solution counts still match the Reference Counts table and to measure the partials-processed delta vs. the baseline.

## Architecture

C++ sources live in `src/`.

| File | Role |
|------|------|
| `src/state.h` | Core `State` type: 7 parallel vectors (rneig, lneig, mirro, polygon_size, glue, label, cycles) |
| `src/vertex_catalog.h/cpp` | 44 Euclidean vertex types (Conway symbols), per-slot geometry, symmetry reduction via `attachment_limit` |
| `src/solver.h/cpp` | `extend_into`: find tightest free edge, try gluing to existing edges or attaching new vertex types |
| `src/pruner.h/cpp` | Global dedup pipeline: canonical labeling → WL hash → isomorphism fallback |
| `src/disk_solver.h/cpp` | Disk mode: BFS fan-out, binary serialization, parallel DFS workers that spill/reload to disk |
| `src/zstd_stream.h` | Streaming zstd-decompressing `std::istream` + `compress_to_zst` helper |
| `src/main.cpp` | CLI, `SharedQueue`, progress display, mode dispatch |
| `euclidean_tiling.py` | Python reference implementation (1998 lines); written first |
| `visualize.py`, `visualize_geo.py` | Petal-diagram and geometric tiling visualizers |

## Key Concepts

- **Flags (edge slots)**: oriented edges of polygons at a vertex. Relations: rneig (σ), lneig (σ⁻¹), mirro (α), glue (edge pairing).
- **Polygon ring walk**: `a → rneig[a] → glue[rneig[a]] → rneig[glue[rneig[a]]] → …` traces a polygon perimeter. Slack = nominal size − segment count.
- **Extend step**: find edge with smallest slack, glue it to a matching free edge or attach a new vertex (only `attachment_limit(type)` slot positions tried).
- **Validity**: polygon sizes along any ring must be uniform; segment count ≤ nominal size; closed rings require nominal size to divide segment count.
- **Symmetry-reduced darts**: some vertex types represent a whole polygon with a single (self-glued) dart, e.g. `(6,6,6)S`, `(4,4,4,4)S4`, `(3,3,3,3,3,3)S6`. A size-`s` polygon can therefore be represented by any divisor of `s` darts, so global counting invariants of the form `count_s % s == 0` are **not** valid (they prune legal tilings). Sound pruning must be local (ring slack, edge-pairing size compatibility) or based on symmetry, not on dart-count divisibility.
- **Partial dedup**: per-worker 64-bit FNV-1a fingerprint → capped bucket (8) → bit-level alias refinement isomorphism check.
- **Pruner pipeline**: (1) canonical labeling via alias refinement, (2) 1-WL or 2-WL colour refinement with FNV hash, (3) full isomorphism on hash collisions.

## Operations

- `make` — build release binary `eusolver` (C++17, `-O3 -march=native`)
- `make debug` — build debug binary `eusolver_dbg`
- `make clean` — remove objects and binaries
- `./eusolver --help` — full option list

## Known Issues

- `propagate_forced` in `solver.cpp` is **fixed and enabled** (was broken); reduces search space ~23% at k=2, ~41% at k=3
- Disk mode has integration bugs at small fan-out targets; loses some k=1 solutions.
- Missing (4,8,8) vertex type from catalogue → k=1 count is 10 instead of 11.
- Partial dedup is per-worker; cross-worker duplicates are not eliminated.
- Pruner is I/O-bound for k > 6.

## Reference Counts

| k | Unique tilings |
|---|---------------|
| 1 | 10 |
| 2 | 20 |
| 3 | 61 |
| 4 | 151 |
| 5 | 332 |
| 6 | 673 |
| 7 | 1,472 |
| 8 | 2,849 |
