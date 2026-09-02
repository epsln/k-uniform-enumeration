#include "tes_store.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <sqlite3.h>
#include <zstd.h>

namespace fs = std::filesystem;

namespace {

void check_sqlite(int rc, sqlite3* db, const char* operation) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW)
        throw std::runtime_error(std::string(operation) + ": " + sqlite3_errmsg(db));
}

void exec(sqlite3* db, const char* sql) {
    char* error = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc == SQLITE_OK) return;
    std::string message = error ? error : sqlite3_errmsg(db);
    sqlite3_free(error);
    throw std::runtime_error(message);
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        check_sqlite(sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr), db, "prepare SQL");
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    sqlite3_stmt* get() const { return stmt_; }

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

void bind_text(sqlite3* db, sqlite3_stmt* stmt, int column, const std::string& value) {
    check_sqlite(sqlite3_bind_text(stmt, column, value.data(),
                                   static_cast<int>(value.size()), SQLITE_TRANSIENT),
                 db, "bind text");
}

std::string column_text(sqlite3_stmt* stmt, int column) {
    const auto* value = sqlite3_column_text(stmt, column);
    int length = sqlite3_column_bytes(stmt, column);
    return value ? std::string(reinterpret_cast<const char*>(value), length) : std::string();
}

} // namespace

TesStore::TesStore(const std::string& path, size_t chunk_target)
    : chunk_target_(chunk_target) {
    if (fs::exists(path))
        throw std::runtime_error("TES database already exists: " + path);
    fs::path parent = fs::path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent);

    int rc = sqlite3_open_v2(path.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        std::string message = db_ ? sqlite3_errmsg(db_) : "cannot allocate SQLite handle";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("open TES database: " + message);
    }

    try {
        exec(db_, "PRAGMA journal_mode=DELETE;");
        exec(db_, "PRAGMA synchronous=NORMAL;");
        exec(db_, "PRAGMA foreign_keys=ON;");
        exec(db_,
             "CREATE TABLE metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
             "INSERT INTO metadata VALUES('schema_version', '1');"
             "INSERT INTO metadata VALUES('compression', 'zstd');"
             "CREATE TABLE chunks("
             " id INTEGER PRIMARY KEY,"
             " record_count INTEGER NOT NULL,"
             " uncompressed_size INTEGER NOT NULL,"
             " compressed_size INTEGER NOT NULL,"
             " data BLOB NOT NULL);"
             "CREATE TABLE entries("
             " id INTEGER PRIMARY KEY,"
             " chunk_id INTEGER NOT NULL REFERENCES chunks(id),"
             " ordinal INTEGER NOT NULL,"
             " byte_offset INTEGER NOT NULL,"
             " byte_length INTEGER NOT NULL,"
             " combo TEXT NOT NULL,"
             " signature TEXT NOT NULL,"
             " legacy_filename TEXT NOT NULL);"
             "CREATE INDEX entries_combo ON entries(combo);");
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        fs::remove(path);
        throw;
    }
}

TesStore::~TesStore() {
    if (db_) sqlite3_close(db_);
}

void TesStore::add(const std::string& combo, const std::string& signature,
                   const std::string& legacy_filename, const std::string& document) {
    if (finished_) throw std::runtime_error("cannot add to a finished TES store");
    if (!entries_.empty() && chunk_.size() + document.size() > chunk_target_) flush();
    entries_.push_back({combo, signature, legacy_filename,
                        static_cast<int64_t>(chunk_.size()),
                        static_cast<int64_t>(document.size())});
    chunk_.append(document);
}

void TesStore::flush() {
    if (entries_.empty()) return;

    size_t bound = ZSTD_compressBound(chunk_.size());
    std::vector<char> compressed(bound);
    size_t compressed_size = ZSTD_compress(compressed.data(), compressed.size(),
                                           chunk_.data(), chunk_.size(), 3);
    if (ZSTD_isError(compressed_size))
        throw std::runtime_error(std::string("compress TES chunk: ")
                                 + ZSTD_getErrorName(compressed_size));
    if (compressed_size > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("compressed TES chunk exceeds SQLite blob limit");

    exec(db_, "BEGIN IMMEDIATE;");
    try {
        Statement chunk_stmt(db_,
            "INSERT INTO chunks(record_count, uncompressed_size, compressed_size, data)"
            " VALUES(?, ?, ?, ?);");
        sqlite3_stmt* cs = chunk_stmt.get();
        check_sqlite(sqlite3_bind_int64(cs, 1, entries_.size()), db_, "bind record count");
        check_sqlite(sqlite3_bind_int64(cs, 2, chunk_.size()), db_, "bind raw size");
        check_sqlite(sqlite3_bind_int64(cs, 3, compressed_size), db_, "bind compressed size");
        check_sqlite(sqlite3_bind_blob(cs, 4, compressed.data(),
                                      static_cast<int>(compressed_size), SQLITE_TRANSIENT),
                     db_, "bind compressed data");
        check_sqlite(sqlite3_step(cs), db_, "insert TES chunk");
        sqlite3_int64 chunk_id = sqlite3_last_insert_rowid(db_);

        Statement entry_stmt(db_,
            "INSERT INTO entries(chunk_id, ordinal, byte_offset, byte_length, combo, signature, legacy_filename)"
            " VALUES(?, ?, ?, ?, ?, ?, ?);");
        sqlite3_stmt* es = entry_stmt.get();
        for (size_t i = 0; i < entries_.size(); ++i) {
            const auto& entry = entries_[i];
            check_sqlite(sqlite3_bind_int64(es, 1, chunk_id), db_, "bind chunk id");
            check_sqlite(sqlite3_bind_int64(es, 2, i), db_, "bind ordinal");
            check_sqlite(sqlite3_bind_int64(es, 3, entry.offset), db_, "bind offset");
            check_sqlite(sqlite3_bind_int64(es, 4, entry.length), db_, "bind length");
            bind_text(db_, es, 5, entry.combo);
            bind_text(db_, es, 6, entry.signature);
            bind_text(db_, es, 7, entry.legacy_filename);
            check_sqlite(sqlite3_step(es), db_, "insert TES entry");
            check_sqlite(sqlite3_reset(es), db_, "reset TES entry insert");
            check_sqlite(sqlite3_clear_bindings(es), db_, "clear TES entry bindings");
        }
        exec(db_, "COMMIT;");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }

    chunk_.clear();
    entries_.clear();
}

void TesStore::finish() {
    if (finished_) return;
    flush();
    exec(db_, "PRAGMA optimize;");
    finished_ = true;
}

void TesStore::checkpoint() {
    if (finished_) throw std::runtime_error("cannot checkpoint a finished TES store");
    flush();
}

int64_t extract_tes_database(const std::string& database_path,
                             const std::string& output_dir, int64_t id) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(database_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        std::string message = db ? sqlite3_errmsg(db) : "cannot allocate SQLite handle";
        if (db) sqlite3_close(db);
        throw std::runtime_error("open TES database: " + message);
    }

    try {
        Statement version(db, "SELECT value FROM metadata WHERE key='schema_version';");
        if (sqlite3_step(version.get()) != SQLITE_ROW || column_text(version.get(), 0) != "1")
            throw std::runtime_error("unsupported TES database schema");

        const char* sql = id == 0
            ? "SELECT c.id, e.id, e.combo, e.legacy_filename, e.byte_offset, e.byte_length, c.data, c.uncompressed_size"
              " FROM entries e JOIN chunks c ON c.id=e.chunk_id ORDER BY e.id;"
            : "SELECT c.id, e.id, e.combo, e.legacy_filename, e.byte_offset, e.byte_length, c.data, c.uncompressed_size"
              " FROM entries e JOIN chunks c ON c.id=e.chunk_id WHERE e.id=?;";
        Statement rows(db, sql);
        if (id != 0)
            check_sqlite(sqlite3_bind_int64(rows.get(), 1, id), db, "bind TES id");

        fs::create_directories(output_dir);
        fs::path output_root = fs::weakly_canonical(output_dir);
        sqlite3_int64 loaded_chunk = -1;
        std::vector<char> decompressed;
        std::unordered_set<std::string> written_paths;
        int64_t count = 0;
        while ((rc = sqlite3_step(rows.get())) == SQLITE_ROW) {
            sqlite3_int64 chunk_id = sqlite3_column_int64(rows.get(), 0);
            sqlite3_int64 entry_id = sqlite3_column_int64(rows.get(), 1);
            const void* blob = sqlite3_column_blob(rows.get(), 6);
            int blob_size = sqlite3_column_bytes(rows.get(), 6);
            int64_t raw_size = sqlite3_column_int64(rows.get(), 7);
            if (raw_size < 0 || static_cast<uint64_t>(raw_size) > std::numeric_limits<size_t>::max())
                throw std::runtime_error("invalid TES chunk size");
            if (chunk_id != loaded_chunk) {
                decompressed.resize(static_cast<size_t>(raw_size));
                size_t actual = ZSTD_decompress(decompressed.data(), decompressed.size(), blob, blob_size);
                if (ZSTD_isError(actual) || actual != decompressed.size())
                    throw std::runtime_error("invalid compressed TES chunk");
                loaded_chunk = chunk_id;
            }
            int64_t offset = sqlite3_column_int64(rows.get(), 4);
            int64_t length = sqlite3_column_int64(rows.get(), 5);
            if (offset < 0 || length < 0 || offset > raw_size || length > raw_size - offset)
                throw std::runtime_error("invalid TES entry bounds");

            std::string combo = column_text(rows.get(), 2);
            std::string filename = column_text(rows.get(), 3);
            if (combo.empty() || filename.empty()
                    || fs::path(combo).filename() != fs::path(combo)
                    || fs::path(filename).filename() != fs::path(filename))
                throw std::runtime_error("unsafe TES entry path");
            fs::path parent = output_root / combo;
            fs::create_directories(parent);
            if (fs::weakly_canonical(parent).parent_path() != output_root)
                throw std::runtime_error("TES entry path escapes extraction directory");
            fs::path path = parent / filename;
            if (fs::exists(path)) {
                if (!written_paths.count(path.string()))
                    throw std::runtime_error("extracted TES file already exists: " + path.string());
                path = parent / (path.stem().string() + " [" + std::to_string(entry_id)
                                 + "]" + path.extension().string());
                if (fs::exists(path))
                    throw std::runtime_error("extracted TES file already exists: " + path.string());
            }
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(decompressed.data() + offset, length);
            if (!out) throw std::runtime_error("write extracted TES file: " + path.string());
            written_paths.insert(path.string());
            ++count;
        }
        if (rc != SQLITE_DONE) check_sqlite(rc, db, "read TES entries");
        if (id != 0 && count == 0) throw std::runtime_error("TES entry id not found");
        sqlite3_close_v2(db);
        return count;
    } catch (...) {
        sqlite3_close_v2(db);
        throw;
    }
}
