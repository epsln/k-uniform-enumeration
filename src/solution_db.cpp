#include "solution_db.h"

#include "canonical.h"
#include "disk_solver.h"
#include "solver.h"

#include <sqlite3.h>
#include <zstd.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

void check(sqlite3* db, int rc, const char* what) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW)
        throw std::runtime_error(std::string(what) + ": " + (db ? sqlite3_errmsg(db) : "sqlite"));
}

void exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = std::string("sqlite: ") + (err ? err : "error") + " in: " + sql;
        sqlite3_free(err);
        throw std::runtime_error(msg);
    }
}

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) { check(db, sqlite3_prepare_v2(db, sql, -1, &s, nullptr), sql); }
    ~Stmt() { sqlite3_finalize(s); }
};

sqlite3* open_db(const std::string& path, bool readonly) {
    sqlite3* db = nullptr;
    int flags = readonly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
        std::string msg = "cannot open solution database " + path + ": " + sqlite3_errmsg(db);
        sqlite3_close_v2(db);
        throw std::runtime_error(msg);
    }
    sqlite3_busy_timeout(db, 60000);
    return db;
}

bool has_layout_column(sqlite3* db) {
    Stmt info(db, "PRAGMA table_info(batches)");
    while (sqlite3_step(info.s) == SQLITE_ROW)
        if (std::string(reinterpret_cast<const char*>(sqlite3_column_text(info.s, 1))) == "layout")
            return true;
    return false;
}

}  // namespace

SolutionDbWriter::SolutionDbWriter(const std::string& path, size_t batch_bytes)
    : batch_bytes_(batch_bytes) {
    db_ = open_db(path, false);
    exec(db_, "PRAGMA journal_mode=WAL");
    // FULL: a chunk recorded done in one worker's database must never survive
    // a crash that loses solutions committed earlier in another's.
    exec(db_, "PRAGMA synchronous=FULL");
    exec(db_, "CREATE TABLE IF NOT EXISTS batches("
              "id INTEGER PRIMARY KEY, combo TEXT NOT NULL, count INTEGER NOT NULL,"
              " raw_size INTEGER NOT NULL, data BLOB NOT NULL)");
    // Databases from older runs (resumed) predate the layout columns.
    if (!has_layout_column(db_))
        exec(db_, "ALTER TABLE batches ADD COLUMN layout INTEGER NOT NULL DEFAULT 0;"
                  "ALTER TABLE batches ADD COLUMN bucket_counts BLOB");
    exec(db_, "CREATE INDEX IF NOT EXISTS batches_combo ON batches(combo, id)");
    exec(db_, "CREATE TABLE IF NOT EXISTS done_chunks(chunk TEXT PRIMARY KEY)");
}

SolutionDbWriter::~SolutionDbWriter() {
    try { close(); } catch (const std::exception&) {}
    // close() threw: do not leak the handle, but never commit partial state.
    if (db_) { sqlite3_close_v2(db_); db_ = nullptr; }
    if (cctx_) { ZSTD_freeCCtx(cctx_); cctx_ = nullptr; }
}

void SolutionDbWriter::add(const State& solution) {
    auto cf = canon::canonical_form(solution);
    if (cf.minimal) add(solution, cf.hash);
}

void SolutionDbWriter::add(const State& solution, const std::array<uint64_t, 2>& hash) {
    std::string combo = fine_name(solution);
    int bucket = solution_bucket(hash);
    Buffer& b = buffers_[combo];
    std::ostringstream os(std::ios::binary);
    write_state_bin(os, solution);
    std::string rec = os.str();
    b.records += rec;
    b.buckets.push_back((char)bucket);
    ++b.counts[bucket];
    buffered_ += rec.size() + 1;
    if (b.records.size() >= batch_bytes_) {
        if (!in_txn_) { exec(db_, "BEGIN IMMEDIATE"); in_txn_ = true; }
        buffered_ -= b.records.size() + b.buckets.size();
        write_batch(combo, b);
    }
}

namespace {
void put_u32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back((char)(uint8_t)(v >> (8 * i)));
}
uint32_t get_u32(const char* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)(uint8_t)p[i] << (8 * i);
    return v;
}
}  // namespace

void SolutionDbWriter::write_batch(const std::string& combo, Buffer& b) {
    if (b.buckets.empty()) return;
    const int64_t count = (int64_t)b.buckets.size();
    std::string payload, counts;
    payload.reserve(4 + b.buckets.size() + b.records.size());
    put_u32(payload, (uint32_t)count);
    payload += b.buckets;
    payload += b.records;
    for (uint32_t c : b.counts) put_u32(counts, c);
    if (!cctx_) cctx_ = ZSTD_createCCtx();
    compressed_.resize(ZSTD_compressBound(payload.size()));
    size_t n = ZSTD_compressCCtx(cctx_, compressed_.data(), compressed_.size(),
                                 payload.data(), payload.size(), 3);
    if (ZSTD_isError(n)) throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(n));
    Stmt st(db_, "INSERT INTO batches(combo, count, raw_size, data, layout, bucket_counts)"
                 " VALUES(?,?,?,?,1,?)");
    sqlite3_bind_text(st.s, 1, combo.data(), (int)combo.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, count);
    sqlite3_bind_int64(st.s, 3, (sqlite3_int64)payload.size());
    sqlite3_bind_blob(st.s, 4, compressed_.data(), (int)n, SQLITE_STATIC);
    sqlite3_bind_blob(st.s, 5, counts.data(), (int)counts.size(), SQLITE_TRANSIENT);
    check(db_, sqlite3_step(st.s), "insert solution batch");
    b.counts.fill(0);
    b.records.clear();
    b.buckets.clear();
}

void SolutionDbWriter::commit() {
    if (!db_) return;
    if (!in_txn_) { exec(db_, "BEGIN IMMEDIATE"); in_txn_ = true; }
    for (auto& [combo, b] : buffers_) write_batch(combo, b);
    buffered_ = 0;
    exec(db_, "COMMIT");
    in_txn_ = false;
}

void SolutionDbWriter::mark_done(const std::vector<std::string>& chunks) {
    if (chunks.empty()) return;
    if (!in_txn_) { exec(db_, "BEGIN IMMEDIATE"); in_txn_ = true; }
    Stmt st(db_, "INSERT OR IGNORE INTO done_chunks(chunk) VALUES(?)");
    for (const auto& c : chunks) {
        sqlite3_bind_text(st.s, 1, c.data(), (int)c.size(), SQLITE_TRANSIENT);
        check(db_, sqlite3_step(st.s), "record done chunk");
        sqlite3_reset(st.s);
    }
    exec(db_, "COMMIT");
    in_txn_ = false;
}

void SolutionDbWriter::close() {
    if (!db_) return;
    commit();
    if (cctx_) { ZSTD_freeCCtx(cctx_); cctx_ = nullptr; }
    exec(db_, "PRAGMA wal_checkpoint(TRUNCATE)");
    check(db_, sqlite3_close_v2(db_), "close solution database");
    db_ = nullptr;
}

std::vector<std::string> list_solution_dbs(const std::string& dir) {
    std::vector<std::string> out;
    if (!fs::exists(dir)) return out;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_directory() || e.path().filename().string().rfind("_worker_", 0) != 0) continue;
        fs::path db = e.path() / "solutions.sqlite3";
        if (fs::is_regular_file(db)) out.push_back(db.string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::set<std::string> read_done_chunks_db(const std::string& dir) {
    std::set<std::string> done;
    for (const auto& path : list_solution_dbs(dir)) {
        // Read-write open so SQLite can recover a WAL left by a killed run.
        sqlite3* db = open_db(path, false);
        try {
            Stmt st(db, "SELECT chunk FROM done_chunks");
            while (sqlite3_step(st.s) == SQLITE_ROW)
                done.insert(reinterpret_cast<const char*>(sqlite3_column_text(st.s, 0)));
        } catch (...) { sqlite3_close_v2(db); throw; }
        sqlite3_close_v2(db);
    }
    return done;
}

std::map<std::string, int64_t> solution_db_combos(const std::vector<std::string>& dbs) {
    std::map<std::string, int64_t> out;
    for (const auto& path : dbs) {
        sqlite3* db = open_db(path, false);
        try {
            Stmt st(db, "SELECT combo, SUM(count) FROM batches GROUP BY combo");
            int rc;
            while ((rc = sqlite3_step(st.s)) == SQLITE_ROW)
                out[reinterpret_cast<const char*>(sqlite3_column_text(st.s, 0))]
                    += sqlite3_column_int64(st.s, 1);
            check(db, rc, "list solution combos");
        } catch (...) { sqlite3_close_v2(db); throw; }
        sqlite3_close_v2(db);
    }
    return out;
}

std::map<std::string, std::map<int, int64_t>>
solution_db_bucket_counts(const std::vector<std::string>& dbs) {
    std::map<std::string, std::map<int, int64_t>> out;
    for (const auto& path : dbs) {
        sqlite3* db = open_db(path, false);
        try {
            bool layout = has_layout_column(db);
            Stmt plain(db, layout
                ? "SELECT combo, SUM(count) FROM batches WHERE layout=0 GROUP BY combo"
                : "SELECT combo, SUM(count) FROM batches GROUP BY combo");
            int rc;
            while ((rc = sqlite3_step(plain.s)) == SQLITE_ROW)
                out[reinterpret_cast<const char*>(sqlite3_column_text(plain.s, 0))][-1]
                    += sqlite3_column_int64(plain.s, 1);
            check(db, rc, "list solution combos");
            if (layout) {
                Stmt st(db, "SELECT combo, bucket_counts FROM batches WHERE layout=1");
                while ((rc = sqlite3_step(st.s)) == SQLITE_ROW) {
                    auto& m = out[reinterpret_cast<const char*>(sqlite3_column_text(st.s, 0))];
                    const char* c = static_cast<const char*>(sqlite3_column_blob(st.s, 1));
                    if (sqlite3_column_bytes(st.s, 1) != 4 * SOLUTION_BUCKETS)
                        throw std::runtime_error("corrupt bucket counts in " + path);
                    for (int i = 0; i < SOLUTION_BUCKETS; ++i)
                        if (uint32_t n = get_u32(c + 4 * i)) m[i] += n;
                }
                check(db, rc, "list solution buckets");
            }
        } catch (...) { sqlite3_close_v2(db); throw; }
        sqlite3_close_v2(db);
    }
    return out;
}

SolutionDbReader::SolutionDbReader(std::vector<std::string> dbs, std::string combo)
    : dbs_(std::move(dbs)), combo_(std::move(combo)) {}

SolutionDbReader::~SolutionDbReader() { close_db(); }

void SolutionDbReader::close_db() {
    if (stmt_) { sqlite3_finalize(stmt_); stmt_ = nullptr; }
    if (db_) { sqlite3_close_v2(db_); db_ = nullptr; }
}

bool SolutionDbReader::next(std::vector<char>& data, int& layout, std::string& buckets,
                            size_t& records_at) {
    while (true) {
        if (!stmt_) {
            if (next_db_ >= dbs_.size()) return false;
            db_ = open_db(dbs_[next_db_++], true);
            check(db_, sqlite3_prepare_v2(db_, has_layout_column(db_)
                ? "SELECT raw_size, data, layout FROM batches WHERE combo=? ORDER BY id"
                : "SELECT raw_size, data, 0 FROM batches WHERE combo=? ORDER BY id",
                -1, &stmt_, nullptr), "select solution batches");
            sqlite3_bind_text(stmt_, 1, combo_.data(), (int)combo_.size(), SQLITE_TRANSIENT);
        }
        int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_DONE) { close_db(); continue; }
        if (rc != SQLITE_ROW) throw std::runtime_error(
            std::string("reading solution batches: ") + sqlite3_errmsg(db_));
        size_t raw = (size_t)sqlite3_column_int64(stmt_, 0);
        const void* blob = sqlite3_column_blob(stmt_, 1);
        size_t n = (size_t)sqlite3_column_bytes(stmt_, 1);
        layout = sqlite3_column_int(stmt_, 2);
        if (layout != 0 && layout != 1)
            throw std::runtime_error("unknown solution batch layout in combo " + combo_);
        data.resize(raw);
        size_t got = ZSTD_decompress(data.data(), raw, blob, n);
        if (ZSTD_isError(got) || got != raw)
            throw std::runtime_error("corrupt solution batch for combo " + combo_);
        if (raw == 0) continue;
        records_at = 0;
        if (layout == 1) {
            if (raw < 4) throw std::runtime_error("corrupt bucketed solution batch");
            size_t n = get_u32(data.data());
            if (4 + n > raw) throw std::runtime_error("corrupt bucketed solution batch");
            buckets.assign(data.data() + 4, n);
            records_at = 4 + n;
        }
        return true;
    }
}

void remove_solution_db(const std::string& path) {
    std::error_code ec;
    for (const char* suffix : {"", "-wal", "-shm", "-journal"})
        fs::remove(path + suffix, ec);
}
