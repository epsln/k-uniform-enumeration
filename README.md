# K-uniform tiling enumeration
This repository contains code for an exhaustive enumeration for all k-uniform tilings. This code is directly adapted from the original work of [Fulgura14](https://github.com/Fulgur14/k-uniform-solver), but reworked to use high performance CPP and some optimizations.

Some words of warning though: this is a work in progress. I made extensive use of coding agents for this program, so I cannot guarantee the exactitude of the program, or that it reproduces exactly the results of the original program. I am currently in the process of validating.

# Algorithm
The base algorithm is unchanged. This is basically an exhaustive combinatorial enumeration, where we combine local or partial solutions to create larger candidates for tilings. A periodic planar tiling has a finite number of tiles that can be projected onto another by isometry. Each tile has a finite number of edges, which can be paired together using [Conway Symbols](https://www.matematita.it/personali/index.php/the_conway_symbol?blog=7). An edge with symbol 0 of tile 0 will always be adjacent to edge 3 of tile 2, or edge 1 of tile 1 mirrored, etc. Not all combination of conway symbols will produce a valid tiling, but the inverse is true. 

# Deduplication
This method does have a drawback: it produces a large amount of duplicate tiling. The computational bottleneck is here. The original pruner would use a isometric check for all pairs of solutions, which turns out to be a costly O(n^2) solution with N being the number of tilings. This check in itself is also costly, being roughly O(N^2) with N the number of nodes in the graph representation of a tiling.

# Build & Run

```sh
make                                       # builds ./eusolver (C++17, requires libzstd and SQLite)
./eusolver --max-polygons 5 --workers 8 --output solutions
./eusolver --mode disk --max-polygons 10 --workers 8 --compress-solutions
```

The pruner stores all HyperRogue `.tes` documents in
`<output>/wl/tilings.sqlite3`. Documents are grouped into zstd-compressed
chunks, avoiding one filesystem inode and one compression process per tiling.
Extract all documents when individual files are needed:

```sh
./eusolver --extract-tes solutions/wl/tilings.sqlite3 --extract-output extracted
./eusolver --extract-tes solutions/wl/tilings.sqlite3 --extract-output one --tes-id 42
```

Extraction refuses to overwrite existing files. Starting a new pruner run in
an existing output directory replaces the previous `tilings.sqlite3`; it does
not resume an interrupted prune.

Run `./eusolver --help` for the full option list. See `CLAUDE.md` for the
architecture overview and `IMPLEMENTATION.md` for implementation details.
