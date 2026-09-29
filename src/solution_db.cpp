#include "solution_db.h"

#include "disk_solver.h"
#include "solver.h"

#include <sqlite3.h>
#include <zstd.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <streambuf>

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
    exec(db_, "CREATE INDEX IF NOT EXISTS batches_combo ON batches(combo, id)");
    exec(db_, "CREATE TABLE IF NOT EXISTS done_chunks(chunk TEXT PRIMARY KEY)");
}

SolutionDbWriter::~SolutionDbWriter() {
    try { close(); } catch (const std::exception&) {}
    // close() threw: do not leak the handle, but never commit partial state.
    if (db_) { sqlite3_close_v2(db_); db_ = nullptr; }
}

void SolutionDbWriter::add(const State& solution) {
    std::string combo = fine_name(solution);
    Buffer& b = buffers_[combo];
    std::ostringstream os(std::ios::binary);
    write_state_bin(os, solution);
    std::string rec = os.str();
    b.records += rec;
    ++b.count;
    buffered_ += rec.size();
    if (b.records.size() >= batch_bytes_) {
        if (!in_txn_) { exec(db_, "BEGIN IMMEDIATE"); in_txn_ = true; }
        buffered_ -= b.records.size();
        write_batch(combo, b.records, b.count);
        b.count = 0;
    }
}

void SolutionDbWriter::write_batch(const std::string& combo, std::string& records, int64_t count) {
    if (records.empty()) return;
    compressed_.resize(ZSTD_compressBound(records.size()));
    size_t n = ZSTD_compress(compressed_.data(), compressed_.size(),
                             records.data(), records.size(), 3);
    if (ZSTD_isError(n)) throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(n));
    Stmt st(db_, "INSERT INTO batches(combo, count, raw_size, data) VALUES(?,?,?,?)");
    sqlite3_bind_text(st.s, 1, combo.data(), (int)combo.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, count);
    sqlite3_bind_int64(st.s, 3, (sqlite3_int64)records.size());
    sqlite3_bind_blob(st.s, 4, compressed_.data(), (int)n, SQLITE_STATIC);
    check(db_, sqlite3_step(st.s), "insert solution batch");
    records.clear();
}

void SolutionDbWriter::commit() {
    if (!db_) return;
    if (!in_txn_) { exec(db_, "BEGIN IMMEDIATE"); in_txn_ = true; }
    for (auto& [combo, b] : buffers_) { write_batch(combo, b.records, b.count); b.count = 0; }
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

namespace {

// Streams the decompressed batches of one combo, database after database.
class SolutionDbStreamBuf : public std::streambuf {
public:
    SolutionDbStreamBuf(std::vector<std::string> dbs, std::string combo)
        : dbs_(std::move(dbs)), combo_(std::move(combo)) {}
    ~SolutionDbStreamBuf() override { close_db(); }

protected:
    int_type underflow() override {
        while (true) {
            if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
            if (!next_batch()) return traits_type::eof();
        }
    }

private:
    void close_db() {
        if (stmt_) { sqlite3_finalize(stmt_); stmt_ = nullptr; }
        if (db_) { sqlite3_close_v2(db_); db_ = nullptr; }
    }

    bool next_batch() {
        while (true) {
            if (!stmt_) {
                if (next_db_ >= dbs_.size()) return false;
                db_ = open_db(dbs_[next_db_++], true);
                check(db_, sqlite3_prepare_v2(db_,
                    "SELECT raw_size, data FROM batches WHERE combo=? ORDER BY id",
                    -1, &stmt_, nullptr), "select solution batches");
                sqlite3_bind_text(stmt_, 1, combo_.data(), (int)combo_.size(), SQLITE_TRANSIENT);
            }
            int rc = sqlite3_step(stmt_);
            if (rc == SQLITE_DONE) { close_db(); continue; }
            if (rc != SQLITE_ROW) throw std::runtime_error(
                std::string("reading solution batches: ") + sqlite3_errmsg(db_));
            size_t raw = (size_t)sqlite3_column_int64(stmt_, 0);
            const void* data = sqlite3_column_blob(stmt_, 1);
            size_t n = (size_t)sqlite3_column_bytes(stmt_, 1);
            buf_.resize(raw);
            size_t got = ZSTD_decompress(buf_.data(), raw, data, n);
            if (ZSTD_isError(got) || got != raw)
                throw std::runtime_error("corrupt solution batch for combo " + combo_);
            if (raw == 0) continue;
            setg(buf_.data(), buf_.data(), buf_.data() + raw);
            return true;
        }
    }

    std::vector<std::string> dbs_;
    std::string combo_;
    size_t next_db_ = 0;
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
    std::vector<char> buf_;
};

class SolutionDbStream : public std::istream {
public:
    SolutionDbStream(std::vector<std::string> dbs, std::string combo)
        : std::istream(nullptr), buf_(std::move(dbs), std::move(combo)) {
        rdbuf(&buf_);
        exceptions(std::ios::badbit);   // surface the original read error
    }
private:
    SolutionDbStreamBuf buf_;
};

}  // namespace

std::unique_ptr<std::istream> open_solution_db_stream(const std::vector<std::string>& dbs,
                                                       const std::string& combo) {
    return std::make_unique<SolutionDbStream>(dbs, combo);
}

void remove_solution_db(const std::string& path) {
    std::error_code ec;
    for (const char* suffix : {"", "-wal", "-shm", "-journal"})
        fs::remove(path + suffix, ec);
}
