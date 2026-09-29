#pragma once
// Exact canonical form of a complete solution via colour refinement.
//
// Colours start as polygon sizes and are refined on the tuple
// (colour, colour∘R, colour∘L, colour∘M, colour∘G) with exact ranks (no
// hashing), so the result is the coarsest stable partition, i.e. dart
// bisimilarity.  This is the same relation that
// SolutionPruner::is_canonical_labeling computes with its alias bitsets:
//   * minimal == partition is discrete (every dart distinguished);
//   * for minimal solutions the final ranks are an isomorphism-invariant
//     labelling, so the relabelled relation table is an exact canonical code.
#include "state.h"
#include <array>
#include <cstdint>
#include <vector>

namespace canon {

struct Result {
    bool minimal = false;                 // pruner's is_canonical_labeling
    std::array<uint64_t, 2> hash{};       // 128-bit hash of the canonical code (minimal only)
};

// Structure-of-arrays view of a complete solution.
struct DartView {
    int n;
    const int *R, *L, *M, *G, *P;
    bool partial = false;   // allow free darts (G < 0), coloured by a sentinel
    const int* C = nullptr; // optional extra initial colour per dart (0..63)
};

// `code` (optional) receives the full canonical code for exact comparisons.
Result canonical_form(const State& st, std::vector<int32_t>* code = nullptr);
Result canonical_form(const DartView& v, std::vector<int32_t>* code = nullptr);

} // namespace canon
