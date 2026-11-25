// Parallel decompression of ONE raw Deflate stream (rapidgzip-style speculative decoding).
//
// The compressed stream is cut into fixed-size chunks. A worker per chunk searches its chunk
// for the first position that parses and decodes as a valid Deflate block, then decodes from
// there into 16-bit symbols: 0-255 are bytes, 256+ are "markers" naming a byte of the unknown
// 32 KiB window that precedes the chunk. It stops at the first block boundary at or past the
// next chunk's nominal start, which is exactly where the next worker's search lands.
//
// The consumer (calling thread) walks the stream in order. A chunk is accepted only if it starts
// precisely at the confirmed end of the previous one; markers are then replaced from the known
// window. Anything else (false boundary, fixed-Huffman boundary the search skips, corrupt data)
// is decoded serially from the confirmed position, so the output is always exact. CRC32 over
// the result is still checked by the caller.
#include "inflate.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace xx {
namespace {

constexpr int TB = 10;                       // primary Huffman table bits
constexpr size_t kWin = 32768;
constexpr size_t kHardLimit = 256ull << 20;  // per-decode output limit (memory guard), bytes

constexpr uint16_t LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct Bits {
    const uint8_t* d;
    uint64_t n;    // bytes
    uint64_t pos;  // bit position
    uint64_t peek() const {  // at least 57 valid bits; zeros past the end
        uint64_t i = pos >> 3, v = 0;
        if (i + 8 <= n) std::memcpy(&v, d + i, 8);
        else for (uint64_t k = 0; i + k < n && k < 8; ++k) v |= (uint64_t)d[i + k] << (8 * k);
        return v >> (pos & 7);
    }
    uint32_t get(int c) { uint32_t v = (uint32_t)(peek() & ((1ull << c) - 1)); pos += c; return v; }
    bool over() const { return pos > n * 8; }
};

enum class Code { lengths, dynamic, fixed };

struct Huff {
    uint16_t tab[1 << TB];  // (symbol << 4) | length, 0 = longer than TB bits
    uint16_t count[16];
    uint16_t sym[288];

    bool build(const uint8_t* lens, int n, Code kind) {
        std::memset(count, 0, sizeof count);
        for (int i = 0; i < n; ++i) count[lens[i]]++;
        count[0] = 0;
        int left = 1, maxlen = 0;
        for (int len = 1; len < 16; ++len) {
            left = (left << 1) - count[len];
            if (left < 0) return false;  // over-subscribed
            if (count[len]) maxlen = len;
        }
        // zlib's rules: code-length codes must be complete; others may be incomplete only
        // when they hold at most one code. The fixed distance code is incomplete by design.
        if (left > 0 && (kind == Code::lengths || (kind == Code::dynamic && maxlen > 1))) return false;
        uint16_t offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = offs[len] + count[len];
        for (int i = 0; i < n; ++i) if (lens[i]) sym[offs[lens[i]]++] = (uint16_t)i;
        std::memset(tab, 0, sizeof tab);
        for (int len = 1, code = 0, k = 0; len <= TB; ++len, code <<= 1)
            for (int c = 0; c < count[len]; ++c, ++code, ++k) {
                int r = 0;
                for (int b = 0; b < len; ++b) r |= ((code >> b) & 1) << (len - 1 - b);
                for (int j = r; j < (1 << TB); j += 1 << len) tab[j] = (uint16_t)(sym[k] << 4 | len);
            }
        return true;
    }

    int decode(Bits& b) const {
        if (uint16_t e = tab[b.peek() & ((1 << TB) - 1)]) { b.pos += e & 15; return e >> 4; }
        int code = 0, first = 0, index = 0;  // canonical bit-by-bit decode for long codes
        for (int len = 1; len < 16; ++len) {
            code |= (int)b.get(1);
            int c = count[len];
            if (code - c < first) return sym[index + (code - first)];
            index += c;
            first = (first + c) << 1;
            code <<= 1;
        }
        return -1;
    }
};

const Huff& fixed_lit() {
    static const Huff h = [] {
        Huff x;
        uint8_t l[288];
        std::fill(l, l + 144, uint8_t{8}), std::fill(l + 144, l + 256, uint8_t{9}), std::fill(l + 256, l + 280, uint8_t{7}), std::fill(l + 280, l + 288, uint8_t{8});
        x.build(l, 288, Code::fixed);
        return x;
    }();
    return h;
}
const Huff& fixed_dist() {
    static const Huff h = [] {
        Huff x;
        uint8_t l[30];
        std::fill(l, l + 30, uint8_t{5});
        x.build(l, 30, Code::fixed);
        return x;
    }();
    return h;
}

struct Chunk {
    std::unique_ptr<uint16_t[]> out;
    std::unique_ptr<uint8_t[]> bytes;  // out narrowed to bytes; markers left for the consumer
    size_t size = 0, cap = 0, bytes_cap = 0;
    size_t marker_end = 0;              // no markers at or after this index
    uint64_t start = 0, end = 0;
    bool ok = false, final = false, too_big = false;

    void reserve(size_t need) {
        if (need <= cap) return;
        size_t nc = std::max(need, cap * 2 + (1 << 20));
        auto p = std::make_unique_for_overwrite<uint16_t[]>(nc);
        if (size) std::memcpy(p.get(), out.get(), size * 2);
        out = std::move(p), cap = nc;
    }
};

// Decodes blocks from bit `start`. Stops at the first block boundary at or after `stop_bit`
// (after at least one block), after the final block, at a boundary once `soft_cap` symbols
// exist, or after `max_blocks` blocks (0 = unlimited). Returns false on invalid data.
bool decode(const uint8_t* in, uint64_t in_n, uint64_t start, uint64_t stop_bit, size_t soft_cap, int max_blocks, Chunk& c) {
    Bits b{in, in_n, start};
    c.start = start, c.size = 0, c.final = c.too_big = false;
    Huff lit, dist;
    for (int blocks = 0;; ++blocks) {
        if (blocks && (b.pos >= stop_bit || c.size >= soft_cap || blocks == max_blocks)) break;
        const bool last = b.get(1);
        const int type = (int)b.get(2);
        if (type == 0) {
            b.pos = (b.pos + 7) & ~7ull;
            const uint32_t len = b.get(16), nlen = b.get(16);
            if ((len ^ 0xFFFF) != nlen || b.over() || len > in_n - (b.pos >> 3)) return false;
            if (c.size + len > kHardLimit) { c.too_big = true; return false; }
            c.reserve(c.size + len);
            const uint8_t* s = in + (b.pos >> 3);
            for (uint32_t i = 0; i < len; ++i) c.out[c.size + i] = s[i];
            c.size += len;
            b.pos += (uint64_t)len * 8;
        } else if (type == 3) {
            return false;
        } else {
            const Huff *pl = &fixed_lit(), *pd = &fixed_dist();
            if (type == 2) {
                const int hlit = (int)b.get(5) + 257, hdist = (int)b.get(5) + 1, hclen = (int)b.get(4) + 4;
                if (hlit > 286 || hdist > 30) return false;
                static constexpr uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                uint8_t cl[19] = {}, lens[320];
                for (int i = 0; i < hclen; ++i) cl[order[i]] = (uint8_t)b.get(3);
                Huff clh;
                if (!clh.build(cl, 19, Code::lengths)) return false;
                for (int n = 0; n < hlit + hdist;) {
                    const int s = clh.decode(b);
                    if (s < 0) return false;
                    if (s < 16) { lens[n++] = (uint8_t)s; continue; }
                    int rep;
                    uint8_t val = 0;
                    if (s == 16) { if (!n) return false; val = lens[n - 1]; rep = 3 + (int)b.get(2); }
                    else if (s == 17) rep = 3 + (int)b.get(3);
                    else rep = 11 + (int)b.get(7);
                    if (n + rep > hlit + hdist) return false;
                    while (rep--) lens[n++] = val;
                }
                if (!lens[256] || !lit.build(lens, hlit, Code::dynamic) || !dist.build(lens + hlit, hdist, Code::dynamic)) return false;
                pl = &lit, pd = &dist;
            }
            constexpr uint64_t M = (1 << TB) - 1;
            for (;;) {
                if (c.size + 258 + 8 > c.cap) c.reserve(c.size + 258 + 8);  // +8: copy overshoot
                // One 57-bit peek covers a table-hit literal/length code (10) + length extra (5)
                // + distance code (10) + distance extra (13).
                uint64_t v = b.peek();
                int s;
                if (const uint16_t e = pl->tab[v & M]) s = e >> 4, v >>= e & 15, b.pos += e & 15;
                else s = pl->decode(b), v = b.peek();
                if (s < 256) {
                    if (s < 0) return false;
                    c.out[c.size++] = (uint16_t)s;
                    continue;
                }
                if (s == 256) break;
                if (s > 285) return false;
                const int le = LEXT[s - 257];
                const uint32_t len = LBASE[s - 257] + (uint32_t)(v & ((1u << le) - 1));
                v >>= le, b.pos += le;
                int ds;
                if (const uint16_t e = pd->tab[v & M]) ds = e >> 4, v >>= e & 15, b.pos += e & 15;
                else ds = pd->decode(b), v = b.peek();
                if (ds < 0 || ds > 29) return false;
                const uint32_t d = DBASE[ds] + (uint32_t)(v & ((1u << DEXT[ds]) - 1));
                b.pos += DEXT[ds];
                if (b.over()) return false;
                if (c.size + len > kHardLimit) { c.too_big = true; return false; }
                uint16_t* dst = c.out.get() + c.size;
                if (d <= c.size) {
                    const uint16_t* src = dst - d;
                    if (d >= 8)  // 8 symbols per step; may write up to 7 past len (reserved above)
                        for (uint32_t k = 0; k < len; k += 8) std::memcpy(dst + k, src + k, 16);
                    else for (uint32_t k = 0; k < len; ++k) dst[k] = src[k];
                } else {  // reaches into the unknown window before the chunk: emit markers
                    for (uint32_t k = 0; k < len; ++k) {
                        const int64_t q = (int64_t)c.size + k - d;
                        dst[k] = q >= 0 ? c.out[q] : (uint16_t)(256 + (int64_t)kWin + q);
                    }
                }
                c.size += len;
            }
        }
        if (b.over()) return false;
        if (last) { c.final = true; break; }
    }
    c.end = b.pos;
    return true;
}

// Narrows symbols to bytes on the decoding thread, so the in-order consumer only has to patch
// the (usually short) prefix that still references the previous chunk's window.
void narrow(Chunk& c) {
    if (c.bytes_cap < c.size) c.bytes = std::make_unique_for_overwrite<uint8_t[]>(c.bytes_cap = c.size);
    const uint16_t* s = c.out.get();
    uint8_t* o = c.bytes.get();
    size_t last = 0;
    for (size_t i = 0; i < c.size; ++i) o[i] = (uint8_t)s[i];
    for (size_t i = c.size; i-- > 0;)
        if (s[i] > 255) { last = i + 1; break; }
    c.marker_end = last;
}

// Cheap header pre-check before a trial decode at bit p.
bool plausible(const Bits& b) {
    const uint64_t v = b.peek();
    const int type = (int)(v >> 1) & 3;
    if (type == 2) return ((v >> 3) & 31) <= 29 && ((v >> 8) & 31) <= 29;
    if (type != 0) return false;  // fixed blocks have no header to recognise
    const int skip = (int)((8 - ((b.pos + 3) & 7)) & 7);
    if ((v >> 3) & ((1u << skip) - 1)) return false;  // padding must be zero
    Bits s{b.d, b.n, (b.pos + 3 + skip)};
    const uint64_t w = s.peek();
    return ((w & 0xFFFF) ^ 0xFFFF) == ((w >> 16) & 0xFFFF);
}

}  // namespace

bool parallel_inflate(const uint8_t* in, uint64_t in_n, uint64_t out_n, unsigned threads, size_t chunk_bytes,
                      const std::function<bool(const uint8_t*, size_t)>& sink, std::string& err) {
    chunk_bytes = std::max<size_t>(chunk_bytes, 4096);
    const size_t nchunks = (size_t)((in_n + chunk_bytes - 1) / chunk_bytes);
    const size_t soft_cap = std::max<size_t>(64ull << 20, chunk_bytes * 16);
    const size_t inflight = threads + 2;
    const auto nominal = [&](size_t i) { return (uint64_t)i * chunk_bytes * 8; };

    std::vector<std::unique_ptr<Chunk>> res(nchunks);
    std::mutex m;
    std::condition_variable cv;
    size_t consumed = 0;
    bool abort = false;
    std::atomic<size_t> next{0};

    // Chunk buffers are recycled: fresh multi-MB allocations page-fault, and page faults from many
    // threads serialize in the kernel's memory manager.
    std::vector<std::unique_ptr<Chunk>> spare;

    auto speculate = [&](size_t i, Chunk& c, Chunk& probe) {
        const uint64_t lo = nominal(i), hi = std::min(nominal(i + 1), in_n * 8);
        const uint64_t stop = i + 1 < nchunks ? nominal(i + 1) : UINT64_MAX;
        c.ok = false;
        for (uint64_t p = lo; p < hi; ++p) {
            if (i && (!plausible(Bits{in, in_n, p}) || !decode(in, in_n, p, UINT64_MAX, SIZE_MAX, 2, probe))) continue;
            if ((c.ok = decode(in, in_n, p, stop, soft_cap, 0, c))) narrow(c);
            return;
        }
    };

    std::vector<std::jthread> pool;
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            Chunk probe;
            for (size_t i; (i = next.fetch_add(1)) < nchunks;) {
                std::unique_ptr<Chunk> c;
                {
                    std::unique_lock l(m);
                    cv.wait(l, [&] { return abort || i < consumed + inflight; });
                    if (abort) return;
                    if (!spare.empty()) c = std::move(spare.back()), spare.pop_back();
                }
                if (!c) c = std::make_unique<Chunk>();
                try { speculate(i, *c, probe); } catch (...) { c->ok = false; }
                { std::lock_guard l(m); res[i] = std::move(c); }
                cv.notify_all();
            }
        });
    struct Stop {  // on every exit path: release waiting workers, then join
        std::mutex& m; std::condition_variable& cv; bool& abort; std::vector<std::jthread>& pool;
        ~Stop() { { std::lock_guard l(m); abort = true; } cv.notify_all(); pool.clear(); }
    } stop_guard{m, cv, abort, pool};

    auto wait = [&](size_t j) -> Chunk& {
        std::unique_lock l(m);
        cv.wait(l, [&] { return res[j] != nullptr; });
        return *res[j];
    };
    auto release = [&](size_t j) {
        { std::lock_guard l(m); spare.push_back(std::move(res[j])); consumed = j + 1; }
        cv.notify_all();
    };

    uint8_t window[kWin];
    size_t wlen = 0;
    uint64_t pos = 0, produced = 0;
    bool final = false;
    size_t j = 0;
    Chunk serial;
    size_t n_spec = 0, n_serial = 0;  // XX_DEBUG=1 prints how often speculation paid off
    while (!final) {
        while (j < nchunks) {  // drop speculative results that can no longer be used
            Chunk& r = wait(j);
            if (r.ok && r.start >= pos) break;
            release(j++);
        }
        Chunk* use;
        if (j < nchunks && res[j]->start == pos) {
            use = res[j].get();
            ++n_spec;
        } else {
            const uint64_t target = j < nchunks ? res[j]->start : UINT64_MAX;
            if (!decode(in, in_n, pos, target, soft_cap, 0, serial)) {
                err = serial.too_big ? "retry" : "corrupt deflate data";
                return false;
            }
            narrow(serial);
            use = &serial;
            ++n_serial;
        }

        // Resolve window markers into bytes, slide the window, hand the bytes to the caller.
        const size_t n = use->size;
        if (produced + n > out_n) { err = "uncompressed data longer than declared size"; return false; }
        const uint16_t* s = use->out.get();
        uint8_t* o = use->bytes.get();
        const size_t min_idx = kWin - wlen;
        for (size_t i = 0; i < use->marker_end; ++i) {
            if (s[i] < 256) continue;
            const size_t idx = s[i] - 256u;
            if (idx < min_idx) { err = "corrupt deflate data (distance too far back)"; return false; }
            o[i] = window[idx];
        }
        if (n >= kWin) std::memcpy(window, o + n - kWin, kWin);
        else { std::memmove(window, window + n, kWin - n); std::memcpy(window + kWin - n, o, n); }
        wlen = std::min(kWin, wlen + n);
        produced += n;
        pos = use->end;
        final = use->final;
        if (n && !sink(o, n)) { if (err.empty()) err = "cancelled"; return false; }
        if (use != &serial) release(j++);
        if (!final && pos >= in_n * 8) { err = "truncated deflate data"; return false; }
    }
#pragma warning(suppress : 4996)  // read-only getenv is fine
    if (std::getenv("XX_DEBUG")) std::fprintf(stderr, "pinflate: %zu chunks, %zu speculative, %zu serial\n", nchunks, n_spec, n_serial);
    if (produced != out_n) { err = "uncompressed data shorter than declared size"; return false; }
    return true;
}

}  // namespace xx
