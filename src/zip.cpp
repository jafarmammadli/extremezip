#include "zip.hpp"

#include <windows.h>
#include <libdeflate.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace xx {
namespace {

template <class T> T rd(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof v); return v; }
uint16_t rd16(const uint8_t* p) { return rd<uint16_t>(p); }
uint32_t rd32(const uint8_t* p) { return rd<uint32_t>(p); }
uint64_t rd64(const uint8_t* p) { return rd<uint64_t>(p); }

constexpr uint32_t SIG_EOCD = 0x06054b50, SIG_EOCD64 = 0x06064b50, SIG_LOC64 = 0x07064b50,
                   SIG_CEN = 0x02014b50, SIG_LOC = 0x04034b50;

[[noreturn]] void corrupt(const std::string& m) { throw std::runtime_error("corrupt archive: " + m); }

std::wstring widen(const uint8_t* s, size_t n, UINT cp) {
    if (!n) return {};
    int len = MultiByteToWideChar(cp, 0, (const char*)s, (int)n, nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(cp, 0, (const char*)s, (int)n, w.data(), len);
    return w;
}

uint64_t dos_to_filetime(uint16_t time, uint16_t date) {
    FILETIME local, utc;
    if (!DosDateTimeToFileTime(date, time, &local) || !LocalFileTimeToFileTime(&local, &utc)) return 0;
    return (uint64_t)utc.dwHighDateTime << 32 | utc.dwLowDateTime;
}

bool is_reserved(std::wstring_view c) {
    c = c.substr(0, c.find(L'.'));
    auto eq = [&](const wchar_t* s) { return CompareStringOrdinal(c.data(), (int)c.size(), s, -1, TRUE) == CSTR_EQUAL; };
    if (eq(L"CON") || eq(L"PRN") || eq(L"AUX") || eq(L"NUL")) return true;
    if (c.size() != 4 || c[3] < L'1' || c[3] > L'9') return false;
    return CompareStringOrdinal(c.data(), 3, L"COM", 3, TRUE) == CSTR_EQUAL ||
           CompareStringOrdinal(c.data(), 3, L"LPT", 3, TRUE) == CSTR_EQUAL;
}

// Turns an archive name into a safe relative Windows path. Rejects "..";
// strips leading separators and "." parts; replaces characters Windows forbids.
bool sanitize(const std::wstring& in, std::wstring& out, std::string& err) {
    out.clear();
    size_t i = 0, n = in.size();
    while (i < n) {
        size_t j = i;
        while (j < n && in[j] != L'/' && in[j] != L'\\') ++j;
        std::wstring comp(in, i, j - i);
        i = j + 1;
        if (comp.empty() || comp == L".") continue;
        if (comp == L"..") { err = "unsafe path (contains '..')"; return false; }
        for (auto& ch : comp)
            if (ch < 32 || std::wcschr(L"<>:\"|?*", ch)) ch = L'_';
        if (comp.back() == L'.' || comp.back() == L' ') comp.back() = L'_';
        if (is_reserved(comp)) comp.insert(0, 1, L'_');
        if (!out.empty()) out += L'\\';
        out += comp;
    }
    if (out.empty()) { err = "empty path"; return false; }
    return true;
}

}  // namespace

std::vector<Entry> parse_central_directory(const uint8_t* b, uint64_t size) {
    if (size < 22) corrupt("file too small to be a ZIP");

    // End of central directory: last signature whose comment fits in the file.
    uint64_t lo = size > 22 + 65535 ? size - 22 - 65535 : 0, eocd = UINT64_MAX;
    for (uint64_t p = size - 22;; --p) {
        if (rd32(b + p) == SIG_EOCD && p + 22 + rd16(b + p + 20) <= size) { eocd = p; break; }
        if (p == lo) break;
    }
    if (eocd == UINT64_MAX) corrupt("end of central directory not found (not a ZIP?)");

    uint64_t count = rd16(b + eocd + 10), cd_size = rd32(b + eocd + 12), cd_off = rd32(b + eocd + 16);
    uint64_t cd_limit = eocd;
    if (eocd >= 20 && rd32(b + eocd - 20) == SIG_LOC64) {
        uint64_t off = rd64(b + eocd - 20 + 8);
        if (size >= 56 && off <= size - 56 &&rd32(b + off) == SIG_EOCD64) {
            count = rd64(b + off + 32);
            cd_size = rd64(b + off + 40);
            cd_off = rd64(b + off + 48);
            cd_limit = off;
        } else if (count == 0xFFFF || cd_size == 0xFFFFFFFF || cd_off == 0xFFFFFFFF) {
            corrupt("bad ZIP64 end of central directory");
        }
    }
    if (cd_size > cd_limit || cd_off > cd_limit - cd_size) corrupt("central directory out of bounds");
    // Data prepended to the archive (SFX stubs) shifts every stored offset.
    const uint64_t delta = cd_limit - cd_size - cd_off;

    std::vector<Entry> out;
    out.reserve((size_t)std::min<uint64_t>(count, cd_size / 46));
    uint64_t p = cd_off + delta;
    const uint64_t end = p + cd_size;
    for (uint64_t i = 0; i < count; ++i) {
        if (end - p < 46 || rd32(b + p) != SIG_CEN) corrupt("bad central directory header #" + std::to_string(i));
        const uint8_t* h = b + p;
        const uint16_t nlen = rd16(h + 28), xlen = rd16(h + 30), clen = rd16(h + 32);
        if (end - p - 46 < (uint64_t)nlen + xlen + clen) corrupt("central directory entry #" + std::to_string(i) + " overruns");

        Entry e;
        const unsigned host = rd16(h + 4) >> 8;
        e.flags = rd16(h + 8);
        e.method = rd16(h + 10);
        e.crc = rd32(h + 16);
        e.comp_size = rd32(h + 20);
        e.size = rd32(h + 24);
        const uint32_t eattr = rd32(h + 38);
        e.local_offset = rd32(h + 42);
        const uint8_t* name = h + 46;

        uint64_t ntfs_time = 0, unix_time = 0;
        std::wstring uname;
        const uint8_t *x = name + nlen, *xe = x + xlen;
        while (xe - x >= 4) {
            const uint16_t id = rd16(x), sz = rd16(x + 2);
            const uint8_t* d = x + 4;
            if (xe - d < sz) break;
            if (id == 0x0001) {
                const uint8_t* q = d;
                auto take = [&](uint64_t& v) {
                    if (v != 0xFFFFFFFF) return;
                    if (d + sz - q < 8) corrupt("bad ZIP64 extra field in entry #" + std::to_string(i));
                    v = rd64(q);
                    q += 8;
                };
                take(e.size);
                take(e.comp_size);
                take(e.local_offset);
            } else if (id == 0x000A && sz >= 4) {
                for (const uint8_t* q = d + 4; d + sz - q >= 4;) {
                    const uint16_t tag = rd16(q), ts = rd16(q + 2);
                    if (d + sz - q - 4 < ts) break;
                    if (tag == 1 && ts >= 24) ntfs_time = rd64(q + 4);
                    q += 4 + ts;
                }
            } else if (id == 0x5455 && sz >= 5 && (d[0] & 1)) {
                unix_time = (uint64_t)(int64_t)rd<int32_t>(d + 1) * 10000000 + 116444736000000000ull;
            } else if (id == 0x7075 && sz >= 5 && d[0] == 1 && rd32(d + 1) == libdeflate_crc32(0, name, nlen)) {
                uname = widen(d + 5, sz - 5, CP_UTF8);
            }
            x = d + sz;
        }
        e.mtime = ntfs_time ? ntfs_time : unix_time ? unix_time : dos_to_filetime(rd16(h + 12), rd16(h + 14));

        e.is_dir = nlen && (name[nlen - 1] == '/' || name[nlen - 1] == '\\');
        if (host == 0 || host == 10 || host == 14) {  // MS-DOS / NTFS / VFAT attributes
            if (eattr & FILE_ATTRIBUTE_DIRECTORY) e.is_dir = true;
            e.attrs = eattr & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        } else if (host == 3 && ((eattr >> 16) & 0xF000) == 0x4000) {
            e.is_dir = true;
        }
        e.local_offset += delta;

        if (uname.empty()) uname = widen(name, nlen, (e.flags & 0x800) ? CP_UTF8 : 437);
        if (!sanitize(uname, e.path, e.error)) e.path = uname;  // keep raw name for the error report
        else if (!e.is_dir) {
            if (e.flags & 1) e.error = "encrypted entries are not supported";
            else if (e.method != 0 && e.method != 8) e.error = "unsupported compression method " + std::to_string(e.method);
            else if (e.method == 0 && e.comp_size != e.size) e.error = "stored entry with mismatched sizes";
        }
        out.push_back(std::move(e));
        p += 46 + nlen + xlen + clen;
    }
    return out;
}

bool resolve_data(const uint8_t* b, uint64_t size, Entry& e, std::string& err) {
    const uint64_t lho = e.local_offset;
    if (size < 30 || lho > size - 30 || rd32(b + lho) != SIG_LOC) { err = "bad local header"; return false; }
    if (rd16(b + lho + 8) != e.method) { err = "local/central header method mismatch"; return false; }
    const uint64_t d = lho + 30 + rd16(b + lho + 26) + rd16(b + lho + 28);
    if (d > size || e.comp_size > size - d) { err = "entry data out of bounds (truncated archive?)"; return false; }
    e.data_offset = d;
    return true;
}

}  // namespace xx
