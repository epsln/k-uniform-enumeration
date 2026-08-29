#include "metrics.h"
#include <chrono>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

// -----------------------------------------------------------------------------
// CRC32C (Castagnoli), reflected polynomial 0x82F63B78.
// -----------------------------------------------------------------------------
static uint32_t crc32c_table[256];
static bool crc32c_init = false;

static void init_crc32c() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
        crc32c_table[i] = crc;
    }
    crc32c_init = true;
}

static uint32_t crc32c(const uint8_t* data, size_t len) {
    if (!crc32c_init) init_crc32c();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = crc32c_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// TFRecord masks the CRC before storing it.
static uint32_t masked_crc(uint32_t crc) {
    return ((crc >> 15) | (crc << 17)) + 0xa282ead8u;
}

// -----------------------------------------------------------------------------
// Protobuf wire helpers (little-endian fixed64/fixed32, varint, length-delimited).
// -----------------------------------------------------------------------------
static void put_u8(std::string& s, uint8_t b) { s.push_back((char)b); }

static void put_fixed64(std::string& s, uint64_t v) {
    for (int i = 0; i < 8; ++i) put_u8(s, (uint8_t)(v >> (8 * i)));
}

static void put_fixed32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i) put_u8(s, (uint8_t)(v >> (8 * i)));
}

static void put_varint(std::string& s, uint64_t v) {
    while (v >= 0x80) { put_u8(s, (uint8_t)(v) | 0x80); v >>= 7; }
    put_u8(s, (uint8_t)v);
}

// Length-delimited field: tag byte + varint length + payload.
static void put_ld(std::string& s, uint8_t tag, const std::string& payload) {
    put_u8(s, tag);
    put_varint(s, (uint64_t)payload.size());
    s += payload;
}

// -----------------------------------------------------------------------------
// MetricsWriter
// -----------------------------------------------------------------------------
MetricsWriter::MetricsWriter(const std::string& dir) {
    fs::create_directories(dir);
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    std::string path = dir + "/events.out.tfevents." + std::to_string(now);
    out_.open(path, std::ios::binary | std::ios::trunc);

    // Standard file_version event: Event { file_version = 3 : "brain.Event:2" }
    std::string ev;
    put_ld(ev, 0x1A, "brain.Event:2");  // field 3 (string), wire type 2
    write_record(ev);
}

MetricsWriter::~MetricsWriter() {
    if (out_.is_open()) out_.close();
}

void MetricsWriter::write_record(const std::string& data) {
    // 8-byte length (LE) + 4-byte masked CRC32C of the length bytes.
    uint8_t len_bytes[8];
    uint64_t len = (uint64_t)data.size();
    for (int i = 0; i < 8; ++i) len_bytes[i] = (uint8_t)(len >> (8 * i));
    uint32_t len_crc = masked_crc(crc32c(len_bytes, 8));
    uint32_t data_crc = masked_crc(crc32c((const uint8_t*)data.data(), data.size()));

    out_.write((const char*)len_bytes, 8);
    out_.write((const char*)&len_crc, 4);
    out_.write(data.data(), data.size());
    out_.write((const char*)&data_crc, 4);
    out_.flush();
}

void MetricsWriter::write_scalar(const std::string& tag, int64_t step,
                                 double wall_time, double value) {
    // Summary.Value { tag = 1 : string, simple_value = 2 : float }
    std::string value_msg;
    put_ld(value_msg, 0x0A, tag);                 // field 1 (tag), wire type 2
    put_u8(value_msg, 0x15);                      // field 2 (simple_value), wire type 5
    put_fixed32(value_msg, (uint32_t)0);
    {
        // overwrite the last 4 bytes with the float bits
        float f = (float)value;
        uint32_t bits; std::memcpy(&bits, &f, 4);
        for (int i = 0; i < 4; ++i) value_msg[value_msg.size() - 4 + i] = (char)(bits >> (8 * i));
    }

    // Summary { value = 1 : repeated Value }
    std::string summary_msg;
    put_ld(summary_msg, 0x0A, value_msg);         // field 1 (value), wire type 2

    // Event { wall_time = 1 : double, step = 2 : int64, summary = 5 : Summary }
    std::string ev;
    put_u8(ev, 0x09);                             // field 1 (wall_time), wire type 1
    { double d = wall_time; uint64_t bits; std::memcpy(&bits, &d, 8); put_fixed64(ev, bits); }
    put_u8(ev, 0x10);                             // field 2 (step), wire type 0
    put_varint(ev, (uint64_t)step);
    put_ld(ev, 0x2A, summary_msg);                // field 5 (summary), wire type 2

    std::lock_guard<std::mutex> lk(mu_);
    write_record(ev);
}
