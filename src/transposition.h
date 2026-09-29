#pragma once
// Shared, bounded transposition table of explored partial states.
//
// The set of tilings below a search node depends only on the isomorphism
// class (type-preserving) of its partial state: every subtree search is
// exhaustive over completions, and the "attached type >= root type" rule
// equals "type >= minimum type present". So a node whose partial state is
// isomorphic to one already explored (or being explored) can be skipped.
//
// Keys are 128-bit hashes of exact canonical codes (only partials whose
// colour refinement is discrete have one). The table is lossy: entries may be
// evicted, which only reduces pruning.
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

class TranspositionTable {
public:
    explicit TranspositionTable(size_t megabytes);
    // Returns true if `h` was already present; otherwise records it.
    bool seen_or_insert(const std::array<uint64_t, 2>& h);
    size_t capacity() const { return slots_.size(); }
    uint64_t hits() const { return hits_.load(std::memory_order_relaxed); }
    uint64_t probes() const { return probes_.load(std::memory_order_relaxed); }

private:
    static constexpr int WAYS = 4;
    static constexpr int LOCKS = 4096;
    std::vector<std::array<uint64_t, 2>> slots_;   // buckets of WAYS entries
    std::vector<uint8_t> next_victim_;
    size_t bucket_mask_ = 0;
    std::unique_ptr<std::mutex[]> locks_;
    std::atomic<uint64_t> hits_{0}, probes_{0};
};
