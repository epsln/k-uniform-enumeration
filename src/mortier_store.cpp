#include "mortier_store.h"
#include "mortier_geometry.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include <sstream>

#include <sqlite3.h>
#include <zstd.h>

namespace fs = std::filesystem;

namespace {

constexpr size_t MAX_ENCODED_RECORD_SIZE = 256 * 1024 * 1024;
constexpr uint64_t MAX_SEEDS = 10'000'000;
constexpr uint64_t MAX_CHUNK_SIZE = 512 * 1024 * 1024;

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
        check_sqlite(sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr), db,
                     "prepare Mortier SQL");
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    sqlite3_stmt* get() const { return stmt_; }

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

void bind_text(sqlite3* db, sqlite3_stmt* stmt, int column,
               const std::string& value) {
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("Mortier text value exceeds SQLite limit");
    check_sqlite(sqlite3_bind_text(stmt, column, value.data(),
                                  static_cast<int>(value.size()), SQLITE_TRANSIENT),
                 db, "bind Mortier text");
}

void bind_blob(sqlite3* db, sqlite3_stmt* stmt, int column,
               const std::vector<uint8_t>& value) {
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("Mortier blob exceeds SQLite limit");
    check_sqlite(sqlite3_bind_blob(stmt, column, value.data(),
                                  static_cast<int>(value.size()), SQLITE_TRANSIENT),
                 db, "bind Mortier blob");
}

std::string column_text(sqlite3_stmt* stmt, int column) {
    const auto* value = sqlite3_column_text(stmt, column);
    int size = sqlite3_column_bytes(stmt, column);
    return value ? std::string(reinterpret_cast<const char*>(value), size)
                 : std::string();
}

uint64_t signed_bits(int64_t value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

int64_t bits_signed(uint64_t bits) {
    int64_t value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint64_t zigzag_encode(int64_t value) {
    uint64_t bits = signed_bits(value);
    return (bits << 1) ^ (0 - (bits >> 63));
}

int64_t zigzag_decode(uint64_t value) {
    uint64_t bits = (value >> 1) ^ (0 - (value & 1));
    return bits_signed(bits);
}

void put_varint(std::vector<uint8_t>& out, uint64_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        out.push_back(static_cast<uint8_t>(byte | (value ? 0x80 : 0)));
    } while (value);
    if (out.size() > MAX_ENCODED_RECORD_SIZE)
        throw std::runtime_error("Mortier record exceeds codec size limit");
}

class Decoder {
public:
    Decoder(const uint8_t* data, size_t size) : current_(data), end_(data) {
        if (size != 0 && data == nullptr)
            throw std::runtime_error("null Mortier codec input");
        end_ = data + size;
    }

    uint64_t varint() {
        uint64_t value = 0;
        for (unsigned i = 0; i < 10; ++i) {
            if (current_ == end_)
                throw std::runtime_error("truncated Mortier varint");
            uint8_t byte = *current_++;
            if (i == 9 && (byte & 0xfe) != 0)
                throw std::runtime_error("overflowing Mortier varint");
            value |= static_cast<uint64_t>(byte & 0x7f) << (7 * i);
            if ((byte & 0x80) == 0) {
                if (i != 0 && byte == 0)
                    throw std::runtime_error("non-canonical Mortier varint");
                return value;
            }
        }
        throw std::runtime_error("overflowing Mortier varint");
    }

    int64_t integer() { return zigzag_decode(varint()); }
    bool empty() const { return current_ == end_; }

private:
    const uint8_t* current_;
    const uint8_t* end_;
};

void encode_point(std::vector<uint8_t>& out, const Z4Point& point) {
    for (int64_t coordinate : point.coordinates)
        put_varint(out, zigzag_encode(coordinate));
}

Z4Point decode_point(Decoder& decoder) {
    Z4Point point;
    for (int64_t& coordinate : point.coordinates) coordinate = decoder.integer();
    return point;
}

struct ReadDatabase {
    explicit ReadDatabase(const std::string& path) {
        int rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
        if (rc != SQLITE_OK) {
            std::string message = db ? sqlite3_errmsg(db) : "cannot allocate SQLite handle";
            if (db) sqlite3_close(db);
            db = nullptr;
            throw std::runtime_error("open Mortier database: " + message);
        }
        try {
            validate();
        } catch (...) {
            sqlite3_close_v2(db);
            db = nullptr;
            throw;
        }
    }

    ~ReadDatabase() { if (db) sqlite3_close_v2(db); }

    std::string metadata(const char* key) {
        Statement statement(db, "SELECT value FROM metadata WHERE key=?;");
        check_sqlite(sqlite3_bind_text(statement.get(), 1, key, -1, SQLITE_STATIC),
                     db, "bind Mortier metadata key");
        int rc = sqlite3_step(statement.get());
        if (rc != SQLITE_ROW)
            throw std::runtime_error(std::string("missing Mortier metadata: ") + key);
        std::string result = column_text(statement.get(), 0);
        if (sqlite3_step(statement.get()) != SQLITE_DONE)
            throw std::runtime_error(std::string("duplicate Mortier metadata: ") + key);
        return result;
    }

    void validate() {
        if (metadata("schema_version") != std::to_string(MortierStore::SCHEMA_VERSION))
            throw std::runtime_error("unsupported Mortier database schema");
        if (metadata("codec_version") != std::to_string(MortierStore::CODEC_VERSION))
            throw std::runtime_error("unsupported Mortier codec version");
        if (metadata("compression") != "zstd")
            throw std::runtime_error("unsupported Mortier compression");
        if (metadata("z4_basis").empty())
            throw std::runtime_error("missing Mortier z4 basis");
        std::string complete = metadata("complete");
        if (complete != "0" && complete != "1")
            throw std::runtime_error("invalid Mortier complete marker");
    }

    sqlite3* db = nullptr;
};

using RowBinder = std::function<void(sqlite3*, sqlite3_stmt*)>;

void read_rows(const std::string& path, const char* where, const RowBinder& binder,
               const std::function<void(const MortierStoredEntry&)>& visitor) {
    ReadDatabase database(path);
    std::string sql =
        "SELECT e.id,e.stable_id,e.k,e.seed_count,e.signature,e.chunk_id,"
        "e.byte_offset,e.byte_length,c.uncompressed_size,c.compressed_size,c.data "
        "FROM entries e JOIN chunks c ON c.id=e.chunk_id ";
    sql += where;
    sql += " ORDER BY e.id;";
    Statement rows(database.db, sql.c_str());
    binder(database.db, rows.get());

    int64_t loaded_chunk = -1;
    std::vector<uint8_t> decompressed;
    int rc;
    while ((rc = sqlite3_step(rows.get())) == SQLITE_ROW) {
        for (int column : {0, 1, 2, 3, 5, 6, 7, 8, 9, 10}) {
            if (sqlite3_column_type(rows.get(), column) == SQLITE_NULL)
                throw std::runtime_error("null in required Mortier entry field");
        }
        int64_t chunk_id = sqlite3_column_int64(rows.get(), 5);
        int64_t raw_size = sqlite3_column_int64(rows.get(), 8);
        int64_t compressed_size = sqlite3_column_int64(rows.get(), 9);
        int blob_size = sqlite3_column_bytes(rows.get(), 10);
        if (chunk_id <= 0 || raw_size <= 0
                || static_cast<uint64_t>(raw_size) > MAX_CHUNK_SIZE
                || compressed_size < 0 || compressed_size != blob_size)
            throw std::runtime_error("invalid Mortier chunk metadata");

        if (chunk_id != loaded_chunk) {
            decompressed.resize(static_cast<size_t>(raw_size));
            const void* blob = sqlite3_column_blob(rows.get(), 10);
            size_t actual = ZSTD_decompress(decompressed.data(), decompressed.size(),
                                            blob, static_cast<size_t>(blob_size));
            if (ZSTD_isError(actual) || actual != decompressed.size())
                throw std::runtime_error("invalid compressed Mortier chunk");
            loaded_chunk = chunk_id;
        }

        int64_t offset = sqlite3_column_int64(rows.get(), 6);
        int64_t length = sqlite3_column_int64(rows.get(), 7);
        if (offset < 0 || length <= 0 || offset > raw_size || length > raw_size - offset)
            throw std::runtime_error("invalid Mortier entry bounds");

        MortierStoredEntry entry;
        entry.id = sqlite3_column_int64(rows.get(), 0);
        const void* stable_id = sqlite3_column_blob(rows.get(), 1);
        int stable_id_size = sqlite3_column_bytes(rows.get(), 1);
        if (entry.id <= 0 || stable_id_size <= 0)
            throw std::runtime_error("invalid Mortier entry identity");
        const auto* stable_bytes = static_cast<const uint8_t*>(stable_id);
        entry.stable_id.assign(stable_bytes, stable_bytes + stable_id_size);
        int64_t k = sqlite3_column_int64(rows.get(), 2);
        entry.seed_count = sqlite3_column_int64(rows.get(), 3);
        if (k < 0 || k > std::numeric_limits<int>::max()
                || entry.seed_count < 0
                || static_cast<uint64_t>(entry.seed_count) > MAX_SEEDS)
            throw std::runtime_error("invalid Mortier entry counts");
        entry.k = static_cast<int>(k);
        if (sqlite3_column_type(rows.get(), 4) != SQLITE_NULL)
            entry.signature = column_text(rows.get(), 4);
        entry.record = decode_mortier_record(
            decompressed.data() + static_cast<size_t>(offset),
            static_cast<size_t>(length));
        if (entry.record.seeds.size() != static_cast<size_t>(entry.seed_count))
            throw std::runtime_error("Mortier seed count does not match record");
        visitor(entry);
    }
    if (rc != SQLITE_DONE) check_sqlite(rc, database.db, "read Mortier entries");
}

class MortierJsonParser {
public:
    explicit MortierJsonParser(std::istream& input) : input_(input) {}

    int64_t parse(MortierStore& store) {
        expect('{');
        skip_space();
        if (consume('}')) {
            require_end();
            return 0;
        }

        int64_t count = 0;
        std::set<std::string> names;
        std::set<std::vector<uint8_t>> stable_ids;
        while (true) {
            std::string name = string();
            if (!names.insert(name).second) fail("duplicate JSON object key");
            int k = 0;
            std::vector<uint8_t> stable_id = record_name(name, k);
            if (!stable_ids.insert(stable_id).second)
                fail("duplicate Mortier stable id");
            expect(':');
            MortierRecord record = record_object();
            if (record.seeds.empty()) fail("Mortier Seed array is empty");
            std::vector<Z4Point> sorted = record.seeds;
            std::sort(sorted.begin(), sorted.end(),
                      [](const Z4Point& left, const Z4Point& right) {
                          return left.coordinates < right.coordinates;
                      });
            for (size_t i = 1; i < sorted.size(); ++i) {
                if (sorted[i - 1].coordinates == sorted[i].coordinates)
                    fail("duplicate Mortier seed");
            }
            mortier_geometry::normalize_basis(record.t1, record.t2);
            for (Z4Point& seed : record.seeds)
                seed = mortier_geometry::reduce_mod_lattice(seed, record.t1, record.t2);
            std::sort(record.seeds.begin(), record.seeds.end());
            record.seeds.erase(std::unique(record.seeds.begin(), record.seeds.end()),
                               record.seeds.end());
            mortier_geometry::validate_mortier_record(record);
            store.add(stable_id, k, record);
            ++count;

            skip_space();
            if (consume('}')) break;
            expect(',');
        }
        require_end();
        return count;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error("Mortier JSON at byte " + std::to_string(offset_)
                                 + ": " + message);
    }

    int peek() {
        int value = input_.peek();
        if (value == std::char_traits<char>::eof() && input_.bad())
            fail("input read failed");
        return value;
    }

    char take() {
        int value = input_.get();
        if (value == std::char_traits<char>::eof()) {
            if (input_.bad()) fail("input read failed");
            fail("unexpected end of input");
        }
        ++offset_;
        return static_cast<char>(value);
    }

    void skip_space() {
        while (true) {
            int value = peek();
            if (value != ' ' && value != '\t' && value != '\n' && value != '\r')
                return;
            take();
        }
    }

    bool consume(char expected) {
        skip_space();
        if (peek() != static_cast<unsigned char>(expected)) return false;
        take();
        return true;
    }

    void expect(char expected) {
        if (!consume(expected))
            fail(std::string("expected '") + expected + "'");
    }

    void require_end() {
        skip_space();
        if (peek() != std::char_traits<char>::eof())
            fail("trailing input after top-level object");
    }

    std::string string() {
        skip_space();
        if (take() != '"') fail("expected JSON string");
        std::string result;
        while (true) {
            unsigned char value = static_cast<unsigned char>(take());
            if (value == '"') return result;
            if (value == '\\')
                fail("escaped strings are not generated Mortier JSON");
            if (value < 0x20 || value > 0x7f)
                fail("non-ASCII byte in Mortier JSON key");
            result.push_back(static_cast<char>(value));
            if (result.size() > 128) fail("Mortier JSON key is too long");
        }
    }

    int64_t integer() {
        skip_space();
        bool negative = false;
        if (peek() == '-') {
            take();
            negative = true;
        }
        int first = peek();
        if (first < '0' || first > '9') fail("expected integer");
        if (first == '0') {
            take();
            int next = peek();
            if (next >= '0' && next <= '9') fail("leading zero in integer");
            return 0;
        }

        const uint64_t limit = negative
            ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1
            : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        uint64_t magnitude = 0;
        while (true) {
            int value = peek();
            if (value < '0' || value > '9') break;
            unsigned digit = static_cast<unsigned>(take() - '0');
            if (magnitude > (limit - digit) / 10)
                fail("integer overflow");
            magnitude = magnitude * 10 + digit;
        }
        if (!negative) return static_cast<int64_t>(magnitude);
        if (magnitude == limit) return std::numeric_limits<int64_t>::min();
        return -static_cast<int64_t>(magnitude);
    }

    Z4Point point() {
        Z4Point result;
        expect('[');
        for (size_t i = 0; i < result.coordinates.size(); ++i) {
            if (i != 0) expect(',');
            result.coordinates[i] = integer();
        }
        expect(']');
        return result;
    }

    std::vector<Z4Point> seeds() {
        std::vector<Z4Point> result;
        expect('[');
        skip_space();
        if (consume(']')) return result;
        while (true) {
            if (result.size() == MAX_SEEDS)
                fail("Mortier seed count exceeds codec limit");
            result.push_back(point());
            skip_space();
            if (consume(']')) return result;
            expect(',');
        }
    }

    MortierRecord record_object() {
        MortierRecord result;
        bool have_t1 = false, have_t2 = false, have_seed = false;
        expect('{');
        skip_space();
        if (consume('}')) fail("Mortier record has no fields");
        while (true) {
            std::string field = string();
            expect(':');
            if (field == "T1") {
                if (have_t1) fail("duplicate Mortier record field T1");
                result.t1 = point();
                have_t1 = true;
            } else if (field == "T2") {
                if (have_t2) fail("duplicate Mortier record field T2");
                result.t2 = point();
                have_t2 = true;
            } else if (field == "Seed") {
                if (have_seed) fail("duplicate Mortier record field Seed");
                result.seeds = seeds();
                have_seed = true;
            } else {
                fail("unknown Mortier record field: " + field);
            }
            skip_space();
            if (consume('}')) break;
            expect(',');
        }
        if (!have_t1 || !have_t2 || !have_seed)
            fail("Mortier record must contain exactly T1, T2, and Seed");
        return result;
    }

    std::vector<uint8_t> record_name(const std::string& name, int& k) {
        if (name.size() < 6 || name[0] != 'k' || name[1] < '0' || name[1] > '9'
                || name[2] < '0' || name[2] > '9' || name[3] != '_')
            fail("invalid Mortier record name");
        std::string hex = name.substr(4);
        if (hex.size() != 64)
            fail("invalid Mortier stable id in record name");
        for (char value : hex) {
            bool valid = (value >= '0' && value <= '9')
                || (value >= 'a' && value <= 'f')
                || (value >= 'A' && value <= 'F');
            if (!valid) fail("invalid Mortier stable id in record name");
        }
        k = (name[1] - '0') * 10 + (name[2] - '0');
        return parse_hex_id(hex);
    }

    std::istream& input_;
    size_t offset_ = 0;
};

} // namespace

std::vector<uint8_t> encode_mortier_record(const MortierRecord& record) {
    if (record.seeds.size() > MAX_SEEDS)
        throw std::runtime_error("Mortier seed count exceeds codec limit");

    std::vector<Z4Point> seeds = record.seeds;
    std::sort(seeds.begin(), seeds.end(), [](const Z4Point& left, const Z4Point& right) {
        return left.coordinates < right.coordinates;
    });

    std::vector<uint8_t> out;
    out.reserve(std::min<size_t>(MAX_ENCODED_RECORD_SIZE, 18 + seeds.size() * 8));
    put_varint(out, MortierStore::CODEC_VERSION);
    encode_point(out, record.t1);
    encode_point(out, record.t2);
    put_varint(out, seeds.size());

    std::array<uint64_t, 4> previous{};
    for (const Z4Point& seed : seeds) {
        for (size_t i = 0; i < 4; ++i) {
            uint64_t current = signed_bits(seed.coordinates[i]);
            int64_t delta = bits_signed(current - previous[i]);
            put_varint(out, zigzag_encode(delta));
            previous[i] = current;
        }
    }
    return out;
}

MortierRecord decode_mortier_record(const uint8_t* data, size_t size) {
    if (size == 0 || size > MAX_ENCODED_RECORD_SIZE)
        throw std::runtime_error("invalid Mortier record size");
    Decoder decoder(data, size);
    if (decoder.varint() != MortierStore::CODEC_VERSION)
        throw std::runtime_error("unsupported Mortier record codec version");

    MortierRecord record;
    record.t1 = decode_point(decoder);
    record.t2 = decode_point(decoder);
    uint64_t seed_count = decoder.varint();
    if (seed_count > MAX_SEEDS || seed_count > size / 4)
        throw std::runtime_error("invalid Mortier seed count");
    record.seeds.reserve(static_cast<size_t>(seed_count));

    std::array<uint64_t, 4> previous{};
    for (uint64_t seed_index = 0; seed_index < seed_count; ++seed_index) {
        Z4Point seed;
        for (size_t i = 0; i < 4; ++i) {
            previous[i] += signed_bits(decoder.integer());
            seed.coordinates[i] = bits_signed(previous[i]);
        }
        if (!record.seeds.empty()
                && seed.coordinates < record.seeds.back().coordinates)
            throw std::runtime_error("non-canonical Mortier seed order");
        record.seeds.push_back(seed);
    }
    if (!decoder.empty())
        throw std::runtime_error("trailing bytes in Mortier record");
    return record;
}

MortierStore::MortierStore(const std::string& path, size_t chunk_target)
    : MortierStore(path, DEFAULT_Z4_BASIS, chunk_target) {}

MortierStore::MortierStore(const std::string& path, const std::string& z4_basis,
                           size_t chunk_target)
    : chunk_target_(chunk_target) {
    if (z4_basis.empty()) throw std::invalid_argument("Mortier z4 basis is empty");
    if (chunk_target == 0 || chunk_target > MAX_CHUNK_SIZE)
        throw std::invalid_argument("invalid Mortier chunk target");
    if (fs::exists(path))
        throw std::runtime_error("Mortier database already exists: " + path);
    fs::path parent = fs::path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent);

    int rc = sqlite3_open_v2(path.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        std::string message = db_ ? sqlite3_errmsg(db_) : "cannot allocate SQLite handle";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("open Mortier database: " + message);
    }

    try {
        exec(db_, "PRAGMA journal_mode=DELETE;");
        exec(db_, "PRAGMA synchronous=FULL;");
        exec(db_, "PRAGMA foreign_keys=ON;");
        exec(db_,
             "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
             "CREATE TABLE chunks("
             " id INTEGER PRIMARY KEY,record_count INTEGER NOT NULL CHECK(record_count>0),"
             " uncompressed_size INTEGER NOT NULL CHECK(uncompressed_size>0),"
             " compressed_size INTEGER NOT NULL CHECK(compressed_size>=0),data BLOB NOT NULL);"
             "CREATE TABLE entries("
             " id INTEGER PRIMARY KEY,stable_id BLOB NOT NULL UNIQUE,k INTEGER NOT NULL CHECK(k>=0),"
             " chunk_id INTEGER NOT NULL REFERENCES chunks(id),"
             " byte_offset INTEGER NOT NULL CHECK(byte_offset>=0),"
             " byte_length INTEGER NOT NULL CHECK(byte_length>0),"
             " seed_count INTEGER NOT NULL CHECK(seed_count>=0),signature TEXT);"
             "CREATE INDEX entries_chunk ON entries(chunk_id,id);");
        Statement metadata(db_, "INSERT INTO metadata(key,value) VALUES(?,?);");
        const std::pair<const char*, std::string> values[] = {
            {"schema_version", std::to_string(SCHEMA_VERSION)},
            {"codec_version", std::to_string(CODEC_VERSION)},
            {"compression", "zstd"}, {"z4_basis", z4_basis}, {"complete", "0"}
        };
        for (const auto& value : values) {
            check_sqlite(sqlite3_bind_text(metadata.get(), 1, value.first, -1, SQLITE_STATIC),
                         db_, "bind Mortier metadata key");
            bind_text(db_, metadata.get(), 2, value.second);
            check_sqlite(sqlite3_step(metadata.get()), db_, "insert Mortier metadata");
            check_sqlite(sqlite3_reset(metadata.get()), db_, "reset Mortier metadata insert");
            check_sqlite(sqlite3_clear_bindings(metadata.get()), db_,
                         "clear Mortier metadata bindings");
        }
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        std::error_code ignored;
        fs::remove(path, ignored);
        throw;
    }
}

MortierStore::~MortierStore() {
    if (db_) sqlite3_close(db_);
}

void MortierStore::add(const std::vector<uint8_t>& stable_id, int k,
                       const MortierRecord& record,
                       std::optional<std::string> signature) {
    if (finished_) throw std::runtime_error("cannot add to a finished Mortier store");
    if (stable_id.empty()) throw std::invalid_argument("Mortier stable id is empty");
    if (k < 0) throw std::invalid_argument("Mortier k is negative");
    std::vector<uint8_t> encoded = encode_mortier_record(record);
    if (!entries_.empty() && chunk_.size() + encoded.size() > chunk_target_) flush();
    entries_.push_back({stable_id, k, static_cast<int64_t>(chunk_.size()),
                        static_cast<int64_t>(encoded.size()),
                        static_cast<int64_t>(record.seeds.size()), std::move(signature)});
    chunk_.insert(chunk_.end(), encoded.begin(), encoded.end());
}

void MortierStore::flush() {
    if (entries_.empty()) return;
    size_t bound = ZSTD_compressBound(chunk_.size());
    std::vector<uint8_t> compressed(bound);
    size_t compressed_size = ZSTD_compress(compressed.data(), compressed.size(),
                                            chunk_.data(), chunk_.size(), 3);
    if (ZSTD_isError(compressed_size))
        throw std::runtime_error(std::string("compress Mortier chunk: ")
                                 + ZSTD_getErrorName(compressed_size));
    compressed.resize(compressed_size);

    exec(db_, "BEGIN IMMEDIATE;");
    try {
        Statement chunk_statement(db_,
            "INSERT INTO chunks(record_count,uncompressed_size,compressed_size,data)"
            " VALUES(?,?,?,?);");
        sqlite3_stmt* chunk = chunk_statement.get();
        check_sqlite(sqlite3_bind_int64(chunk, 1, entries_.size()), db_,
                     "bind Mortier record count");
        check_sqlite(sqlite3_bind_int64(chunk, 2, chunk_.size()), db_,
                     "bind Mortier chunk size");
        check_sqlite(sqlite3_bind_int64(chunk, 3, compressed.size()), db_,
                     "bind compressed Mortier size");
        bind_blob(db_, chunk, 4, compressed);
        check_sqlite(sqlite3_step(chunk), db_, "insert Mortier chunk");
        int64_t chunk_id = sqlite3_last_insert_rowid(db_);

        Statement entry_statement(db_,
            "INSERT INTO entries(stable_id,k,chunk_id,byte_offset,byte_length,seed_count,signature)"
            " VALUES(?,?,?,?,?,?,?);");
        sqlite3_stmt* entry = entry_statement.get();
        for (const PendingEntry& pending : entries_) {
            bind_blob(db_, entry, 1, pending.stable_id);
            check_sqlite(sqlite3_bind_int(entry, 2, pending.k), db_, "bind Mortier k");
            check_sqlite(sqlite3_bind_int64(entry, 3, chunk_id), db_,
                         "bind Mortier chunk id");
            check_sqlite(sqlite3_bind_int64(entry, 4, pending.offset), db_,
                         "bind Mortier offset");
            check_sqlite(sqlite3_bind_int64(entry, 5, pending.length), db_,
                         "bind Mortier length");
            check_sqlite(sqlite3_bind_int64(entry, 6, pending.seed_count), db_,
                         "bind Mortier seed count");
            if (pending.signature)
                bind_text(db_, entry, 7, *pending.signature);
            else
                check_sqlite(sqlite3_bind_null(entry, 7), db_, "bind null Mortier signature");
            check_sqlite(sqlite3_step(entry), db_, "insert Mortier entry");
            check_sqlite(sqlite3_reset(entry), db_, "reset Mortier entry insert");
            check_sqlite(sqlite3_clear_bindings(entry), db_,
                         "clear Mortier entry bindings");
        }
        exec(db_, "COMMIT;");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
    chunk_.clear();
    entries_.clear();
}

void MortierStore::checkpoint() {
    if (finished_) throw std::runtime_error("cannot checkpoint a finished Mortier store");
    flush();
}

void MortierStore::finish() {
    if (finished_) return;
    flush();
    exec(db_, "PRAGMA optimize;");
    exec(db_, "BEGIN IMMEDIATE;");
    try {
        exec(db_, "UPDATE metadata SET value='1' WHERE key='complete' AND value='0';");
        if (sqlite3_changes(db_) != 1)
            throw std::runtime_error("cannot mark Mortier database complete");
        exec(db_, "COMMIT;");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw;
    }
    int rc = sqlite3_close(db_);
    if (rc != SQLITE_OK)
        throw std::runtime_error(std::string("close Mortier database: ")
                                 + sqlite3_errstr(rc));
    db_ = nullptr;
    finished_ = true;
}

bool MortierStore::is_complete(const std::string& database_path) {
    ReadDatabase database(database_path);
    return database.metadata("complete") == "1";
}

void MortierStore::for_each(
    const std::string& database_path,
    const std::function<void(const MortierStoredEntry&)>& visitor) {
    if (!visitor) throw std::invalid_argument("Mortier visitor is empty");
    read_rows(database_path, "", [](sqlite3*, sqlite3_stmt*) {}, visitor);
}

std::optional<MortierStoredEntry> MortierStore::read_by_id(
    const std::string& database_path, int64_t id) {
    if (id <= 0) throw std::invalid_argument("Mortier entry id must be positive");
    std::optional<MortierStoredEntry> result;
    read_rows(database_path, "WHERE e.id=?",
              [id](sqlite3* db, sqlite3_stmt* statement) {
                  check_sqlite(sqlite3_bind_int64(statement, 1, id), db,
                               "bind Mortier entry id");
              },
              [&result](const MortierStoredEntry& entry) { result = entry; });
    return result;
}

std::optional<MortierStoredEntry> MortierStore::read_by_stable_id(
    const std::string& database_path, const std::vector<uint8_t>& stable_id) {
    if (stable_id.empty()) throw std::invalid_argument("Mortier stable id is empty");
    std::optional<MortierStoredEntry> result;
    read_rows(database_path, "WHERE e.stable_id=?",
              [&stable_id](sqlite3* db, sqlite3_stmt* statement) {
                  bind_blob(db, statement, 1, stable_id);
              },
              [&result](const MortierStoredEntry& entry) { result = entry; });
    return result;
}

std::string format_hex_id(const std::vector<uint8_t>& id) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint8_t byte : id) out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

std::vector<uint8_t> parse_hex_id(const std::string& text) {
    if (text.empty() || text.size() % 2 != 0)
        throw std::invalid_argument("Mortier stable id must contain an even number of hex digits");
    std::vector<uint8_t> result;
    result.reserve(text.size() / 2);
    auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < text.size(); i += 2) {
        int high = digit(text[i]), low = digit(text[i + 1]);
        if (high < 0 || low < 0) throw std::invalid_argument("invalid hex Mortier stable id");
        result.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return result;
}

int64_t export_mortier_json(const std::string& database_path,
                            const std::string& json_path, int64_t id,
                            const std::vector<uint8_t>& stable_id) {
    if (id != 0 && !stable_id.empty())
        throw std::invalid_argument("select a Mortier record by id or stable id, not both");
    std::error_code path_error;
    fs::path database_canonical = fs::weakly_canonical(database_path, path_error);
    if (path_error) throw std::runtime_error("resolve Mortier database path: " + path_error.message());
    fs::path json_canonical = fs::weakly_canonical(json_path, path_error);
    if (path_error) throw std::runtime_error("resolve Mortier JSON path: " + path_error.message());
    if (database_canonical == json_canonical)
        throw std::invalid_argument("Mortier JSON output must differ from its source database");
    fs::path parent = fs::path(json_path).parent_path();
    if (!parent.empty()) fs::create_directories(parent);
    std::string temporary = json_path + ".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot open Mortier JSON output: " + temporary);
    int64_t count = 0;
    out << "{\n";
    auto write = [&](const MortierStoredEntry& entry) {
        if (count++) out << ",\n";
        out << "  \"k" << std::setw(2) << std::setfill('0') << entry.k << "_"
            << format_hex_id(entry.stable_id) << "\": {\n";
        auto point = [&](const Z4Point& value) {
            out << '[';
            for (size_t i = 0; i < 4; ++i) {
                if (i) out << ',';
                out << value.coordinates[i];
            }
            out << ']';
        };
        out << "    \"T1\": "; point(entry.record.t1);
        out << ",\n    \"T2\": "; point(entry.record.t2);
        out << ",\n    \"Seed\": [";
        for (size_t i = 0; i < entry.record.seeds.size(); ++i) {
            if (i) out << ',';
            point(entry.record.seeds[i]);
        }
        out << "]\n  }";
    };
    if (id != 0) {
        auto entry = MortierStore::read_by_id(database_path, id);
        if (!entry) throw std::runtime_error("Mortier entry id not found");
        write(*entry);
    } else if (!stable_id.empty()) {
        auto entry = MortierStore::read_by_stable_id(database_path, stable_id);
        if (!entry) throw std::runtime_error("Mortier stable id not found");
        write(*entry);
    } else {
        MortierStore::for_each(database_path, write);
    }
    out << "\n}\n";
    out.close();
    if (!out) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw std::runtime_error("failed to write Mortier JSON output");
    }
    std::error_code ec;
    fs::rename(temporary, json_path, ec);
    if (ec) throw std::runtime_error("publish Mortier JSON output: " + ec.message());
    return count;
}

int64_t import_mortier_json(const std::string& json_path,
                            const std::string& database_path,
                            size_t chunk_target) {
    if (fs::exists(database_path))
        throw std::runtime_error("Mortier database already exists: " + database_path);
    std::string temporary = database_path + ".tmp";
    if (fs::exists(temporary))
        throw std::runtime_error("Mortier temporary database already exists: " + temporary);

    std::ifstream input(json_path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open Mortier JSON input: " + json_path);

    bool temporary_created = false;
    try {
        int64_t count;
        {
            MortierStore store(temporary, chunk_target);
            temporary_created = true;
            MortierJsonParser parser(input);
            count = parser.parse(store);
            store.finish();
        }
        if (fs::exists(database_path))
            throw std::runtime_error("Mortier database already exists: " + database_path);
        std::error_code ec;
        fs::rename(temporary, database_path, ec);
        if (ec)
            throw std::runtime_error("publish Mortier database: " + ec.message());
        temporary_created = false;
        return count;
    } catch (...) {
        if (temporary_created) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
        }
        throw;
    }
}
