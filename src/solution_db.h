#pragma once
// Raw solver output in SQLite (disk mode).
//
// Every writer (disk worker, fan-out) owns one database file
// OUTPUT/_worker_<name>/solutions.sqlite3, so writers never contend for a lock and
// each holds a single descriptor regardless of how many combos it produces.
// Solutions are buffered per combo and stored as zstd-compressed batches of
// binary records (the .bin record format), keeping ~30 bytes per solution.
// Layout 1 batches (current) start with a little-endian u32 record count n
// and n bytes giving each record's hash bucket -- the top
// SOLUTION_BUCKET_BITS of its canonical hash -- followed by the records in
// solver order (which keeps them compressible). The `bucket_counts` column
// holds SOLUTION_BUCKETS u32 per-bucket record counts, so the pruner can plan
// and dedup one hash range at a time. Layout 0 batches (older databases) are
// plain record sequences.
// Finished frontier chunks are recorded in the same database; a chunk is only
// recorded after every batch holding its solutions was committed, so --resume
// can skip it.
#include <array>
#include <cstdint>
#include <functional>
#include <istream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;
struct ZSTD_CCtx_s;
struct State;

constexpr int SOLUTION_BUCKET_BITS = 6;
constexpr int SOLUTION_BUCKETS = 1 << SOLUTION_BUCKET_BITS;
inline int solution_bucket(const std::array<uint64_t, 2>& h) {
    return (int)(h[0] >> (64 - SOLUTION_BUCKET_BITS));
}

class SolutionDbWriter {
public:
    // Opens (creating if needed) `path`; existing batches are kept (append).
    explicit SolutionDbWriter(const std::string& path, size_t batch_bytes = 1 << 20);
    ~SolutionDbWriter();
    SolutionDbWriter(const SolutionDbWriter&) = delete;
    SolutionDbWriter& operator=(const SolutionDbWriter&) = delete;

    // `hash` is the canonical-form hash of the (minimal) solution.
    void add(const State& solution, const std::array<uint64_t, 2>& hash);
    // Computes the canonical form; non-minimal solutions are dropped.
    void add(const State& solution);
    size_t buffered_bytes() const { return buffered_; }
    // Commits every buffered solution (one transaction, fsynced).
    void commit();
    // Records finished chunks (by file name) in one transaction.
    void mark_done(const std::vector<std::string>& chunks);
    void close();

private:
    struct Buffer {
        std::string records, buckets;
        std::array<uint32_t, SOLUTION_BUCKETS> counts{};
    };
    void write_batch(const std::string& combo, Buffer& buffer);

    sqlite3* db_ = nullptr;
    size_t batch_bytes_;
    size_t buffered_ = 0;
    std::map<std::string, Buffer> buffers_;
    std::string compressed_;
    ZSTD_CCtx_s* cctx_ = nullptr;
    bool in_txn_ = false;
};

// Paths of all solution databases in `dir`/_worker_*/ (sorted).
std::vector<std::string> list_solution_dbs(const std::string& dir);
// Union of finished chunk names over all solution databases in `dir`.
std::set<std::string> read_done_chunks_db(const std::string& dir);
// Combo -> solution count over the given databases.
std::map<std::string, int64_t> solution_db_combos(const std::vector<std::string>& dbs);
// Combo -> bucket (-1 = layout 0, not bucketed) -> solution count.
std::map<std::string, std::map<int, int64_t>>
solution_db_bucket_counts(const std::vector<std::string>& dbs);

// Decompressed batches of one combo across `dbs`, in storage order.
class SolutionDbReader {
public:
    SolutionDbReader(std::vector<std::string> dbs, std::string combo);
    ~SolutionDbReader();
    SolutionDbReader(const SolutionDbReader&) = delete;
    SolutionDbReader& operator=(const SolutionDbReader&) = delete;
    // False at the end. `layout` is the batch layout (see above); for layout
    // 1, `buckets` receives the bucket ids and `records_at` the byte offset of
    // the first record in `data`.
    bool next(std::vector<char>& data, int& layout, std::string& buckets, size_t& records_at);
private:
    void close_db();
    std::vector<std::string> dbs_;
    std::string combo_;
    size_t next_db_ = 0;
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};
// Removes a database and its -wal/-shm/-journal companions.
void remove_solution_db(const std::string& path);
