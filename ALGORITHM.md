# Algorithm: Euclidean k-uniform Tiling Enumeration

## Overview

A **k-uniform tiling** is a tiling of the Euclidean plane by regular polygons where
every vertex is incident to the same sequence of polygon sizes, and exactly k
distinct vertex types (orbits under the tiling's symmetry group) appear.

This program exhaustively enumerates all k-uniform tilings for a given k by
building vertex-configuration assignments edge by edge and then deduplicating
the results.

## Data model: flags and edge slots

The tiling is represented as a **combinatorial map** — a set of "flags" (or
"edge slots"), each describing one oriented edge of one polygon at one vertex.

Every flag has five structural relations:

```
                  polygon to its right
                  ┌─────────┐
                  │  size=p │
   mirro[i] ◄─────┤   ╲     │
   (mirror)       │    ╲    │
                  │     ╲   │
                  │      ╲  │
          lneig[i]│  i    ╲ │──► rneig[i]
    (counter-     │        ╲│   (clockwise
     clockwise)   │         │    neighbour)
                  └─────────┘
```


| Field | Meaning |
|-------|---------|
| `rneig[i]` | Next flag clockwise around the same vertex |
| `lneig[i]` | Previous flag clockwise (= next counter-clockwise) |
| `mirro[i]` | Mirror-image flag (involution α) |
| `polygon_size[i]` | Size (3, 4, 6, or 12) of the polygon on flag i's right |
| `glue[i]` | The flag glued to flag i across a shared polygon edge (−1 = free) |

`rneig` and `lneig` form a permutation σ (the clockwise walk around each
vertex). `mirro` is an involution α (reflection). `glue` is an involution β
(edge pairing). Together (σ, α, β) define a **combinatorial map** in the sense
of Tutte.

### Concatenation across vertices

A tiling with N vertices has N × (slots per vertex) flags in total. The five
arrays are concatenated from each vertex's per-vertex-type template. Slot
offset `k` belonging to vertex `tile` has indices computed as:

    offset = sum_{v < tile} slot_count(vertex_type[v])

## Vertex type catalogue

The program knows 44 Euclidean vertex types (Conway symbols). Each type is
defined by static tables in `vertex_catalog.cpp`:

| Table | Description |
|-------|-------------|
| `symbols` | Conway symbol, e.g. `"(3,12,12)A"` |
| `edge_label_templates` | Per-slot label strings, e.g. `["0","1","*0"]` |
| `left_neighbors` | Per-slot counter-clockwise neighbour indices |
| `right_neighbors` | Per-slot clockwise neighbour indices |
| `mirrors` | Per-slot mirror-image slot indices |
| `polygon_sizes` | Per-slot polygon sizes (3, 4, 6, or 12) |
| `codes` | Short codes like `"3a"` for file naming |

### Symmetry reduction

Many vertex types have internal symmetry, so not all slots need to be tried
as attachment points when adding a new vertex. The `attachment_limit(vertex_type)`
function returns the canonical number of slots to try (e.g. 1 for `(4,4,4,4)F`,
3 for `(3,12,12)F`, 8 for `(3,3,4,12)`), preventing generation of duplicate
solutions that are trivial relabelings.

## Polygon ring walk

A polygon is traced by following flags around its perimeter:

```
                  glue[b] = c            glue[d] = e
gap across edge ──►   ◄── gap across edge ──►
         ┌────────────┬────────────┐
         │  vertex 0  │  vertex 1  │
         │  a──►b     │  c──►d     │
         │  polygon P │  polygon P │
         └────────────┴────────────┘
```

Starting at flag `a` (on vertex 0), the polygon P is traced as:

    a → rneig[a] = b → glue[b] = c → rneig[c] = d → glue[d] = e → ...

Each segment `(L → rneig[L])` is one vertex's contribution to polygon P. The
step `glue[rneig[L]]` crosses the shared polygon edge to the next vertex.

The polygon is **open** when `glue[rneig[L]] == -1` (an unglued edge).

The polygon is **closed** when tracing returns to the starting flag.

### Segment counting

The polygon starts at flag `L0`, traces rneig[L0], crosses glue[rneig[L0]],
and repeats. The **segment count** is the number of `L → rneig[L]` steps taken.
For a polygon of nominal size `S`, the slack is:

    slack = S - segment_count

A valid closed polygon satisfies `S % segment_count == 0` (the polygon may
close after a fraction of its full circuit, representing a sub-multiple).

## Solver algorithm

### Initialization

44 initial states are created, each containing a single vertex of one of the
44 catalogue types with all glue entries set to −1 (fully unglued).

### Extension step (`extend_into`)

For a given partial state:

1. **Find the tightest free edge** via `analyze_cycles`. Walk every polygon
   ring and identify the open ring with the smallest slack (fewest remaining
   edge slots before the polygon would exceed its nominal size).

2. **Normalize**: if the tightest edge's label starts with `*`, replace it
   with its mirror image (we always glue from the non-starred side).

3. **Determine mirror type**: `mirrored = (mirro[ff] == ff)`. Self-mirrored
   edges can only pair with other self-mirrored edges.

4. **Try gluings**:

   a) **Pair with existing free edge**: for every free flag `i` in the state
      whose mirror type matches, set `glue[ff] = i` and `glue[i] = ff` (and
      their mirrors). Validate with `check_partial`. If the result has no −1
      entries, it's a complete solution. Otherwise push to the queue.

   b) **Attach new vertex**: if fewer than `max_polygons` vertices exist, for
      each vertex type from `vertex_types[0]` upward, create an extended state
      with the new vertex's slots appended. Only try `attachment_limit(type)`
      many slots. Glue `ff` to each matching slot, validate, and handle
      complete/partial results.

### Validity check (`check_partial`)

For every flag `i`, walk the polygon ring starting at `i`. The walk must
satisfy:

1. All polygon sizes along the ring must be equal.
2. The segment count must not exceed the polygon's nominal size.
3. If the ring closes back at `i`, the nominal size must divide the segment
   count evenly.

### Partial canonical filtering

Before a partial state is queued, bitset alias refinement checks whether its
current flag labeling is canonical. A non-canonical labeling is discarded
because another construction order produces its canonical representative.
Forced propagation also closes an edge when only one compatible partner is
possible. Complete solutions receive an additional bounded per-worker hash
deduplication pass before being written; global deduplication remains the
pruner's responsibility.

## Pruning pipeline

The solver produces **raw solutions** — complete gluings with a specific choice
of vertex/flag labels. Multiple labelings of the same geometric tiling exist.
The pruner collapses equivalent labelings to produce **unique tilings**.

### Phase 1: canonical labeling (`is_canonical_labeling`)

For a complete state, iteratively narrow the set of slots each slot could be
an automorphic image of. Constraints: matching polygon size, mirror pairing,
glue pairing, rneig/lneig adjacency. If every slot ends up uniquely matched to
itself, this labeling is canonical and should be kept.

### Phase 2: WL hash deduplication

**Weisfeiler-Lehman (1-WL) colour refinement** over the flag-slot graph:

1. Initial node colour = polygon size.
2. Until convergence (or the configured iteration cap), replace each node's colour with:

       FNV(colour, sort{FNV("R", rneig_colour),
                         FNV("L", lneig_colour),
                         FNV("M", mirro_colour),
                         FNV("G", glue_colour)})

3. Final hash = sorted multiset of all node colours.

The WL hash is a strong invariant: isomorphic tilings have identical hashes,
and non-isomorphic tilings almost never collide. The pruner groups solutions
by WL hash and only falls back to the full isomorphism check on collisions.

### Phase 3: isomorphism fallback (`solutions_match`)

Alias refinement on the combined 2n-slot graph of two complete states. If every
slot's alias set is singletons, the states are not isomorphic. Otherwise a
valid bijection exists and one is a duplicate.

### Output

For each accepted unique tiling, the pruner writes:

- **Raw**: retained compressed text or versioned binary solver states.
- **TES**: canonical HyperRogue documents packed into zstd-compressed SQLite
  chunks, with optional extraction to individual `.tes` files.
- **Mortier**: canonical exact Z4 translation vectors and seed sets packed into
  a versioned zstd/SQLite database. Translation periods are derived by exact
  affine development of the assembled polygon adjacency.

All modes still run global pruning and report canonical counts. TES and Mortier
databases use different schemas despite sharing the conventional output path
`<output>/wl/tilings.sqlite3`.

## Conway symbols

The gluing of a tiling is serialized as Conway notation. A pairing of two
slot labels is written as:

    (label1 label2)   — normal pairing
    [label1 label2]   — exactly one side is mirrored

Slot labels are of the form `"0"`, `"*2"`, `"1''"`, `"2@5"`:

| Component | Meaning |
|-----------|---------|
| `*` prefix | Mirrored edge |
| Digit | Slot number within vertex |
| `'` suffixes | Tile number 0-3 (one apostrophe per tile) |
| `@N` suffix | Tile number > 3 |

The assembled tile-adjacency Conway symbol (the one in the `.tes` file)
describes which tile edges meet, using parentheses for normal edges and
brackets for mirror edges.

## Generated counts

Counts in the checked local `k<=20` result database (unique after
deduplication, excluding the `(4,8,8)` vertex type not in the catalogue).
Counts through `k=8` agree with existing references; higher values are solver
results rather than an independent proof:

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
| 9 | 5,959 |
| 10 | 11,866 |
| 11 | 24,459 |
| 12 | 49,793 |
| 13 | 103,080 |
| 14 | 212,630 |
| 15 | 445,289 |
| 16 | 933,636 |
| 17 | 1,972,148 |
| 18 | 4,177,505 |
| 19 | 8,896,553 |
| 20 | 18,992,613 |
