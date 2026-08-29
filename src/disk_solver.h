#pragma once
#include "state.h"
#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

// Compact binary serialization of State for disk-based search.
// Format:  uint8 num | uint8 n_edges | num*uint8 vertype | n_edges*int16 glue
// Derived fields (rneig, lneig, mirro, polygon_size, label) are NOT stored —
// they are reconstructed from vertype on deserialization.

inline void write_u8(std::ostream& os, uint8_t v)  { os.write((char*)&v, 1); }
inline uint8_t read_u8(std::istream& is)            { uint8_t v; is.read((char*)&v, 1); return v; }
inline void write_i16(std::ostream& os, int16_t v)  { os.write((char*)&v, 2); }
inline int16_t read_i16(std::istream& is)           { int16_t v; is.read((char*)&v, 2); return v; }
inline void write_i32(std::ostream& os, int32_t v)  { os.write((char*)&v, 4); }
inline int32_t read_i32(std::istream& is)           { int32_t v; is.read((char*)&v, 4); return v; }

void write_state_bin(std::ostream& os, const State& s);
State read_state_bin(std::istream& is);

// Write vector with leading count
void write_states_bin(const std::string& path, const std::vector<State>& v);
void write_packed_states_bin(const std::string& path, const std::vector<PackedState>& v);
std::vector<State> read_states_bin(const std::string& path);

// Stream states from a file (no leading count), calling cb on each
template<class F>
int64_t stream_states_bin(const std::string& path, F&& cb) {
    std::ifstream f(path, std::ios::binary);
    int64_t count = 0;
    while (f.good() && f.peek() != EOF) {
        try { cb(read_state_bin(f)); ++count; }
        catch (...) { break; }
    }
    return count;
}

// DFS worker that pulls chunks from a shared atomic pool, extends,
// writes solutions to disk.  All workers compete via next_chunk; each
// worker pulls a new chunk whenever its local queue is empty and no
// spill data remains.
struct DiskSolverStats {
    int64_t partials_checked = 0;
    int64_t solutions_found = 0;
    int64_t partials_deduped = 0;
};

DiskSolverStats disk_solver_worker(
    const std::vector<std::string>& chunk_paths,
    std::atomic<int>& next_chunk,
    const std::string& out_dir,
    int max_polygons,
    int spill_threshold = 50000,
    int64_t sol_dedup_cap = 100000,
    int worker_id = 0);

// BFS fan-out: expand initial states until we have at least `target` frontier states.
// Returns the frontier states.  early_states receives completed solutions found during fan-out.
std::vector<State> bfs_fanout(int target, int max_polygons,
                               std::vector<State>* early_states = nullptr);

// Enable global shared partial dedup for disk workers
void set_pdedup_shared(bool v);

// Shared progress counters for ETA display (disk mode), one slot per worker.
constexpr int MAX_WORKERS = 64;
extern std::atomic<int64_t> g_disk_partials[MAX_WORKERS];
extern std::atomic<int64_t> g_disk_solutions[MAX_WORKERS];
extern std::atomic<int64_t> g_disk_queue[MAX_WORKERS];        // in-RAM queue size
extern std::atomic<int64_t> g_disk_spilled[MAX_WORKERS];      // states spilled to disk
extern std::atomic<bool> g_disk_running;
extern bool g_compress_solutions;
extern int64_t g_compress_threshold;  // bytes; .bin files above this size are compressed mid-run
