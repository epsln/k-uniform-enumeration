#pragma once
#include "state.h"
#include <atomic>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// Compact binary serialization of State for disk-based search.
// Version 2 record: 0 marker | uint8 version | uint16 num | uint16 n_edges |
//                   num*uint8 vertype | n_edges*int16 glue
// Legacy unversioned uint8-count records remain readable. New writes are v2.
// Derived fields (rneig, lneig, mirro, polygon_size, label) are NOT stored —
// they are reconstructed from vertype on deserialization.

inline void write_u8(std::ostream& os, uint8_t v)  { os.write((char*)&v, 1); }
inline uint8_t read_u8(std::istream& is)            { uint8_t v = 0; is.read((char*)&v, 1); return v; }
inline void write_u16(std::ostream& os, uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v & 0xff), (uint8_t)(v >> 8)};
    os.write((char*)b, 2);
}
inline uint16_t read_u16(std::istream& is) {
    uint8_t b[2] = {}; is.read((char*)b, 2);
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}
inline void write_i16(std::ostream& os, int16_t v)  { os.write((char*)&v, 2); }
inline int16_t read_i16(std::istream& is)           { int16_t v = 0; is.read((char*)&v, 2); return v; }
inline void write_i32(std::ostream& os, int32_t v)  { os.write((char*)&v, 4); }
inline int32_t read_i32(std::istream& is)           { int32_t v = 0; is.read((char*)&v, 4); return v; }

constexpr uint8_t BINARY_STATE_VERSION = 2;
inline void write_binary_record_header(std::ostream& os, size_t num, size_t ne) {
    if (num == 0 || num > std::numeric_limits<uint16_t>::max()
            || ne == 0 || ne > std::numeric_limits<int16_t>::max())
        throw std::runtime_error("binary state dimensions are out of range");
    write_u8(os, 0);
    write_u8(os, BINARY_STATE_VERSION);
    write_u16(os, (uint16_t)num);
    write_u16(os, (uint16_t)ne);
}
inline void read_binary_record_header(std::istream& is, uint16_t& num, uint16_t& ne) {
    uint8_t first = read_u8(is);
    if (is.fail()) throw std::runtime_error("truncated binary state header");
    if (first != 0) {
        num = first;
        ne = read_u8(is);
    } else {
        uint8_t version = read_u8(is);
        if (is.fail()) throw std::runtime_error("truncated binary state version");
        if (version != BINARY_STATE_VERSION)
            throw std::runtime_error("unsupported binary state version " + std::to_string(version));
        num = read_u16(is);
        ne = read_u16(is);
    }
    if (is.fail()) throw std::runtime_error("truncated binary state header");
    if (num == 0 || ne == 0 || ne > (uint16_t)std::numeric_limits<int16_t>::max())
        throw std::runtime_error("invalid binary state dimensions");
}

void write_state_bin(std::ostream& os, const State& s);
State read_state_bin(std::istream& is);
PackedState read_packed_state_bin(std::istream& is);

// Write vector with leading count
void write_states_bin(const std::string& path, const std::vector<State>& v);
void write_packed_states_bin(const std::string& path, const std::vector<PackedState>& v);
std::vector<State> read_states_bin(const std::string& path);
std::vector<PackedState> read_packed_states_bin(const std::string& path);

// Stream states from a file (no leading count), calling cb on each
template<class F>
int64_t stream_states_bin(const std::string& path, F&& cb) {
    std::ifstream f(path, std::ios::binary);
    int64_t count = 0;
    while (f.good() && f.peek() != EOF) {
        cb(read_state_bin(f));
        ++count;
    }
    if (f.bad()) throw std::runtime_error("failed while streaming binary states");
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
std::vector<PackedState> bfs_fanout(int target, int max_polygons,
                                    std::vector<State>* early_states = nullptr);

// Shared progress counters for ETA display (disk mode), one slot per worker.
constexpr int MAX_WORKERS = 64;
extern std::atomic<int64_t> g_disk_partials[MAX_WORKERS];
extern std::atomic<int64_t> g_disk_solutions[MAX_WORKERS];
extern std::atomic<int64_t> g_disk_queue[MAX_WORKERS];        // in-RAM queue size
extern std::atomic<int64_t> g_disk_spilled[MAX_WORKERS];      // states spilled to disk
extern std::atomic<bool> g_disk_running;
extern bool g_compress_solutions;
extern int64_t g_compress_threshold;  // bytes; .bin files above this size are compressed mid-run
