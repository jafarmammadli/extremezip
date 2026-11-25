// C + D + E: parallel extraction scheduler, buffers, filesystem writer. Public engine API.
// The engine never calls into the UI: callers poll Progress from their own thread.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace xx {

struct Options {
    unsigned threads = 0;                   // 0 = auto
    unsigned closers = 0;                   // handle-closing threads, 0 = auto
    uint64_t one_shot_limit = 64ull << 20;  // entries up to this size are inflated in one call (libdeflate)
    uint64_t prealloc_min = 1ull << 20;     // set final file size up front for files at least this big
    uint64_t pinflate_min = 128ull << 20;   // Deflate entries this big are decompressed on several threads
    size_t pinflate_chunk = 4 << 20;        // compressed bytes per parallel-decompression chunk
    bool stop_on_error = false;
};

struct Progress {
    std::atomic<uint64_t> bytes_total{0}, bytes_done{0};
    std::atomic<uint64_t> files_total{0}, files_done{0};
    std::atomic<unsigned> threads{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> pause{false};  // set false + pause.notify_all() to resume
    std::atomic<bool> finished{false};

    void add_error(std::wstring msg) {
        std::lock_guard l(mu_);
        errors_.push_back(std::move(msg));
        error_count.fetch_add(1, std::memory_order_relaxed);
    }
    std::vector<std::wstring> errors() const { std::lock_guard l(mu_); return errors_; }
    std::atomic<size_t> error_count{0};

private:
    mutable std::mutex mu_;
    std::vector<std::wstring> errors_;
};

// Extracts every entry of a ZIP archive into dest. Per-entry problems are recorded in
// Progress and do not stop other entries. Returns the error count.
// Throws std::runtime_error if the archive cannot be opened or its directory is corrupt.
size_t extract(const std::wstring& archive, const std::wstring& dest, const Options& opt, Progress& p);

}  // namespace xx
