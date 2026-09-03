#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "mortier_store.h"

bool operator==(const Z4Point& left, const Z4Point& right);
bool operator<(const Z4Point& left, const Z4Point& right);

namespace mortier_geometry {

bool operator!=(const Z4Point& left, const Z4Point& right);
Z4Point operator+(const Z4Point& left, const Z4Point& right);
Z4Point operator-(const Z4Point& left, const Z4Point& right);
Z4Point operator-(const Z4Point& value);
Z4Point operator*(int64_t scale, const Z4Point& value);

// Unit edge in one of the twelve directions, counter-clockwise from +x.
Z4Point direction_step(int direction);
std::pair<long double, long double> cartesian(const Z4Point& point);
bool cartesian_independent(const Z4Point& first, const Z4Point& second);

struct ConwayAdjacency {
    int tile = -1;
    int pattern_edge = -1;
    bool mirrored = false;
};

struct TilingDescription {
    // One regular polygon type per entry.
    std::vector<int> polygon_sides;
    // Number of repetitions of the physical edge pattern. Empty means all 1.
    std::vector<int> repeats;
    // physical_to_pattern[t][physical edge]. Empty rows are derived from repeats.
    std::vector<std::vector<int>> physical_to_pattern;
    // adjacency[t][pattern edge]. Every pattern edge must have one reciprocal mate.
    std::vector<std::vector<ConwayAdjacency>> adjacency;
};

// Parses forms such as "(0 1')[2](3@4 0)". Parentheses reverse the
// neighbour's directed entry edge; brackets preserve it.
std::vector<std::vector<ConwayAdjacency>> parse_conway_adjacency(
    const std::string& conway,
    const std::vector<std::vector<int>>& physical_to_pattern);

TilingDescription make_tiling_description(
    std::vector<int> polygon_sides,
    std::vector<int> repeats,
    const std::string& conway);

TilingDescription make_tiling_description(
    std::vector<int> polygon_sides,
    std::vector<std::vector<int>> physical_to_pattern,
    std::vector<std::vector<ConwayAdjacency>> adjacency);

// Validates and fills default repeats/mappings. Throws std::invalid_argument.
void validate_tiling_description(TilingDescription& description);

struct DevelopmentOptions {
    size_t max_tiles = 50000;
    size_t max_depth = 24;
    size_t max_quotient_tiles = 100000;
};

struct DevelopmentResult {
    MortierRecord record;
    size_t developed_tiles = 0;
    size_t quotient_tiles = 0;
    size_t translation_candidates = 0;
};

// Reduces a point into the half-open fundamental parallelogram [0,1)^2.
// The basis must be Cartesian rank two.
Z4Point reduce_mod_lattice(const Z4Point& point,
                           const Z4Point& translation1,
                           const Z4Point& translation2);

// Deterministically chooses signs/order among the eight equivalent bases.
void normalize_basis(Z4Point& translation1, Z4Point& translation2);

DevelopmentResult develop_exact_geometry(
    TilingDescription description,
    const DevelopmentOptions& options = DevelopmentOptions{});

// Checks basis rank, unique normalized seeds, reconstructed stars, and every
// face walk. This validates geometry represented by the record, not tile labels.
void validate_mortier_record(const MortierRecord& record);

// Limitations: only unit-edge regular 3-, 4-, 6-, and 12-gons embed in this
// 12-direction ring. The input must describe a connected, edge-to-edge,
// translation-periodic Euclidean tiling; disconnected types and tilings whose
// period is not exposed before DevelopmentOptions limits are rejected.

} // namespace mortier_geometry
