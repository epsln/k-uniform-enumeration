# Explore: Prune vertex-type / edge candidates by polygon size at glue time

Branch: `feature/flag-size-pruning` (off `dev`).

## Goal

`extend_into` (src/solver.h:157-207) currently tries candidates with no
polygon-size awareness:

- **free-edge loop** (solver.h:172-185): glues `first_free` to *every* free
  edge `i` regardless of size.
- **new-vertex loop** (solver.h:186-206): tries all 44 vertex types × all slots
  up to `attachment_limit(gr)`, regardless of the glued slot's polygon size.

Many of these are trivially invalid and are only rejected later by
`check_partial` / `propagate_forced`. Add cheap size-based filters at the
glue point to cut the search tree.

## Invariants (confirmed against euclidean_tiling.py)

- A `glue` always pairs two flags lying on the **same polygon** (the two
  endpoints of one shared edge). Therefore
  `polygon_size[a] == polygon_size[b]` whenever `glue[a] = b`.
- `mirro[a]` lies on the **opposite side** of the edge (a different polygon,
  possibly a different size). `euclidean_tiling.py:780-783` logs
  `firstfree = ... between {polygon_size[first_free]} and {polygon_size[mirro[first_free]]}`.
- When `!mirrored`, the code *forces* `glue[mirro[first_free]] = mirro[i]`
  (solver.h:177-180, 195-198). That mirror pair is itself a glue, so it must
  also satisfy `polygon_size[mirro[first_free]] == polygon_size[mirro[i]]`.

Consequences:

| Filter | free-edge loop (`i` existing) | new-vertex loop (`i` in type `gr`) |
|---|---|---|
| 1. glue size | `darts[i].polygon_size == darts[first_free].polygon_size` | `polygon_sizes[gr][i - offset] == darts[first_free].polygon_size` |
| 2. mirror size (`!mirrored`) | `darts[mirro[i]].polygon_size == darts[mirro[first_free]].polygon_size` | `polygon_sizes[gr][mirrors[gr][i - offset]] == darts[mirro[first_free]].polygon_size` |

Sizes only take values {3,4,6,12} in the catalog, so these are cheap integer
comparisons.

## Plan

1. **Confirm invariants** by tracing the ring walk in `check_partial`
   (solver.cpp:144) and `_is_valid_partial` (euclidean_tiling.py:662). Verify
   the claim that `glue` connects same-size flags and `mirro` connects
   opposite-side flags with a known solution (e.g. (3,4,6,4) family).

2. **Implement Filter 1** (glue-size match) in both loops of `extend_into`.
   This is the "only try vertex types containing a polygon of the flag's size"
   pruning the request describes.

3. **Implement Filter 2** (mirror-size match) for the `!mirrored` branch. This
   is the "look at the polygon size of the neighbor" extension, since
   `mirro[first_free]`'s size must match the new vertex's mirror slot.

4. **Precompute candidate slots** (optional perf step): build a static table
   `(vertex_type, glue_size [, mirror_size]) -> list of local slots` restricted
   to `attachment_limit(gr)`. This lets the outer `for gr` skip types with zero
   matches and the inner loop iterate only matching slots.

5. **Verify correctness**: `make`, then run `./eusolver --max-polygons N` for
   N = 1..8 and confirm counts match the Reference Counts table in CLAUDE.md.

6. **Verify performance**: record the `partials, Q=` progress delta vs. a
   baseline run (same machine, same flags).

## Open questions / risks

- **Filter 2 over-pruning**: should be safe (the mirror pair is forced by the
  algorithm, so it is a necessary condition), but confirm empirically that no
  solutions are lost.
- **Low-k payoff**: the precomputed table adds indirection that may not pay at
  small k; profile before keeping it.
- **propagate_forced** (solver.cpp:203): already returns false on mismatched
  ring sizes; consider short-circuiting earlier with the same size check.
- **compare.py**: CLAUDE.md references `scripts/compare.py`, which is not in
  the tree. Recreate it or compare `eu_final_results.txt` / per-k totals
  manually for the merge gate.
- **k=1 baseline**: known missing (4,8,8) vertex type means k=1 is 10, not 11;
  keep that in mind when diffing counts.
