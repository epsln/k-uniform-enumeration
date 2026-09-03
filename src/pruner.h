#pragma once
#include "state.h"
#include "bfl.h"
#include "tes_store.h"
#include <array>
#include <cstdint>
#include <istream>
#include <map>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

void write_cycle_final(const State& st, std::ostream& out,
                       TesStore& tes_store, const std::string& combo,
                       const std::string& tes_filename,
                       const std::string& sig_raw);

class SolutionPruner {
public:
    explicit SolutionPruner(const std::string& output_dir);

    const std::map<int, int>& solutions_per_k() const { return solutions_per_k_; }

    static std::pair<bool, std::string> is_canonical_labeling(const State& s);
    static std::pair<bool, std::string> solutions_match(const State& a, const State& b);
    static State decode_solution(const std::string& vertex_line, const std::string& conway_line);

    struct SolutionRecord {
        std::string vertex_line;
        std::string signature_line;
        std::string tes_line;
        std::string conway_line;
        State state;
        std::string count_signature;
        int solution_index = 0;
    };

protected:
    std::string output_dir_;
    std::map<int, int> solutions_per_k_;

    static std::vector<int> make_glue(const std::string& conway,
                                       const std::vector<Dart>& darts,
                                       const std::vector<int>& vertype);
};

// Per-worker online solution dedup with a total-entry cap and sliding-window
// reset.  In BFL mode it stores only the 32-byte canonical hash (collisions are
// astronomically unlikely); in WL mode it stores full PackedStates keyed by the
// WL hash with the exact BFL-word backup on collision.
struct Hash32 {
    size_t operator()(const std::array<uint64_t, 4>& a) const {
        uint64_t h = 14695981039346656037ULL;
        for (uint64_t x : a) { h ^= x; h *= 1099511628211ULL; }
        return (size_t)h;
    }
};

class OnlineDedup {
public:
    explicit OnlineDedup(int64_t cap) : cap_(std::max<int64_t>(1, cap)) {}

    // Returns true if st is a duplicate (and does not remember it);
    // otherwise remembers st and returns false.
    bool check_and_remember(const State& st);

private:
    int64_t cap_;
    int64_t stored_ = 0;
    std::unordered_set<std::array<uint64_t, 4>, Hash32> seen_bfl_;
    std::unordered_map<std::string, std::vector<PackedState>> seen_wl_;
};

// Exact, disk-backed set of BFL canonical words.  A 64-bit word hash is kept in
// RAM (hash -> byte offsets); the full words live in an append-only disk file.
// Lookup reads the stored word back from disk and compares exactly, so the
// result is exact regardless of hash collisions.  RAM stays O(#unique).
class DiskWordIndex {
public:
    explicit DiskWordIndex(const std::string& path);
    ~DiskWordIndex() = default;

    bool contains(const std::vector<int>& word);
    void insert(const std::vector<int>& word);
    size_t size() const { return index_.size(); }

private:
    std::string path_;
    uint64_t next_offset_ = 0;
    std::unordered_map<uint64_t, std::vector<uint64_t>> index_;
};

// Streaming solution reader — reads one solution record at a time from file.
bool read_next_solution(std::istream& in, SolutionPruner::SolutionRecord& rec);

// Binary solution reader — reads packed state from .bin file and computes text fields.
bool read_next_solution_bin(std::istream& in, SolutionPruner::SolutionRecord& rec,
                             int& sol_idx);

// WL graph hashing — dim=1 (slot-level) or dim=2 (pair-level, stronger)
extern int g_wl_dim;
extern int g_wl_iters;
extern bool g_use_bfl;
extern bool g_no_iso_check;
extern bool g_compress_pruner_outputs;
extern bool g_keep_pruner_inputs;
extern bool g_profile_pruner;
std::string wl_hash  (const State& st, int iterations = 3);
std::string wl_hash_2(const State& st, int iterations = 3);
std::string wl_hash_partial(const State& st, int iterations = 0);
inline std::string wl_hash_dispatch(const State& st, int iters = 0) {
    if (g_use_bfl) return bfl_canonical_hash(st);
    int i = iters > 0 ? iters : g_wl_iters;
    return g_wl_dim == 2 ? wl_hash_2(st, i) : wl_hash(st, i);
}

class WLPruner : public SolutionPruner {
public:
    explicit WLPruner(const std::string& output_dir, int num_workers = 1);
    void run(const std::vector<std::string>& listfile_paths);

private:
    void process_file_wl(const std::string& path);
    std::map<std::string, std::vector<PackedState>> solutions_by_hash_;
    DiskWordIndex solutions_words_;  // used when g_use_bfl
    TesStore tes_store_;
    int num_workers_;

    struct CanonicalResult {
        bool is_canonical;
        std::string canon_log;
        std::string wl_hash;
        std::vector<int> bfl_word;
    };
    CanonicalResult compute_canonical_and_wl(const State& st) const;
};
