#pragma once
#include <cstdint>
#include <vector>
#include <string>

// A single flag (dart) of the combinatorial map: one oriented edge of one
// polygon at one vertex.  All five structural relations plus the mirror-edge
// flag live in ONE contiguous record so that ring walks hit one cache line
// and a State copy is a single memcpy.
struct Dart {
    int rneig;            // next flag clockwise around the vertex (sigma)
    int lneig;            // previous flag (sigma^-1)
    int polygon_size;     // size of polygon to the dart's right
    int mirro;            // mirror-image flag (involution beta)
    int glue;             // flag glued across the shared edge (-1 = free)
    int is_mirror_edge;   // 1 if this dart's label starts with '*'
};

struct State {
    std::vector<Dart> darts;   // one contiguous array of flags
    std::vector<int> vertype;  // vertex type index per vertex (length k)
};

// Compact queue representation: only glue + vertype (~25x smaller than full State).
// Derived fields (rneig, lneig, mirro, polygon_size, is_mirror_edge) are rebuilt on unpack.
struct PackedState {
    std::vector<uint8_t> vertype;
    std::vector<int16_t> glue;
};
