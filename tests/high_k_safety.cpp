#include "disk_solver.h"
#include "pruner.h"
#include "solver.h"
#include "vertex_catalog.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

bool g_propagate = true;
bool g_binary_solutions = false;
bool g_no_spill = false;

using namespace catalog;

static State large_state(bool complete) {
    int largest_type = 0;
    for (int vt = 1; vt < NUM_VERTEX_TYPES; ++vt)
        if (left_neighbors[vt].size() > left_neighbors[largest_type].size())
            largest_type = vt;

    std::vector<int> vertex_types;
    while (vertex_types.size() * left_neighbors[largest_type].size() <= 256)
        vertex_types.push_back(largest_type);

    size_t darts = vertex_types.size() * left_neighbors[largest_type].size();
    std::vector<int> glue(darts, -1);
    if (complete)
        for (size_t i = 0; i < darts; ++i) glue[i] = (int)i;
    return EuclideanSolver::rebuild_from_vertype_glue(vertex_types, glue);
}

static void test_large_canonical_refinement() {
    State partial = large_state(false);
    assert(partial.darts.size() > 256);
    (void)EuclideanSolver::is_canonical_partial(partial);

    State complete = large_state(true);
    (void)SolutionPruner::is_canonical_labeling(complete);
}

static void test_a2_attachment_orbits() {
    int a2 = -1;
    for (int vt = 0; vt < NUM_VERTEX_TYPES; ++vt) {
        if (symbols[vt] == "(4,4,4,4)A2") {
            a2 = vt;
            break;
        }
    }
    assert(a2 >= 0);
    assert(attachment_limit(a2) == 2);
    assert(mirrors[a2][0] == 1);
    assert(mirrors[a2][1] == 0);
}

static void test_v2_round_trip() {
    State original = large_state(false);
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    write_state_bin(stream, original);
    stream.seekg(0);
    State decoded = read_state_bin(stream);
    assert(decoded.vertype == original.vertype);
    assert(decoded.darts.size() == original.darts.size());
    for (size_t i = 0; i < decoded.darts.size(); ++i)
        assert(decoded.darts[i].glue == original.darts[i].glue);
}

static void test_v2_large_vertex_count() {
    std::vector<int> vertex_types(257, 0);
    size_t darts = vertex_types.size() * left_neighbors[0].size();
    State original = EuclideanSolver::rebuild_from_vertype_glue(
        vertex_types, std::vector<int>(darts, -1));
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    write_state_bin(stream, original);
    stream.seekg(0);
    State decoded = read_state_bin(stream);
    assert(decoded.vertype.size() == 257);
    assert(decoded.vertype == original.vertype);
}

static void test_truncated_v2_header() {
    std::string bytes{"\0\2\1", 3};
    std::stringstream stream(bytes, std::ios::in | std::ios::binary);
    bool rejected = false;
    try {
        (void)read_state_bin(stream);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);
}

static void test_legacy_round_trip() {
    State original = EuclideanSolver::make_initial(0);
    PackedState packed = EuclideanSolver::pack_state(original);
    assert(packed.vertype.size() < 256 && packed.glue.size() < 256);

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    write_u8(stream, (uint8_t)packed.vertype.size());
    write_u8(stream, (uint8_t)packed.glue.size());
    for (uint8_t vt : packed.vertype) write_u8(stream, vt);
    for (int16_t glue : packed.glue) write_i16(stream, glue);
    stream.seekg(0);
    State decoded = read_state_bin(stream);
    assert(decoded.vertype == original.vertype);
    assert(decoded.darts.size() == original.darts.size());
}

static void test_invalid_packed_state() {
    PackedState invalid;
    invalid.vertype = {(uint8_t)NUM_VERTEX_TYPES};
    invalid.glue = {-1};
    bool rejected = false;
    try {
        (void)EuclideanSolver::unpack_state(invalid);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);
}

int main() {
    test_a2_attachment_orbits();
    test_large_canonical_refinement();
    test_v2_round_trip();
    test_v2_large_vertex_count();
    test_truncated_v2_header();
    test_legacy_round_trip();
    test_invalid_packed_state();
    std::cout << "high-k safety tests passed\n";
    return 0;
}
