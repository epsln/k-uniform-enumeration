#pragma once
#include "state.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// BFL canonical signature per Gosselin, Solnon, Damiand (TCS 2011).
//
// Algorithm 1: Breadth First Labelling — BFS from a starting dart with
// fixed β-function traversal order (no sorting).  For our 3D combinatorial
// map: β₀=σ⁻¹ (lneig), β₁=σ (rneig), β₂=α (glue), β₃=μ (mirro).
// ε (unglued) maps to label 0.
//
// Word Signature per Definition 9: try all starting darts, compute
// W_BFL(M, d) for each, return the lexicographically smallest word.
// Theorem 1: two connected maps are isomorphic ↔ they share a word.

// Compute BFL label array for a state, starting from `start_dart`.
// Returns label[dart_index] = BFS discovery order (0 = ε, 1 = start, ...).
// Unreachable darts get label -1 (can happen for disconnected partial states).
std::vector<int> bfl_labels(const State& st, int start_dart);

// Build the word W(M, l) from a BFL label assignment.
// For each label k=1..n, write: polygon_size, then l(β₁), l(β₂), l(β₃).
// The polygon_size annotation ensures geometric differences are captured.
std::vector<int> bfl_build_word(const State& st, const std::vector<int>& label);

// Word Signature: try all starting darts,
// compute W_BFL, return the lexicographically smallest.
std::vector<int> bfl_word_signature(const State& st);

// Compute W_BFL(M, start) for a single starting dart (for Set Tree checking).
std::vector<int> bfl_word_from_start(const State& st, int start_dart);

// Convenience: Word Signature hashed to 32-byte string for dedup keys.
std::string bfl_canonical_hash(const State& st);

// Word Signature hashed to a 32-byte (256-bit) inline key — no heap allocation,
// so it can be stored directly inside an unordered_set node.
std::array<uint64_t, 4> bfl_canonical_hash128(const State& st);
