#pragma once
// Raw solver output in SQLite (disk mode).
//
// Every writer (disk worker, fan-out) owns one database file
// OUTPUT/_worker_<name>/solutions.sqlite3, so writers never contend for a lock and
// each holds a single descriptor regardless of how many combos it produces.
// Solutions are buffered per combo and stored as zstd-compressed batches of
// binary records (the .bin record format), keeping ~30 bytes per solution.
// Finished frontier chunks are recorded in the same database; a chunk is only
// recorded after every batch holding its solutions was committed, so --resume
// can skip it.
#include <cstdint>
#include <functional>
#include <istream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct sqlite3;
struct State;

class SolutionDbWriter {
public:
    // Opens (creating if needed) `path`; existing batches are kept (append).
    explicit SolutionDbWriter(const std::string& path, size_t batch_bytes = 1 << 20);
    ~SolutionDbWriter();
    SolutionDbWriter(const SolutionDbWriter&) = delete;
    SolutionDbWriter& operator=(const SolutionDbWriter&) = delete;

    void add(const State& solution);
    size_t buffered_bytes() const { return buffered_; }
    // Commits every buffered solution (one transaction, fsynced).
    void commit();
    // Records finished chunks (by file name) in one transaction.
    void mark_done(const std::vector<std::string>& chunks);
    void close();

private:
    void write_batch(const std::string& combo, std::string& records, int64_t count);

    sqlite3* db_ = nullptr;
    size_t batch_bytes_;
    size_t buffered_ = 0;
    struct Buffer { std::string records; int64_t count = 0; };
    std::map<std::string, Buffer> buffers_;
    std::string compressed_;
    bool in_txn_ = false;
};

// Paths of all solution databases in `dir`/_worker_*/ (sorted).
std::vector<std::string> list_solution_dbs(const std::string& dir);
// Union of finished chunk names over all solution databases in `dir`.
std::set<std::string> read_done_chunks_db(const std::string& dir);
// Combo -> solution count over the given databases.
std::map<std::string, int64_t> solution_db_combos(const std::vector<std::string>& dbs);
// Stream of the concatenated binary records of `combo` across `dbs`.
std::unique_ptr<std::istream> open_solution_db_stream(const std::vector<std::string>& dbs,
                                                       const std::string& combo);
// Removes a database and its -wal/-shm/-journal companions.
void remove_solution_db(const std::string& path);
