#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

struct TesEntry {
    int64_t id = 0;
    std::string combo;
    std::string signature;
    std::string legacy_filename;
    std::string document;
};

struct TesSourceIdentity {
    std::string canonical_path;
    uintmax_t file_size = 0;
    int64_t entry_count = 0;
    int64_t max_id = 0;
    int schema_version = 0;
    std::string fingerprint;
};

class TesReader {
public:
    // Opening a reader scans and decompresses the complete source once to compute
    // its logical SHA-256 fingerprint. Entry iteration remains streaming.
    explicit TesReader(const std::string& path);
    ~TesReader();

    TesReader(const TesReader&) = delete;
    TesReader& operator=(const TesReader&) = delete;

    const TesSourceIdentity& identity() const { return identity_; }
    void for_each_after(int64_t start_id,
                        const std::function<void(const TesEntry&)>& visitor) const;

private:
    sqlite3* db_ = nullptr;
    TesSourceIdentity identity_;
};

// Schema 2 (written): one zstd chunk per run of entries of a single combo.
// The chunk payload is, per entry, three varint-length-prefixed strings
// (signature, legacy filename, TES document); entry ids are implicit
// (chunks.first_entry + ordinal), so the only per-entry storage is inside the
// compressed chunk. Schema 1 (separate `entries` rows) is still readable.
class TesStore {
public:
    static constexpr int SCHEMA_VERSION = 2;

    // With `resume`, opens an existing unfinished store and appends to it;
    // otherwise `path` must not exist.
    explicit TesStore(const std::string& path, size_t chunk_target = 8 * 1024 * 1024,
                      bool resume = false);
    ~TesStore();

    TesStore(const TesStore&) = delete;
    TesStore& operator=(const TesStore&) = delete;

    void add(const std::string& combo, const std::string& signature,
             const std::string& legacy_filename, const std::string& document);
    void checkpoint();
    void finish();

    // Unit transactions: entries added between begin_unit and commit_unit are
    // committed atomically with the progress row (unit, value). A crash
    // inside a unit leaves the store as it was before begin_unit.
    void begin_unit();
    void commit_unit(const std::string& unit, const std::string& value);
    std::map<std::string, std::string> progress() const;
    std::optional<std::string> metadata(const std::string& key) const;
    void set_metadata(const std::string& key, const std::string& value);

private:
    void flush();
    int64_t combo_id(const std::string& combo);

    sqlite3* db_ = nullptr;
    size_t chunk_target_;
    std::string chunk_;
    std::string chunk_combo_;
    int64_t chunk_records_ = 0;
    int64_t next_entry_ = 1;
    std::map<std::string, int64_t> combo_ids_;
    bool in_unit_ = false;
    bool finished_ = false;
};

// Extracts all records, or one record when id is nonzero. Returns the count.
int64_t extract_tes_database(const std::string& database_path,
                             const std::string& output_dir,
                             int64_t id = 0);
