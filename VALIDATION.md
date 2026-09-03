# Release 1.0.0 Validation

Validation was performed on Linux/x86-64 before creating the `1.0.0` tag.

## Inputs

| Artifact | SHA-256 |
|----------|---------|
| Pre-Mortier `dev` baseline executable | `3fccb2065f42fb2a4fbc8cb9dc00e001869d0f2534db95b4078b37fbc9414b10` |
| `../mortier/data/database.json` | `9612cdb1b1633a6c04dbf5edff88e2f85e2b114b448761a876e5a6f0bebb0a9f` |
| `tilings_k20.sqlite3` | `82bb04f2b739e0070d55f2d189bf57b03db280e1808f9acb60faee09db954075` |

The baseline executable was built from local `dev` commit `be6a506` before the
release cleanup. The external Mortier catalogue and large result database are
not included in the Git tree.

## Commands And Results

```sh
make test BASELINE=/tmp/opencode/kue-phase12-dev/eusolver TEST_MAX_K=2
```

- Reference/generated counts: `k=1: 10`, `k=2: 20`.
- Exact baseline canonical TES set: 30 matching documents.
- Memory/disk exact canonical set at `k=1`: 10 matching documents.
- C++ codec, exact geometry, database, JSON, and safety tests passed.
- Eight Python unit tests passed.

```sh
python3 scripts/benchmark.py \
  --baseline /tmp/opencode/kue-phase12-dev/eusolver \
  --candidate ./eusolver --max-k 2 --repeats 2
```

- Both executables processed 1,105 partials per sample.
- Median solver and pruner ratios were 1.000x.
- Median end-to-end ratio was 1.062x over these short samples.

```sh
python3 scripts/validate_mortier.py \
  --generated /tmp/opencode/mortier-final-k2/database.json \
  --reference ../mortier/data/database.json --max-k 2
```

- All 30 generated records matched exactly one ordinary Galebach record.
- No generated record was unmatched or ambiguous.
- Singular reference record `t1002` was reported and skipped.

## Large Database Checks

`PRAGMA integrity_check` returned `ok`, `PRAGMA foreign_key_check` returned no
rows, all 6,523 chunks had consistent counts and byte ranges, and the database
contained 35,831,099 canonical entries covering every `k` from 1 through 20.
The database has no embedded source commit or completion manifest, so these
checks establish internal integrity, not independent mathematical completeness.
