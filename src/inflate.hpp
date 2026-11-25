// B + F: Deflate decompression and CRC32. Both libraries pick SIMD paths at runtime (CPUID).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct libdeflate_decompressor;
struct zng_stream_s;

namespace xx {

uint32_t crc32(uint32_t crc, const void* p, size_t n);

// Decompresses one raw Deflate stream on `threads` worker threads (pinflate.cpp); sink gets
// the output in order. On failure err is set; err == "retry" means the stream has a block too
// large for this decoder's memory guard and should be decompressed with Inflater::stream instead.
bool parallel_inflate(const uint8_t* in, uint64_t in_n, uint64_t out_n, unsigned threads, size_t chunk_bytes,
                      const std::function<bool(const uint8_t*, size_t)>& sink, std::string& err);

// One per worker thread; reuses its state across entries.
class Inflater {
public:
    Inflater();
    ~Inflater();
    Inflater(const Inflater&) = delete;
    Inflater& operator=(const Inflater&) = delete;

    // Whole-buffer decompression (libdeflate). Output must be exactly out_n bytes.
    bool one_shot(const uint8_t* in, size_t in_n, uint8_t* out, size_t out_n, std::string& err);

    // Streaming decompression (zlib-ng) for entries too big for one buffer.
    // sink gets each produced chunk; returning false aborts with err = "cancelled".
    bool stream(const uint8_t* in, uint64_t in_n, uint64_t out_n,
                const std::function<bool(const uint8_t*, size_t)>& sink, std::string& err);

    static constexpr size_t kChunk = 4 << 20;

private:
    libdeflate_decompressor* d_;
    std::unique_ptr<zng_stream_s> z_;
    std::unique_ptr<uint8_t[]> chunk_;
};

}  // namespace xx
