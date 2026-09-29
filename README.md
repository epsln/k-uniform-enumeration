# K-uniform tiling enumeration

`eusolver` exhaustively enumerates combinatorial candidates for k-uniform
edge-to-edge tilings of the Euclidean plane by regular polygons. It is adapted
from [Fulgura14's reference solver](https://github.com/Fulgur14/k-uniform-solver)
and reimplemented in C++17 with parallel search, disk-backed high-k operation,
global canonical deduplication, and TES/Mortier output.

Release 1.0.0 validates exact canonical TES output through `k=2` against the
pre-export baseline, memory/disk equivalence at `k=1`, and all generated
Mortier records through `k=2` against the Galebach catalogue. Higher-k database
counts are reproducible solver results, not an independent proof of mathematical
completeness. The vertex catalogue omits `(4,8,8)`, so `k=1` has 10 rather than
the literature value 11.

# Algorithm
The base algorithm is unchanged. This is basically an exhaustive combinatorial enumeration, where we combine local or partial solutions to create larger candidates for tilings. A periodic planar tiling has a finite number of tiles that can be projected onto another by isometry. Each tile has a finite number of edges, which can be paired together using [Conway Symbols](https://www.matematita.it/personali/index.php/the_conway_symbol?blog=7). An edge with symbol 0 of tile 0 will always be adjacent to edge 3 of tile 2, or edge 1 of tile 1 mirrored, etc. Not all combination of conway symbols will produce a valid tiling, but the inverse is true. 

# Deduplication
This method does have a drawback: it produces a large amount of duplicate tiling. The computational bottleneck is here. The original pruner would use a isometric check for all pairs of solutions, which turns out to be a costly O(n^2) solution with N being the number of tilings. This check in itself is also costly, being roughly O(N^2) with N the number of nodes in the graph representation of a tiling.

# Build And Run

```sh
make                                       # C++17; requires zstd, SQLite, and Boost headers
./eusolver --max-polygons 5 --workers 8 --output solutions
./eusolver --mode disk --max-polygons 10 --workers 8 --compress-solutions
```

Select the final representation with `--format raw|tes|mortier` (`tes` is the
default). `raw` retains the compressed pre-pruning solver state streams while
still running canonical pruning for counts. `mortier` derives an exact periodic `Z[exp(i*pi/6)]` vertex
representation and writes `<output>/wl/tilings.sqlite3`.

TES and Mortier databases deliberately use different schemas even though both
use the conventional path `<output>/wl/tilings.sqlite3`. Select the intended
format when generating or consuming a database.

The pruner stores all HyperRogue `.tes` documents in
`<output>/wl/tilings.sqlite3`. Documents are grouped into zstd-compressed
chunks, avoiding one filesystem inode and one compression process per tiling.
Extract all documents when individual files are needed:

```sh
./eusolver --extract-tes solutions/wl/tilings.sqlite3 --extract-output extracted
./eusolver --extract-tes solutions/wl/tilings.sqlite3 --extract-output one --tes-id 42
```

Extraction refuses to overwrite existing files. A pruner run builds a temporary
database and atomically replaces the previous `tilings.sqlite3` only after all
inputs succeed. It does not resume an interrupted prune.

## Mortier output

Mortier output uses a separate versioned SQLite schema. Normalized records hold
two translation vectors and a sorted seed set. Signed coordinates are ZigZag
varints, nearby seeds are delta encoded, and records are grouped into 8 MiB
zstd-compressed chunks. The entry table stores `k`, seed count, and a binary
stable ID derived from the canonical BFL representation, allowing indexed
access without loading or parsing the full collection.

```sh
./eusolver --max-polygons 6 --format mortier --output solutions

# Export all records, one numeric row, or one stable hash to Mortier JSON.
./eusolver --export-mortier-json solutions/wl/tilings.sqlite3 \
  --json-output database.json
./eusolver --export-mortier-json solutions/wl/tilings.sqlite3 \
  --json-output one.json --mortier-id 42
./eusolver --export-mortier-json solutions/wl/tilings.sqlite3 \
  --json-output one.json --mortier-stable-id HEX

# Convert generated JSON or Mortier's legacy eu_raw_*.json catalogue to SQLite.
./eusolver --import-mortier-json database.json \
  --mortier-database database.sqlite3

# Convert an existing TES database directly, resuming after interruption.
./eusolver --convert-tes-to-mortier tes.sqlite3 \
  --mortier-database mortier.sqlite3
./eusolver --convert-tes-to-mortier tes.sqlite3 \
  --mortier-database mortier.sqlite3 --resume
```

TES conversion writes `DEST.partial`, checkpoints each compressed output chunk,
and atomically renames it to `DEST` only after every source row succeeds. The
default `--conversion-errors fail` records the failing source ID and stops;
`continue` records failures, processes later rows, returns nonzero, and retains
the partial database. A resume retries every persisted failure, including
failures below the continue-mode watermark, and then processes rows after the
watermark. Successfully retried rows replace their saved failure records.

New Mortier databases use schema version 2. Readers remain compatible with
schema version 1 databases.

Conversion IDs use the documented `tes-source-v1` 32-byte source-qualified
policy, not BFL. The source qualifier is a logical SHA-256 over the TES schema,
compression mode, and every ordered entry field and document byte. Opening a
conversion source therefore performs one complete streaming fingerprint pass;
resume additionally requires the same canonical path and file size. Repacking
the same logical records into different SQLite/zstd chunks preserves the
fingerprint and IDs, while any record mutation changes them. An in-progress
conversion must resume from its original canonical path.

The JSON importer accepts generated keys of the form `kNN_<stable-id>` and
legacy solver keys such as `eu_raw_4u_5d2_6h_1`. For legacy keys it decodes `k`
from catalogue-code multiplicities, retains the original key as entry metadata,
and creates a deterministic 32-byte ID from the alias and normalized geometry.
A top-level `_failures` string map produced by Mortier's converter is persisted;
the imported database remains `complete=0` and the command returns nonzero. The
validated records remain queryable, and diagnostics are stored in
`import_failures`.
Galebach names such as `t1003` still require the separate equivalence matcher.

Exact development supports the regular polygon sizes represented by the solver
(3, 4, 6, and 12). It uses integer Z4 edge steps throughout and rejects an
inconsistent tiling, a singular period lattice, or a period not found within the
finite affine holonomy rather than using a depth-limited spatial search.

Validate an exported JSON catalogue against Mortier's Galebach data with:

```sh
python3 scripts/validate_mortier.py \
  --generated database.json \
  --reference ../mortier/data/database.json \
  --max-k 6
```

Run `./eusolver --help` for the full option list. See `CLAUDE.md` for the
architecture overview and `IMPLEMENTATION.md` for implementation details.

# Generated Counts

The following counts through `k=16` were regenerated after correcting the
`(4,4,4,4)A2` attachment-slot orbits and agree with the Tiling Atlas reference.
The `(4,8,8)` omission applies to this whole catalogue, so `k=1` remains 10
rather than the literature count of 11. The older `tilings_k20.sqlite3`
database predates the correction and is not an authoritative source for
`k=17` through `k=20`.

| k | Unique tilings |
|---:|---------------:|
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
| **Total** | **1,792,287** |

## Result Analysis

The canonical database can be summarized without loading all entries into memory:

```sh
python3 scripts/analyze_results.py tilings_k20.sqlite3 \
  --csv /tmp/k20_m_distribution.csv
```

The report includes the distribution by $m$-Archimedean class, vertex-configuration
multiplicity profiles, frequent base configurations, polygon-size signatures, and
growth ratios between successive values of $k$. The $m$ value is computed by
collapsing catalogue variants such as `(3,12,12)A` and `(3,12,12)F` to their base
configuration `(3,12,12)`.

# Regression Tests And Benchmarks

The quick test suite checks the repository's expected k=1 count (10 because the
catalogue omits `(4,8,8)`) and compares the exact canonical solution documents
from memory and disk mode:

```sh
make test
make test TEST_MAX_K=3
make test BASELINE=/path/to/baseline TEST_MAX_K=2
```

With `BASELINE`, the test hashes every canonical TES document in each pruner
SQLite database and compares the complete sets, not only their counts. This
comparison uses one worker by default because parallel runs may retain different
isomorphic representatives; `--exact-workers` can override it. Expected counts
for k=1 through k=20 are built into the harness; larger subsets can take a long
time. Run `python3 scripts/regression.py --help` to select counts, workers,
retained output directories, or to skip the separate mode-equivalence run.

The repeat-sample benchmark reports partial counts plus solver, pruner, and
end-to-end wall times. It is informational unless an explicit slowdown threshold
is supplied, and the Make target does not rebuild either executable:

```sh
make benchmark BASELINE=/path/to/baseline CANDIDATE=./eusolver
make benchmark BASELINE=/path/to/baseline BENCHMARK_REPEATS=5 \
  BENCHMARK_MAX_K=3 BENCHMARK_MAX_SLOWDOWN=1.10
```

Use `scripts/benchmark.py --mode disk` to measure the packed disk pipeline.
Additional solver arguments can follow `--`, for example
`-- --binary-solutions --fanout 5000 --chunks 32`.

Both harnesses use only the Python 3 standard library. They require the solver's
normal runtime `libzstd` library to inspect compressed canonical documents.
