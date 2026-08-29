#pragma once
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <istream>
#include <memory>
#include <streambuf>
#include <string>
#include <vector>
#include <zstd.h>

// Streaming zstd-decompressing std::istream over a file, plus a one-shot
// compress_to_zst helper.  Used by the pruner to read raw solutions straight
// from .zst files so raw solutions never have to be decompressed to disk.

inline bool has_zst_suffix(const std::string& s) {
    return s.size() > 4 && s.compare(s.size() - 4, 4, ".zst") == 0;
}

class ZstdStreamBuf : public std::streambuf {
public:
    explicit ZstdStreamBuf(std::string path) : path_(std::move(path)) {
        in_.open(path_, std::ios::binary);
        dctx_ = ZSTD_createDStream();
        if (dctx_) ZSTD_initDStream(dctx_);
        in_buf_.resize(CHUNK);
        out_buf_.resize(CHUNK);
    }

    ~ZstdStreamBuf() override {
        if (dctx_) ZSTD_freeDStream(dctx_);
    }

    bool good() const { return in_.is_open() && dctx_ != nullptr; }

protected:
    int_type underflow() override {
        if (gptr() && gptr() < egptr()) return traits_type::to_int_type(*gptr());
        setg(nullptr, nullptr, nullptr);
        if (!good()) return traits_type::eof();

        for (;;) {
            if (in_len_ == 0) {
                if (in_.eof()) return traits_type::eof();
                in_.read(in_buf_.data(), (std::streamsize)in_buf_.size());
                in_len_ = (size_t)in_.gcount();
                in_pos_ = 0;
                if (in_len_ == 0) continue;
            }

            ZSTD_inBuffer zin{in_buf_.data() + in_pos_, in_len_, 0};
            ZSTD_outBuffer zout{out_buf_.data(), out_buf_.size(), 0};
            size_t rc = ZSTD_decompressStream(dctx_, &zout, &zin);
            in_pos_ += zin.pos;
            in_len_ -= zin.pos;
            if (ZSTD_isError(rc)) return traits_type::eof();

            if (zout.pos > 0) {
                setg(out_buf_.data(), out_buf_.data(), out_buf_.data() + zout.pos);
                return traits_type::to_int_type(out_buf_[0]);
            }

            // No output produced. rc == 0 means a frame ended; reset and
            // continue to the next concatenated frame (zstd frames concatenate).
            if (rc == 0) {
                if (ZSTD_isError(ZSTD_initDStream(dctx_))) return traits_type::eof();
                continue;
            }
            // rc > 0: decompressor needs more input; loop to refill.
        }
    }

private:
    static constexpr size_t CHUNK = 1 << 16;
    std::string path_;
    std::ifstream in_;
    ZSTD_DStream* dctx_ = nullptr;
    std::vector<char> in_buf_;
    size_t in_pos_ = 0;
    size_t in_len_ = 0;
    std::vector<char> out_buf_;
};

class ZstdInputFile : public std::istream {
public:
    explicit ZstdInputFile(const std::string& path) : std::istream(&buf_), buf_(path) {}
    bool good() const { return buf_.good(); }

private:
    ZstdStreamBuf buf_;
};

// Opens `path` for reading as a solution stream.  .zst files are transparently
// stream-decompressed; anything else is opened as a plain binary stream.
// Returns nullptr if the file cannot be opened / is not a valid zstd stream.
inline std::unique_ptr<std::istream> open_solution_istream(const std::string& path) {
    if (has_zst_suffix(path)) {
        auto s = std::make_unique<ZstdInputFile>(path);
        if (!s->good()) return nullptr;
        return std::unique_ptr<std::istream>(std::move(s));
    }
    auto f = std::make_unique<std::ifstream>(path, std::ios::binary);
    if (!*f) return nullptr;
    return std::unique_ptr<std::istream>(std::move(f));
}

// Compress `path` into `path.zst` (overwriting) and remove the plain source.
// Aborts on failure (e.g. disk full) rather than silently losing data.
inline void compress_to_zst(const std::string& path) {
    if (has_zst_suffix(path)) return;

    std::ifstream in(path, std::ios::binary);
    if (!in) return;

    std::string zst = path + ".zst";
    std::ofstream out(zst, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "FATAL: cannot open " << zst << " for writing.\n";
        std::abort();
    }

    ZSTD_CStream* cs = ZSTD_createCStream();
    if (!cs) {
        std::cerr << "FATAL: zstd stream creation failed.\n";
        std::abort();
    }
    ZSTD_initCStream(cs, 3);

    std::vector<char> inbuf(1 << 16);
    std::vector<char> outbuf(ZSTD_CStreamOutSize());
    bool ok = true;

    while (in) {
        in.read(inbuf.data(), (std::streamsize)inbuf.size());
        size_t n = (size_t)in.gcount();
        if (n == 0) break;
        ZSTD_inBuffer zin{inbuf.data(), n, 0};
        while (zin.pos < zin.size) {
            ZSTD_outBuffer zout{outbuf.data(), outbuf.size(), 0};
            size_t r = ZSTD_compressStream(cs, &zout, &zin);
            if (ZSTD_isError(r)) { ok = false; break; }
            out.write(outbuf.data(), (std::streamsize)zout.pos);
        }
        if (!ok) break;
    }

    if (ok) {
        for (;;) {
            ZSTD_outBuffer zout{outbuf.data(), outbuf.size(), 0};
            size_t r = ZSTD_endStream(cs, &zout);
            out.write(outbuf.data(), (std::streamsize)zout.pos);
            if (r == 0) break;
            if (ZSTD_isError(r)) { ok = false; break; }
        }
    }

    ZSTD_freeCStream(cs);
    out.close();

    if (!ok || !out) {
        std::cerr << "FATAL: compression of " << path << " failed (disk full?).\n";
        std::error_code ec;
        std::filesystem::remove(zst, ec);
        std::abort();
    }
    std::filesystem::remove(path);
}
