# Implementation: Euclidean k-uniform Tiling Solver (C++)

## Build

```sh
make
# or:
g++ -O3 -std=c++17 -pthread -march=native -Isrc \
    src/main.cpp src/solver.cpp src/pruner.cpp src/vertex_catalog.cpp \
    src/disk_solver.cpp src/bfl.cpp src/sig_tree.cpp src/metrics.cpp \
    -lzstd -o eusolver
```

Zero warnings with `-Wall -Wextra`. Requires C++17 (structured bindings,
`std::filesystem`, `if`-init) and pthreads.

## Usage

```
./eusolver --max-polygons 8 --workers 8 [--output solutions]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--max-polygons N` | 5 | Maximum number of vertex types (k) |
| `--workers N` | hw threads | Number of solver + pruner threads |
| `--output DIR` | solutions | Output directory |
| `--mode memory\|disk` | memory | Solver mode |
| `--fanout N` | 40000 | BFS fan-out target (disk mode only) |
| `--spill N` | 50000 | Disk spill threshold (disk mode only) |

## File layout

C++ sources live in `src/`.

```
├── src/
│   ├── vertex_catalog.h       # Static catalogue interface
│   ├── vertex_catalog.cpp     # 44 vertex types with per-slot data tables
│   ├── state.h                # State struct (7 parallel arrays)
│   ├── solver.h               # EuclideanSolver class + extend_into template
│   ├── solver.cpp             # Core search: extend, check_partial, analyze_cycles,
│   │                          #   partial dedup, fingerprint, isomorphism, output
│   ├── pruner.h               # SolutionPruner + WLPruner classes + wl_hash()
│   ├── pruner.cpp             # Pruning pipeline: parsing, canonical check,
│   │                          #   WL hash, solutions_match, cycle-final writer
│   ├── disk_solver.h          # Binary I/O + BFS fan-out + disk DFS worker
│   ├── disk_solver.cpp        # Compact serialization, spill/reload, fan-out, worker
│   ├── zstd_stream.h          # Streaming zstd input + compress_to_zst helper
│   ├── bfl.h/cpp              # Breadth-First Labelling canonical signatures
│   ├── sig_tree.h/cpp         # Set-tree signature helpers
│   ├── metrics.h/cpp          # TensorBoard metrics writer
│   └── main.cpp               # CLI, shared queue, progress thread, pipeline orchestration
├── Makefile                   # Build rules
├── ALGORITHM.md               # Algorithm documentation
└── IMPLEMENTATION.md          # This file
```

## Architecture

### `State` struct (state.h)

The central data structure. Contains 7 parallel vectors indexed by slot ID:

```cpp
struct State {
    std::vector<int> rneig;         // clockwise neighbour
    std::vector<int> lneig;         // counter-clockwise neighbour
    std::vector<int> polygon_size;  // polygon size to the right
    std::vector<int> mirro;         // mirror-image slot
    std::vector<int> glue;          // −1 = free, else paired slot ID
    std::vector<std::string> label; // Conway label string
    std::vector<int> vertype;       // per-vertex catalogue index
};
```

`rneig`, `lneig`, `mirro`, `polygon_size`, and `label` are fully determined
by `vertype` (they are concatenations of the per-vertex-type table rows).
Only `glue` and `vertype` carry state-specific information.

### Solver pipeline

```
                    main()
                      │
        ┌─────────────┴─────────────┐
        ▼                           ▼
  memory mode                 disk mode
        │                           │
  SharedQueue ◄── 44 initials   BFS fan-out
        │                      chunk binary files
  8 worker threads             parallel disk workers
  batch pop/push (B=64)        spill/reload to disk
  per-worker dedup             write solution text
        │                           │
        └─────────────┬─────────────┘
                      ▼
              merge worker outputs
                      │
              WLPruner (parallel)
                      │
              pruned output + tilings.sqlite3
```

### Shared work queue (`SharedQueue`, main.cpp)

Workers pull batches of up to 64 states from a shared deque protected by a
mutex and condition variable. New partials are pushed back in bulk. Workers
sleep when the queue is empty and active > 0; they wake via `notify_all`
when new work arrives. Termination occurs when queue is empty and active=0.

### Progress display (main.cpp)

A dedicated progress thread reads shared atomic counters every 500 ms and
prints a clean status line:

```
  P:320648  S:19811  Q:37206  A:512  ETA:<1s
```

- `P` = partials processed, `S` = raw solutions found
- `Q` = queue depth, `A` = active states (held by workers)
- `ETA` = queue × EMA(seconds per partial) × avg-branch-factor

The pruner prints per-file progress with accumulating per-k unique counts:

```
  pruner: 25/47 files  [k1:10 k2:20 k3:61]
```

### `extend_into` template (solver.h)

The core search step is a static template method, allowing both the
in-memory solver and the disk worker to share the same logic:

```cpp
template<typename F>
static void extend_into(const State& st, F&& cb, int max_polygons);
```

`cb` is called for each valid candidate state. In memory mode, the callback
writes complete solutions and enqueues partials. In disk mode, it writes
solution text and pushes partials to the disk worker's deque.

### Partial dedup (solver.cpp)

Each worker maintains a `fingerprint → vector<State>` map (lock-free,
per-worker). Before enqueuing a candidate:

1. Compute `state_fingerprint()` — FNV-1a over 6-tuples + vertex histogram.
2. Look up the fingerprint bucket.
3. Run `are_isomorphic_partial()` — bit-level alias refinement — against
   stored entries. If isomorphic, discard. Otherwise store and enqueue.
4. Bucket size capped at 8 to bound memory.

### Pruner parallelism (pruner.cpp)

The `WLPruner` computes canonical labelings and WL hashes in parallel:

```cpp
// Atomic-indexed work distribution
std::atomic<size_t> idx{0};
for (int w = 0; w < num_workers; ++w)
    threads.emplace_back([&]() {
        while (size_t i = idx.fetch_add(1), i < records.size())
            results[i] = compute_canonical_and_wl(records[i].state);
    });
```

The canonical check (`is_canonical_labeling`) uses set-based alias refinement
(O(n² × iterations)). The WL hash (`wl_hash`) uses 3-round colour refinement
with FNV-1a and neighbour-type prefixes. Both are pure functions with no
shared state, making them trivially parallelizable.

The sequential filtering phase uses `solutions_by_hash_` (a
`map<wl_hash_string, vector<State>>`) for O(1) dedup lookups. Only on WL
hash collisions (theoretically impossible for Euclidean tiling graphs) does
it fall back to the O(n²) `solutions_match`.

### TES storage (`tes_store.cpp`)

The pruner renders each HyperRogue `.tes` document in memory and appends it to
`wl/tilings.sqlite3`, rather than creating millions of small files. The SQLite
database has three tables:

- `metadata`: format version and compression algorithm.
- `chunks`: zstd-compressed concatenated document data and size metadata.
- `entries`: stable numeric ID, byte range, combo, signature, and legacy name.

Chunks target 8 MiB of uncompressed text. A transaction is committed after
each merged solver input file. Input files are retained until the complete
database has been published, then removed together. This bounds data at risk
and permits a failed prune to be retried without requiring a transaction or
zstd frame per tiny document. A document larger than the target is stored as
its own chunk.

`--extract-tes DB --extract-output DIR` recreates the combo-directory layout.
`--tes-id N` limits extraction to one entry. Extraction validates stored path
components, zstd sizes, and byte ranges, and refuses to overwrite files.

The database is a final-output container, not a pruner checkpoint. A fresh
pruner run writes `tilings.sqlite3.tmp` and atomically publishes it only after
all inputs succeed, preserving a prior completed database on failure. Global
deduplication state still lives in memory and cannot currently be resumed
safely.

### Binary serialization (disk_solver.cpp)

For k ≥ 10, states must be spilled to disk. The compact binary format:

```
uint8  num_vertices
uint8  num_edges
num_vertices × uint8   vertex type indices
num_edges × int16      glue array (−1 = 0xFFFF)
```

Derived arrays (rneig, lneig, mirro, polygon_size, label) are reconstructed
on deserialization from the vertex type catalogue. At k=20, this is ~500
bytes per state vs ~2,000 for the text format.

The disk worker uses spill/reload:

```cpp
if (queue.size() > spill_threshold) {
    // Write half of queue to spill file
    write_i32(sf, count);
    for each state: write_state_bin(sf, state);
    queue.erase(first_half);
}
// Later:
while (queue.empty() && spill_has_data) {
    read_i32(sf); // batch count
    for count: queue.push_front(read_state_bin(sf));
}
```

### Thread safety

| Component | Strategy |
|-----------|----------|
| `SharedQueue` | Mutex + condition variable, batch push/pop |
| `write_solution_static` | Per-worker mutex on output tracking maps |
| Partial dedup | Per-worker (lock-free) fingerprint buckets |
| WL hash computation | Atomic-index parallel loop, no shared mutable state |
| `solutions_by_hash_` | Sequential accumulation (must be serial) |
| Progress display | Shared atomics, read-only from progress thread |

## Performance

All timings are wall-clock on an 8-core machine, k=7:

| Phase | Time | Notes |
|-------|------|-------|
| Solver (8 workers) | ~2.0s | 322K partials processed |
| Output merge | negligible | File concatenation |
| Pruner (8 workers) | ~3.0s | 66 files, 20K raw → 2.7K unique, parallel WL |

Per-k unique tiling count growth (verified against Python):

| k | Unique | Raw solutions | Solver time (8w) |
|---|--------|--------------|-------------------|
| 4 | 151 | 2,841 | 0.5s |
| 5 | 332 | 8,323 | 1.4s |
| 6 | 673 | ~17K | 4.0s |
| 7 | 1,472 | ~20K | 2.0s |
| 8 | 2,849 | ~37K | 25.8s |

## Known limitations

1. **`propagate_forced` disabled** — the constraint propagation function is
   implemented but produces incorrect solution counts when enabled. Fixing
   it would reduce the search space by an estimated 40-60%.

2. **Disk mode integration** — binary serialization, BFS fan-out, and disk
   worker infrastructure exist but the chunk-distribution to workers loses
   some k=1 solutions when the fan-out target is small. Needs debugging
   before production use at k ≥ 10.

3. **No (4,8,8) vertex type** — the catalogue omits the semi-regular
   Archimedean tiling (4,8,8), so k=1 count is 10 instead of the literature
   value of 11.

4. **Partial dedup is per-worker** — duplicates across different workers'
   search spaces are not detected. A global shared dedup would increase
   pruning effectiveness at the cost of lock contention.

5. **Pruner is I/O-bound** — writing cycle-final descriptions and `.tes`
   files dominates pruner time for k > 6. The parallel WL hash computation
   helps but the overall speedup is modest.
