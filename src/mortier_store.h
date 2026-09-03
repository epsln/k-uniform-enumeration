#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

struct Z4Point {
    std::array<int64_t, 4> coordinates{};
};

struct MortierRecord {
    Z4Point t1;
    Z4Point t2;
    std::vector<Z4Point> seeds;
};

// The encoded seed order is lexicographic, independent of the input order.
std::vector<uint8_t> encode_mortier_record(const MortierRecord& record);
MortierRecord decode_mortier_record(const uint8_t* data, size_t size);
inline MortierRecord decode_mortier_record(const std::vector<uint8_t>& data) {
    return decode_mortier_record(data.data(), data.size());
}

struct MortierStoredEntry {
    int64_t id = 0;
    std::vector<uint8_t> stable_id;
    int k = 0;
    int64_t seed_count = 0;
    std::optional<std::string> signature;
    MortierRecord record;
};

class MortierStore {
public:
    static constexpr int SCHEMA_VERSION = 1;
    static constexpr int CODEC_VERSION = 1;
    static constexpr size_t DEFAULT_CHUNK_TARGET = 8 * 1024 * 1024;
    static constexpr const char* DEFAULT_Z4_BASIS = "e0,e1,e2,e3";

    explicit MortierStore(const std::string& path,
                          size_t chunk_target = DEFAULT_CHUNK_TARGET);
    MortierStore(const std::string& path, const std::string& z4_basis,
                 size_t chunk_target = DEFAULT_CHUNK_TARGET);
    ~MortierStore();

    MortierStore(const MortierStore&) = delete;
    MortierStore& operator=(const MortierStore&) = delete;

    void add(const std::vector<uint8_t>& stable_id, int k,
             const MortierRecord& record,
             std::optional<std::string> signature = std::nullopt);
    void checkpoint();
    void finish();

    // Readers accept both checkpointed and finished databases. Call
    // is_complete() when publication completeness is required.
    static bool is_complete(const std::string& database_path);
    static void for_each(
        const std::string& database_path,
        const std::function<void(const MortierStoredEntry&)>& visitor);
    static std::optional<MortierStoredEntry> read_by_id(
        const std::string& database_path, int64_t id);
    static std::optional<MortierStoredEntry> read_by_stable_id(
        const std::string& database_path, const std::vector<uint8_t>& stable_id);

private:
    struct PendingEntry {
        std::vector<uint8_t> stable_id;
        int k;
        int64_t offset;
        int64_t length;
        int64_t seed_count;
        std::optional<std::string> signature;
    };

    void flush();

    sqlite3* db_ = nullptr;
    size_t chunk_target_;
    std::vector<uint8_t> chunk_;
    std::vector<PendingEntry> entries_;
    bool finished_ = false;
};

// Writes a Mortier-compatible JSON object. With neither selector, exports all.
int64_t export_mortier_json(const std::string& database_path,
                            const std::string& json_path,
                            int64_t id = 0,
                            const std::vector<uint8_t>& stable_id = {});
int64_t import_mortier_json(
    const std::string& json_path, const std::string& database_path,
    size_t chunk_target = MortierStore::DEFAULT_CHUNK_TARGET);
std::vector<uint8_t> parse_hex_id(const std::string& text);
std::string format_hex_id(const std::vector<uint8_t>& id);
