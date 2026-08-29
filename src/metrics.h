#pragma once
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

// Minimal TensorBoard (TFEvents) writer.  Emits a growing event file that
// TensorBoard can read live with `tensorboard --logdir <dir>`.
//
// Format implemented here (no external dependencies):
//   - TFRecord framing: u64 len | u32 masked_crc32c(len) | data | u32 masked_crc32c(data)
//   - Protobuf wire encoding for Event { wall_time=1, step=2, summary=5 } and
//     Summary.Value { tag=1, simple_value=2 }.
class MetricsWriter {
public:
    // Opens <dir>/events.out.tfevents.<unix_ts> and writes the file_version event.
    explicit MetricsWriter(const std::string& dir);
    ~MetricsWriter();

    // Write a scalar time series point.
    void write_scalar(const std::string& tag, int64_t step, double wall_time, double value);

private:
    std::ofstream out_;
    std::mutex mu_;

    void write_record(const std::string& data);
};
