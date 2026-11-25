#include "inflate.hpp"

#include <libdeflate.h>
#include <zlib-ng.h>

#include <algorithm>
#include <stdexcept>

namespace xx {

uint32_t crc32(uint32_t crc, const void* p, size_t n) { return libdeflate_crc32(crc, p, n); }

Inflater::Inflater()
    : d_(libdeflate_alloc_decompressor()), z_(std::make_unique<zng_stream>()),
      chunk_(std::make_unique_for_overwrite<uint8_t[]>(kChunk)) {
    if (!d_ || zng_inflateInit2(z_.get(), -15) != Z_OK) throw std::runtime_error("decompressor init failed");
}

Inflater::~Inflater() {
    zng_inflateEnd(z_.get());
    libdeflate_free_decompressor(d_);
}

bool Inflater::one_shot(const uint8_t* in, size_t in_n, uint8_t* out, size_t out_n, std::string& err) {
    switch (libdeflate_deflate_decompress(d_, in, in_n, out, out_n, nullptr)) {
        case LIBDEFLATE_SUCCESS: return true;
        case LIBDEFLATE_SHORT_OUTPUT: err = "uncompressed data shorter than declared size"; return false;
        case LIBDEFLATE_INSUFFICIENT_SPACE: err = "uncompressed data longer than declared size"; return false;
        default: err = "corrupt deflate data"; return false;
    }
}

bool Inflater::stream(const uint8_t* in, uint64_t in_n, uint64_t out_n,
                      const std::function<bool(const uint8_t*, size_t)>& sink, std::string& err) {
    zng_stream& z = *z_;
    zng_inflateReset(&z);
    z.next_in = in;
    z.avail_in = 0;
    uint64_t in_left = in_n, produced = 0;
    for (;;) {
        if (z.avail_in == 0 && in_left) {
            z.avail_in = (uint32_t)std::min<uint64_t>(in_left, 1u << 30);
            in_left -= z.avail_in;
        }
        z.next_out = chunk_.get();
        z.avail_out = (uint32_t)kChunk;
        const int r = zng_inflate(&z, Z_NO_FLUSH);
        const size_t got = kChunk - z.avail_out;
        if (got) {
            produced += got;
            if (produced > out_n) { err = "uncompressed data longer than declared size"; return false; }
            if (!sink(chunk_.get(), got)) { if (err.empty()) err = "cancelled"; return false; }
        }
        if (r == Z_STREAM_END) break;
        if (r == Z_BUF_ERROR && z.avail_in == 0 && in_left == 0) { err = "truncated deflate data"; return false; }
        if (r != Z_OK && r != Z_BUF_ERROR) { err = "corrupt deflate data"; return false; }
    }
    if (produced != out_n) { err = "uncompressed data shorter than declared size"; return false; }
    return true;
}

}  // namespace xx
