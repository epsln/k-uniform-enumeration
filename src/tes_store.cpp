#include "tes_store.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <sqlite3.h>
#include <zstd.h>

namespace fs = std::filesystem;

namespace {

constexpr uint64_t MAX_TES_CHUNK_SIZE = 512ULL * 1024 * 1024;

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

class Sha256 {
public:
    void update(const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        bit_count_ += static_cast<uint64_t>(size) * 8;
        while (size) {
            size_t take = std::min(size, block_.size() - block_size_);
            std::memcpy(block_.data() + block_size_, bytes, take);
            block_size_ += take;
            bytes += take;
            size -= take;
            if (block_size_ == block_.size()) {
                transform(block_.data());
                block_size_ = 0;
            }
        }
    }

    std::string finish_hex() {
        const uint64_t message_bits = bit_count_;
        const uint8_t marker = 0x80;
        update(&marker, 1);
        const uint8_t zero = 0;
        while (block_size_ != 56) update(&zero, 1);
        uint8_t length[8];
        for (int i = 0; i < 8; ++i)
            length[7 - i] = static_cast<uint8_t>(message_bits >> (i * 8));
        update(length, sizeof(length));

        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (uint32_t word : state_) out << std::setw(8) << word;
        return out.str();
    }

private:
    static uint32_t rotate_right(uint32_t value, unsigned amount) {
        return (value >> amount) | (value << (32 - amount));
    }

    void transform(const uint8_t* block) {
        static constexpr std::array<uint32_t, 64> constants{{
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
            0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
            0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        }};
        std::array<uint32_t, 64> words{};
        for (size_t i = 0; i < 16; ++i) {
            words[i] = static_cast<uint32_t>(block[i * 4]) << 24
                | static_cast<uint32_t>(block[i * 4 + 1]) << 16
                | static_cast<uint32_t>(block[i * 4 + 2]) << 8
                | static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (size_t i = 16; i < words.size(); ++i) {
            uint32_t s0 = rotate_right(words[i - 15], 7)
                ^ rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3);
            uint32_t s1 = rotate_right(words[i - 2], 17)
                ^ rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (size_t i = 0; i < words.size(); ++i) {
            uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            uint32_t choice = (e & f) ^ (~e & g);
            uint32_t temporary1 = h + sum1 + choice + constants[i] + words[i];
            uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temporary2 = sum0 + majority;
            h = g; g = f; f = e; e = d + temporary1;
            d = c; c = b; b = a; a = temporary1 + temporary2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<uint32_t, 8> state_{{
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    }};
    std::array<uint8_t, 64> block_{};
    size_t block_size_ = 0;
    uint64_t bit_count_ = 0;
};

void hash_field(Sha256& hash, const void* data, size_t size) {
    uint8_t length[8];
    const uint64_t value = static_cast<uint64_t>(size);
    for (int i = 0; i < 8; ++i)
        length[7 - i] = static_cast<uint8_t>(value >> (i * 8));
    hash.update(length, sizeof(length));
    hash.update(data, size);
}

void hash_field(Sha256& hash, const std::string& value) {
    hash_field(hash, value.data(), value.size());
}

void hash_id(Sha256& hash, int64_t id) {
    uint8_t encoded[8];
    const uint64_t value = static_cast<uint64_t>(id);
    for (int i = 0; i < 8; ++i)
        encoded[7 - i] = static_cast<uint8_t>(value >> (i * 8));
    hash_field(hash, encoded, sizeof(encoded));
}

} // namespace

TesReader::TesReader(const std::string& path) {
    std::error_code path_error;
    fs::path canonical = fs::canonical(path, path_error);
    if (path_error)
        throw std::runtime_error("resolve TES database path: " + path_error.message());
    identity_.canonical_path = canonical.string();
    identity_.file_size = fs::file_size(canonical, path_error);
    if (path_error)
        throw std::runtime_error("inspect TES database: " + path_error.message());

    int rc = sqlite3_open_v2(identity_.canonical_path.c_str(), &db_,
                             SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        std::string message = db_ ? sqlite3_errmsg(db_) : "cannot allocate SQLite handle";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("open TES database: " + message);
    }
    try {
        exec(db_, "BEGIN;");
        Statement version(db_, "SELECT value FROM metadata WHERE key='schema_version';");
        if (sqlite3_step(version.get()) != SQLITE_ROW || column_text(version.get(), 0) != "1")
            throw std::runtime_error("unsupported TES database schema");
        identity_.schema_version = 1;
        Statement compression(db_, "SELECT value FROM metadata WHERE key='compression';");
        if (sqlite3_step(compression.get()) != SQLITE_ROW)
            throw std::runtime_error("unsupported TES database compression");
        const std::string compression_name = column_text(compression.get(), 0);
        if (compression_name != "zstd")
            throw std::runtime_error("unsupported TES database compression");

        Sha256 fingerprint;
        // Every logical field is framed by an unsigned 64-bit big-endian length;
        // IDs are fixed-width big-endian values inside the same framing.
        hash_field(fingerprint, std::string("tes-source-fingerprint-v2"));
        hash_field(fingerprint, std::to_string(identity_.schema_version));
        hash_field(fingerprint, compression_name);
        int64_t scanned_count = 0;
        for_each_after(0, [&](const TesEntry& entry) {
            hash_id(fingerprint, entry.id);
            hash_field(fingerprint, entry.combo);
            hash_field(fingerprint, entry.signature);
            hash_field(fingerprint, entry.legacy_filename);
            hash_field(fingerprint, entry.document);
            ++scanned_count;
            identity_.max_id = entry.id;
        });
        Statement totals(db_, "SELECT COUNT(*) FROM entries;");
        check_sqlite(sqlite3_step(totals.get()), db_, "read TES entry count");
        identity_.entry_count = sqlite3_column_int64(totals.get(), 0);
        if (identity_.entry_count != scanned_count)
            throw std::runtime_error("TES entry ids must be positive");
        identity_.fingerprint = fingerprint.finish_hex();
    } catch (...) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
        throw;
    }
}

TesReader::~TesReader() {
    if (db_) sqlite3_close_v2(db_);
}

void TesReader::for_each_after(
        int64_t start_id, const std::function<void(const TesEntry&)>& visitor) const {
    if (start_id < 0) throw std::invalid_argument("TES start id is negative");
    if (!visitor) throw std::invalid_argument("TES visitor is empty");
    Statement rows(db_,
        "SELECT e.id,e.combo,e.signature,e.legacy_filename,e.chunk_id,e.ordinal,"
        "e.byte_offset,e.byte_length,c.record_count,c.uncompressed_size,"
        "c.compressed_size,c.data FROM entries e JOIN chunks c ON c.id=e.chunk_id "
        "WHERE e.id>? ORDER BY e.id;");
    check_sqlite(sqlite3_bind_int64(rows.get(), 1, start_id), db_, "bind TES start id");

    int64_t previous_id = start_id;
    int64_t loaded_chunk = -1;
    int64_t raw_size = 0;
    std::vector<char> decompressed;
    int rc;
    while ((rc = sqlite3_step(rows.get())) == SQLITE_ROW) {
        for (int column = 0; column < 12; ++column) {
            if (sqlite3_column_type(rows.get(), column) == SQLITE_NULL)
                throw std::runtime_error("null in required TES entry field");
        }
        TesEntry entry;
        entry.id = sqlite3_column_int64(rows.get(), 0);
        if (entry.id <= previous_id)
            throw std::runtime_error("TES entry ids are not strictly ascending");
        previous_id = entry.id;
        entry.combo = column_text(rows.get(), 1);
        entry.signature = column_text(rows.get(), 2);
        entry.legacy_filename = column_text(rows.get(), 3);

        int64_t chunk_id = sqlite3_column_int64(rows.get(), 4);
        int64_t ordinal = sqlite3_column_int64(rows.get(), 5);
        int64_t record_count = sqlite3_column_int64(rows.get(), 8);
        raw_size = sqlite3_column_int64(rows.get(), 9);
        int64_t compressed_size = sqlite3_column_int64(rows.get(), 10);
        int blob_size = sqlite3_column_bytes(rows.get(), 11);
        if (chunk_id <= 0 || ordinal < 0 || ordinal >= record_count || record_count <= 0
                || raw_size < 0
                || static_cast<uint64_t>(raw_size) > MAX_TES_CHUNK_SIZE
                || compressed_size < 0 || compressed_size != blob_size)
            throw std::runtime_error("invalid TES chunk metadata");
        if (chunk_id != loaded_chunk) {
            decompressed.resize(static_cast<size_t>(raw_size));
            const void* blob = sqlite3_column_blob(rows.get(), 11);
            size_t actual = ZSTD_decompress(decompressed.data(), decompressed.size(),
                                            blob, static_cast<size_t>(blob_size));
            if (ZSTD_isError(actual) || actual != decompressed.size())
                throw std::runtime_error("invalid compressed TES chunk");
            loaded_chunk = chunk_id;
        }
        int64_t offset = sqlite3_column_int64(rows.get(), 6);
        int64_t length = sqlite3_column_int64(rows.get(), 7);
        if (offset < 0 || length < 0 || offset > raw_size || length > raw_size - offset)
            throw std::runtime_error("invalid TES entry bounds");
        entry.document.assign(decompressed.data() + static_cast<size_t>(offset),
                              static_cast<size_t>(length));
        visitor(entry);
    }
    if (rc != SQLITE_DONE) check_sqlite(rc, db_, "read TES entries");
}

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
    check_sqlite(sqlite3_close(db_), db_, "close TES database");
    db_ = nullptr;
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
            if (raw_size < 0 || static_cast<uint64_t>(raw_size) > MAX_TES_CHUNK_SIZE)
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
            std::error_code status_error;
            fs::file_status path_status = fs::symlink_status(path, status_error);
            if (status_error && status_error != std::errc::no_such_file_or_directory)
                throw std::runtime_error("inspect extracted TES path: " + status_error.message());
            if (fs::is_symlink(path_status))
                throw std::runtime_error("extracted TES path is a symlink: " + path.string());
            if (fs::exists(path_status)) {
                if (!written_paths.count(path.string()))
                    throw std::runtime_error("extracted TES file already exists: " + path.string());
                path = parent / (path.stem().string() + " [" + std::to_string(entry_id)
                                 + "]" + path.extension().string());
                path_status = fs::symlink_status(path, status_error);
                if (fs::is_symlink(path_status) || fs::exists(path_status))
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
