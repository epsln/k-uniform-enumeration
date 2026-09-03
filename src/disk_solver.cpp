#include "disk_solver.h"
#include "solver.h"
#include "pruner.h"
#include "vertex_catalog.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <zstd.h>

namespace fs = std::filesystem;
using namespace catalog;

// Spill compression level. Kept low/fast since spill_flush can fire very
// frequently (every ~100 iterations once the queue is over threshold) —
// this is on the hot path, unlike the once-in-a-while output compression.
static int g_spill_zstd_level = 3;

static void validate_packed_state(const std::vector<uint8_t>& vertype,
                                  const std::vector<int16_t>& glue,
                                  bool require_complete) {
    if (vertype.empty() || glue.empty())
        throw std::runtime_error("binary state has empty vertex or dart data");
    size_t expected = 0;
    for (uint8_t vt : vertype) {
        if (vt >= NUM_VERTEX_TYPES)
            throw std::runtime_error("binary state has invalid vertex type");
        expected += left_neighbors[vt].size();
    }
    if (expected != glue.size())
        throw std::runtime_error("binary state dart count does not match vertex types");
    for (size_t i = 0; i < glue.size(); ++i) {
        int g = glue[i];
        if (g == -1 && !require_complete) continue;
        if (g < 0 || (size_t)g >= glue.size())
            throw std::runtime_error("binary state has invalid glue index");
        if (glue[(size_t)g] != (int)i)
            throw std::runtime_error("binary state glue is not an involution");
    }
}

static void append_packed_record(std::string& out, const PackedState& ps) {
    validate_packed_state(ps.vertype, ps.glue, false);
    if (ps.vertype.size() > std::numeric_limits<uint16_t>::max()
            || ps.glue.size() > (size_t)std::numeric_limits<int16_t>::max())
        throw std::runtime_error("spill state dimensions are out of range");
    out.push_back(0);
    out.push_back((char)BINARY_STATE_VERSION);
    uint16_t nv = (uint16_t)ps.vertype.size(), ne = (uint16_t)ps.glue.size();
    out.push_back((char)(nv & 0xff)); out.push_back((char)(nv >> 8));
    out.push_back((char)(ne & 0xff)); out.push_back((char)(ne >> 8));
    for (uint8_t v : ps.vertype) out.push_back((char)v);
    for (int16_t g : ps.glue) {
        out.push_back((char)(g & 0xff));
        out.push_back((char)(((uint16_t)g >> 8) & 0xff));
    }
}

// Compress one spill batch (a vector of PackedState) into a single
// self-contained zstd frame and append it to the spill file as:
//   [i32 record_count][i32 uncompressed_size][i32 compressed_size][compressed bytes]
// Framing this way (one independent frame per batch, with explicit sizes)
// means spill_reload never has to seek *inside* a zstd stream — it just
// reads one whole frame at a time from a byte offset, exactly like the
// raw version did, except the offset now points into compressed data.
static void write_spill_batch(std::ofstream& sf, const std::vector<PackedState>& batch) {
    std::string plain;
    plain.reserve(batch.size() * 24);
    for (const auto& ps : batch) append_packed_record(plain, ps);

    size_t bound = ZSTD_compressBound(plain.size());
    std::string comp(bound, '\0');
    size_t csize = ZSTD_compress(comp.data(), bound, plain.data(), plain.size(),
                                  g_spill_zstd_level);
    if (ZSTD_isError(csize)) {
        std::cerr << "\nFATAL: zstd spill compression failed: "
                   << ZSTD_getErrorName(csize) << "\n";
        std::abort();
    }

    write_i32(sf, (int32_t)batch.size());
    write_i32(sf, (int32_t)plain.size());
    write_i32(sf, (int32_t)csize);
    sf.write(comp.data(), (std::streamsize)csize);
}

// Reads exactly one batch (frame) starting at the stream's current position
// and pushes its states to the front of queue, in the same order the raw
// version did. Returns false if there's no complete batch to read (EOF or
// truncated frame header).
static bool read_spill_batch(std::ifstream& sf, std::deque<PackedState>& queue) {
    if (sf.peek() == EOF) return false;
    int32_t count = read_i32(sf);
    int32_t usize = read_i32(sf);
    int32_t csize = read_i32(sf);
    if (sf.fail() || count <= 0 || usize <= 0 || csize <= 0) return false;

    std::string comp(csize, '\0');
    sf.read(comp.data(), csize);
    if (sf.gcount() != csize) return false;   // truncated frame — stop here

    std::string plain(usize, '\0');
    size_t dsize = ZSTD_decompress(plain.data(), usize, comp.data(), csize);
    if (ZSTD_isError(dsize) || (int32_t)dsize != usize) {
        std::cerr << "\nFATAL: zstd spill decompression failed\n";
        std::abort();
    }

    size_t pos = 0;
    for (int i = 0; i < count; ++i) {
        if (pos >= plain.size()) return false;
        uint16_t num = (uint8_t)plain[pos++], ne;
        if (num == 0) {
            if (plain.size() - pos < 5) return false;
            uint8_t version = (uint8_t)plain[pos++];
            if (version != BINARY_STATE_VERSION)
                throw std::runtime_error("unsupported spill state version");
            num = (uint16_t)(uint8_t)plain[pos] | ((uint16_t)(uint8_t)plain[pos + 1] << 8);
            ne = (uint16_t)(uint8_t)plain[pos + 2] | ((uint16_t)(uint8_t)plain[pos + 3] << 8);
            pos += 4;
        } else {
            if (pos >= plain.size()) return false;
            ne = (uint8_t)plain[pos++];
        }
        if (num == 0 || ne == 0 || ne > (uint16_t)std::numeric_limits<int16_t>::max()
                || plain.size() - pos < (size_t)num + (size_t)ne * 2)
            return false;
        PackedState ps;
        ps.vertype.resize(num);
        for (uint16_t j = 0; j < num; ++j) ps.vertype[j] = (uint8_t)plain[pos++];
        ps.glue.resize(ne);
        for (uint16_t j = 0; j < ne; ++j) {
            uint16_t lo = (uint8_t)plain[pos];
            uint16_t hi = (uint8_t)plain[pos + 1];
            pos += 2;
            ps.glue[j] = (int16_t)(lo | (hi << 8));
        }
        validate_packed_state(ps.vertype, ps.glue, false);
        queue.push_front(std::move(ps));
    }
    if (pos != plain.size()) return false;
    return true;
}

// Shared progress counters for disk mode ETA display (one slot per worker)
std::atomic<int64_t> g_disk_partials[MAX_WORKERS];
std::atomic<int64_t> g_disk_solutions[MAX_WORKERS];
std::atomic<int64_t> g_disk_queue[MAX_WORKERS];
std::atomic<int64_t> g_disk_spilled[MAX_WORKERS];
std::atomic<bool> g_disk_running{false};
bool g_compress_solutions = false;
int64_t g_compress_threshold = 256LL * 1024 * 1024;
static bool s_pdedup_shared = false;
void set_pdedup_shared(bool v) { s_pdedup_shared = v; }

// Append-compress a .bin file: compress the current tail into a zstd frame and
// concatenate it onto X.bin.zst (zstd streams concatenate, so zstd -d yields the
// full history), then remove the tail.  Never overwrites X.bin.zst, so earlier
// solutions are never lost even when the file is compressed many times mid-run.
static void append_compress(const std::string& path) {
    std::string cmd = "zstd -c -q \"" + path + "\" >> \"" + path + ".zst\" && rm \"" + path + "\"";
    std::system(cmd.c_str());
}

// =============================================================================
// Binary I/O
// =============================================================================
void write_state_bin(std::ostream& os, const State& s) {
    PackedState ps = EuclideanSolver::pack_state(s);
    validate_packed_state(ps.vertype, ps.glue, false);
    write_binary_record_header(os, ps.vertype.size(), ps.glue.size());
    for (uint8_t v : ps.vertype) write_u8(os, v);
    for (int16_t g : ps.glue) write_i16(os, g);
    if (!os) throw std::runtime_error("failed to write binary state");
}

PackedState read_packed_state_bin(std::istream& is) {
    uint16_t num, ne;
    read_binary_record_header(is, num, ne);
    PackedState p;
    p.vertype.resize(num);
    for (uint8_t& vt : p.vertype) vt = read_u8(is);
    p.glue.resize(ne);
    for (int16_t& glue : p.glue) glue = read_i16(is);
    if (is.fail()) throw std::runtime_error("truncated binary state payload");
    validate_packed_state(p.vertype, p.glue, false);
    return p;
}

State read_state_bin(std::istream& is) {
    return EuclideanSolver::unpack_state(read_packed_state_bin(is));
}

void write_states_bin(const std::string& path, const std::vector<State>& v) {
    std::string tmp = path + ".tmp";
    { std::ofstream f(tmp, std::ios::binary);
      write_i32(f, (int32_t)v.size());
      for (const auto& s : v) write_state_bin(f, s); }
    fs::rename(tmp, path);
}

void write_packed_states_bin(const std::string& path, const std::vector<PackedState>& v) {
    std::string tmp = path + ".tmp";
    { std::ofstream f(tmp, std::ios::binary);
      write_i32(f, (int32_t)v.size());
      for (const auto& ps : v) {
          validate_packed_state(ps.vertype, ps.glue, false);
          write_binary_record_header(f, ps.vertype.size(), ps.glue.size());
          for (uint8_t vt : ps.vertype) write_u8(f, vt);
          for (int16_t g : ps.glue)   write_i16(f, g);
      }
    }
    fs::rename(tmp, path);
}

std::vector<State> read_states_bin(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    int32_t n = read_i32(f);
    if (f.fail() || n < 0) throw std::runtime_error("invalid binary state file count");
    std::vector<State> v(n);
    for (auto& s : v) s = read_state_bin(f);
    if (f.peek() != EOF) throw std::runtime_error("trailing data in binary state file");
    return v;
}

std::vector<PackedState> read_packed_states_bin(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    int32_t n = read_i32(f);
    if (f.fail() || n < 0) throw std::runtime_error("invalid binary state file count");
    std::vector<PackedState> v(n);
    for (auto& s : v) s = read_packed_state_bin(f);
    if (f.peek() != EOF) throw std::runtime_error("trailing data in binary state file");
    return v;
}

// =============================================================================
// BFS fan-out
// =============================================================================
std::vector<PackedState> bfs_fanout(int target, int max_polygons,
                                    std::vector<State>* early_states) {
    std::vector<PackedState> frontier;
    for (int vt = 0; vt < NUM_VERTEX_TYPES; ++vt)
        frontier.push_back(EuclideanSolver::pack_state(EuclideanSolver::make_initial(vt)));

    std::vector<State> early;
    size_t head = 0;

    while ((int)(frontier.size() - head) < target && head < frontier.size()) {
        State st = EuclideanSolver::unpack_state(frontier[head++]);
        EuclideanSolver::extend_into(st, [&](State&& cand) {
            bool done = true;
            for (const auto& d : cand.darts) if (d.glue == -1) { done = false; break; }
            if (done) {
                early.push_back(std::move(cand));
            }
            else frontier.push_back(EuclideanSolver::pack_state(cand));
        }, max_polygons);
    }

    if (head > 0) frontier.erase(frontier.begin(), frontier.begin() + head);
    if (early_states) *early_states = std::move(early);

    return frontier;
}

// =============================================================================
// DFS worker (disk-based)
// =============================================================================
DiskSolverStats disk_solver_worker(
    const std::vector<std::string>& chunk_paths,
    std::atomic<int>& next_chunk,
    const std::string& out_dir,
    int max_polygons,
    int spill_threshold,
    int64_t sol_dedup_cap,
    int worker_id)
{
    if (worker_id < 0 || worker_id >= MAX_WORKERS)
        throw std::invalid_argument("worker_id exceeds MAX_WORKERS");
    fs::create_directories(out_dir);
    DiskSolverStats stats;

    std::deque<PackedState> queue;

    std::string spill_path = out_dir + "/spill.zst";
    if (fs::exists(spill_path)) fs::remove(spill_path);
    bool spill_has_data = false;
    int64_t spill_read_pos = 0;

    // ETA model bookkeeping (per worker)
    int64_t spilled = 0;         // states currently on disk in spill.zst
    int report_counter = 0;

    // Solution output tracking (same as EuclideanSolver)
    static thread_local std::mutex mu;
    static thread_local std::map<std::string,int> run_totals;
    static thread_local std::map<std::string,std::string> solution_files;
    static thread_local HistogramMap vertex_combos;
    OnlineDedup local_dedup(sol_dedup_cap); // per-worker, capped + sliding-window

    auto spill_flush = [&]() {
        int fc = (int)queue.size() / 2;
        if (fc == 0) return;
        std::vector<PackedState> batch(queue.begin(), queue.begin() + fc);
        std::ofstream sf(spill_path,
            std::ios::binary | (spill_has_data ? std::ios::app : std::ios::out));
        write_spill_batch(sf, batch);
        sf.close();
        if (!sf) {  // write failed (e.g. disk full) — do NOT silently drop states
            std::cerr << "\nFATAL: spill write to " << spill_path
                      << " failed (disk full?). Aborting.\n";
            std::abort();
        }
        queue.erase(queue.begin(), queue.begin() + fc);
        spilled += fc;
        spill_has_data = true;
    };

    auto spill_reload = [&]() -> bool {
        std::ifstream sf(spill_path, std::ios::binary);
        if (!sf) return false;
        sf.seekg(spill_read_pos);
        size_t before = queue.size();
        if (!read_spill_batch(sf, queue)) return false;
        spilled -= (int64_t)(queue.size() - before);
        spill_read_pos = (int64_t)sf.tellg();
        return true;
    };

    auto pull_chunk = [&]() -> bool {
        int ci = next_chunk.fetch_add(1);
        if (ci >= (int)chunk_paths.size()) return false;
        auto v = read_packed_states_bin(chunk_paths[ci]);
        for (auto& s : v) queue.push_back(std::move(s));
        return true;
    };

    // Pre-load first chunk
    pull_chunk();

    int spill_check_counter = 0;
    int compress_check_counter = 0;

    while (true) {
        if (queue.empty()) {
            if (spill_has_data && spill_reload()) continue;
            if (!pull_chunk()) break;
            continue;
        }
        State st = EuclideanSolver::unpack_state(queue.front()); queue.pop_front();
        ++stats.partials_checked;

        // Check if state is already complete (e.g. early solution from fan-out)
        bool already_done = true;
        for (const auto& d : st.darts) if (d.glue == -1) { already_done = false; break; }
        if (already_done) {
            if (local_dedup.check_and_remember(st)) continue;
            ++stats.solutions_found;
            EuclideanSolver::write_solution_static(st, out_dir, mu,
                run_totals, solution_files, vertex_combos);
            continue;
        }

        EuclideanSolver::extend_into(st, [&](State&& cand) {
            bool done = true;
            for (const auto& d : cand.darts) if (d.glue == -1) { done = false; break; }
            if (done) {
                if (local_dedup.check_and_remember(cand)) return;
                ++stats.solutions_found;
                EuclideanSolver::write_solution_static(cand, out_dir, mu,
                        run_totals, solution_files, vertex_combos);
            } else {
                queue.push_back(EuclideanSolver::pack_state(cand));
            }
        }, max_polygons);

        if (++spill_check_counter >= 100) {
            spill_check_counter = 0;
            if ((int)queue.size() > spill_threshold) spill_flush();
        }

        // Mid-run compression: append-compress any oversized .bin so disk stays
        // bounded.  X.bin is just the uncompressed tail; append_compress never
        // overwrites X.bin.zst, so earlier solutions are preserved.
        if (g_compress_solutions && (++compress_check_counter >= 500)) {
            compress_check_counter = 0;
            for (const auto& entry : fs::directory_iterator(out_dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".bin"
                    && (int64_t)entry.file_size() >= g_compress_threshold)
                    append_compress(entry.path().string());
            }
        }

        if (++report_counter >= 50) {
            report_counter = 0;
            g_disk_partials[worker_id].store(stats.partials_checked, std::memory_order_relaxed);
            g_disk_solutions[worker_id].store(stats.solutions_found, std::memory_order_relaxed);
            g_disk_queue[worker_id].store((int64_t)queue.size(), std::memory_order_relaxed);
            g_disk_spilled[worker_id].store(spilled, std::memory_order_relaxed);
        }
    }

    if (spill_has_data) fs::remove(spill_path);

    // Compress any remaining uncompressed tails once the worker has finished writing.
    if (g_compress_solutions) {
        for (const auto& entry : fs::directory_iterator(out_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".bin")
                append_compress(entry.path().string());
        }
    }

    g_disk_partials[worker_id].store(stats.partials_checked, std::memory_order_relaxed);
    g_disk_solutions[worker_id].store(stats.solutions_found, std::memory_order_relaxed);
    g_disk_queue[worker_id].store(0, std::memory_order_relaxed);
    g_disk_spilled[worker_id].store(0, std::memory_order_relaxed);

    return stats;
}
