# eusolver 1.0.0

Initial stable release of the C++ k-uniform Euclidean tiling enumerator.

## Highlights

- Parallel in-memory enumeration and disk-backed search for high-k runs.
- Forced edge propagation and canonical partial-state filtering.
- Global canonical pruning with selectable WL or exact BFL-word deduplication.
- Versioned compact binary state records with legacy record reading.
- HyperRogue TES documents stored in zstd-compressed SQLite chunks.
- Exact Mortier Z4 export with derived translation lattices and seed sets.
- Versioned, compressed Mortier SQLite storage with stable 256-bit identifiers.
- Selective SQLite-to-JSON export and generated JSON-to-SQLite conversion.
- Exact Galebach catalogue matcher under lattice basis changes, translations,
  rotations, and reflections.

## Validation

- Expected solver counts pass through `k=2` in the release test run.
- Memory and disk modes produce the same canonical `k=1` TES set.
- All 30 generated tilings through `k=2` match exactly one ordinary Galebach
  catalogue record; Mortier's singular `t1002` record is reported separately.
- The existing `k<=20` result database passes SQLite integrity, foreign-key,
  chunk-boundary, and record-count consistency checks and contains 35,831,099
  canonical records.

## Compatibility

- Mortier SQLite schema version: 1.
- Mortier binary codec version: 1.
- Raw binary state record version: 2; legacy records remain readable.
- Stable BFL hash lanes are serialized little-endian.
- The previously accepted `--pdedup` no-op and redundant `--propagate` switch
  were removed; propagation remains enabled by default.

## Known Limitations

- `(4,8,8)` is absent from the vertex catalogue, yielding 10 instead of 11
  one-uniform tilings.
- Higher-k counts are generated results and are not independently proven by the
  database itself.
- Mortier development supports regular polygon sizes 3, 4, 6, and 12.
- There is no configured Git remote in this checkout, so this release consists
  of the local `1.0.0` annotated tag and these release notes.
