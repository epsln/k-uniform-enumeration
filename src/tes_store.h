#pragma once

#include <cstdint>
#include <functional>
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

class TesStore {
public:
    explicit TesStore(const std::string& path, size_t chunk_target = 8 * 1024 * 1024);
    ~TesStore();

    TesStore(const TesStore&) = delete;
    TesStore& operator=(const TesStore&) = delete;

    void add(const std::string& combo, const std::string& signature,
             const std::string& legacy_filename, const std::string& document);
    void checkpoint();
    void finish();

private:
    struct PendingEntry {
        std::string combo;
        std::string signature;
        std::string legacy_filename;
        int64_t offset;
        int64_t length;
    };

    void flush();

    sqlite3* db_ = nullptr;
    size_t chunk_target_;
    std::string chunk_;
    std::vector<PendingEntry> entries_;
    bool finished_ = false;
};

// Extracts all records, or one record when id is nonzero. Returns the count.
int64_t extract_tes_database(const std::string& database_path,
                             const std::string& output_dir,
                             int64_t id = 0);
