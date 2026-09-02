#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

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
