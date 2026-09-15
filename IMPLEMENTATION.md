# Implementation Guide

This document describes the C++ implementation shipped in release 1.0.x. See
`ALGORITHM.md` for the combinatorial search itself.

## Build

```sh
make
make debug
make test TEST_MAX_K=2
```

The build requires a C++17 compiler, pthreads, zstd, SQLite, and the Boost
Multiprecision headers. Release builds use `-O3 -march=native`.

## Source layout

| File | Responsibility |
|------|----------------|
| `src/state.h` | `Dart`, `State`, and compact `PackedState` definitions |
| `src/vertex_catalog.*` | Static catalogue of 44 supported Euclidean vertex types |
| `src/solver.*` | Extension, polygon-cycle checks, forced propagation, and raw output |
| `src/disk_solver.*` | Versioned binary records, BFS fan-out, spill/reload, and disk workers |
| `src/pruner.*` | Canonical filtering, WL/BFL deduplication, and final rendering |
| `src/bfl.*` | Breadth-first canonical words and portable 256-bit hashes |
| `src/tes_store.*` | Chunked zstd/SQLite storage and extraction for HyperRogue TES |
| `src/mortier_geometry.*` | Exact Z4 development and translation-lattice derivation |
| `src/mortier_store.*` | Mortier codec, chunked SQLite storage, and JSON conversion |
| `src/zstd_stream.h` | Concatenated-frame zstd input and durable compression helpers |
| `src/main.cpp` | CLI, memory queue, progress reporting, and pipeline dispatch |

## State representation

`State` is a vector of darts plus one catalogue type per representative vertex:

```cpp
struct Dart {
    int rneig, lneig;
    int polygon_size;
    int mirro;
    int glue;
    bool is_mirror_edge;
};

struct State {
    std::vector<Dart> darts;
    std::vector<int> vertype;
};
```

Only `vertype` and the dart `glue` values vary between compact states. The
remaining dart fields are reconstructed from the vertex catalogue. A complete
state has no `glue == -1` entry.

## Search pipelines

Memory mode keeps packed partial states in a shared queue. Workers unpack a
batch, call `EuclideanSolver::extend_into`, write complete states into private
directories, and return packed partials in bulk. Complete-solution online dedup
is per worker and bounded; the final pruner performs global deduplication.

Disk mode first performs BFS fan-out into chunk files. Parallel workers claim
chunks, run DFS-like local queues, and spill bounded batches as zstd-compressed
frames when queues exceed the configured threshold. Search frontier/spill files
are internal formats and are independent of the selected final output format.

Forced unique-partner propagation is enabled. Partial-state canonical filtering
uses bitset alias refinement before candidates are queued. The old fingerprint
bucket partial deduplication path was removed because it was disabled and did
not improve tested runs.

## Binary state records

New records use binary format version 2:

```text
u8  marker = 0
u8  version = 2
u16 vertex_count, little endian
u16 dart_count, little endian
u8[vertex_count] catalogue types
i16[dart_count] glue indices
```

Readers continue to accept the legacy two-`u8` header. Counts and indices are
validated before a state is rebuilt.

## Global pruning

The pruner reads text or binary raw streams in batches and evaluates canonical
labeling and graph hashes in parallel. The default path uses 1-WL refinement to
convergence and exact isomorphism checks on hash collisions. `--dedup bfl` uses
the canonical BFL word and a disk-backed exact word index. `--wl-dim 2` enables
the more expensive pair-colour refinement.

Accepted states are converted once into a structured polygon description. TES
and Mortier renderers share this description, including polygon repeats,
mirrored Conway pairings, and chirality decisions.

## Final formats

`--format tes` is the default. It stores HyperRogue documents in
`<output>/wl/tilings.sqlite3`, using zstd-compressed text chunks indexed by
document byte ranges. `--extract-tes` recreates individual files safely.

`--format mortier` stores canonical Mortier records under the same path but in
a distinct schema. Z4 points are signed integer coefficients in the ordered
basis `(1, exp(i*pi/6), exp(i*pi/3), i)`. Exact affine polygon development
derives translation generators; exact rank-two module reduction derives `T1`
and `T2`; vertices reduced modulo that lattice form `Seed`.

Mortier records use ZigZag varints, sorted/delta-encoded seeds, and 8 MiB zstd
chunks. The entry table stores `k`, seed count, and a 32-byte BFL stable ID.
Hash lanes are serialized little-endian, so IDs are platform-independent.
SQLite metadata versions both the schema and binary codec and marks completion
only after all records have been committed.

`--format raw` retains compressed pre-pruning solver streams while still
running global pruning for counts. It does not create a final geometry database.

## Conversion and validation

```sh
./eusolver --export-mortier-json DB --json-output database.json
./eusolver --export-mortier-json DB --json-output one.json --mortier-id 42
./eusolver --import-mortier-json database.json --mortier-database DB

python3 scripts/validate_mortier.py \
  --generated database.json \
  --reference ../mortier/data/database.json
```

JSON import accepts generated `kNN_<stable-id>` keys and legacy solver names
such as `eu_raw_4u_5d2_6h_1`. Legacy names are decoded against the vertex
catalogue, retained as aliases, and assigned deterministic IDs from alias plus
normalized geometry. `_failures` diagnostics are persisted and keep the
database incomplete. The validator matches
periodic point sets under translation-basis changes, origin shifts, rotations,
and reflections.

## Durability

Final databases are built as `tilings.sqlite3.tmp` and renamed only after a
successful finish. Chunk insertion is transactional. Raw inputs are removed
only after final publication unless `--keep-pruner-inputs` or `--format raw` is
selected. JSON conversion likewise writes a temporary output and protects the
source database from direct or symlink-aliased overwrite.

## Testing

`make test` runs C++ safety/codec/geometry tests, Python unit tests, count
regressions, and a memory/disk equivalence run. `TEST_MAX_K` expands the count
range. Supplying `BASELINE=/path/to/eusolver` also compares exact TES document
sets. `scripts/benchmark.py` compares partial counts and timings separately.

## Known limitations

- The catalogue omits `(4,8,8)`, so the solver reports 10 rather than 11
  one-uniform tilings.
- Mathematical completeness above the independently checked reference range is
  not proven solely by the generated database.
- Mortier exact development supports the regular polygon sizes represented by
  this catalogue: 3, 4, 6, and 12.
- Search and global deduplication are not resumable as one atomic operation;
  disk mode can resume frontier work, while pruning restarts from raw inputs.
