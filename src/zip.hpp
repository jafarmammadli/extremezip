// A: ZIP archive parser (central directory, ZIP64, timestamps, names).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace xx {

struct Entry {
    std::wstring path;         // sanitized relative path, '\' separated
    std::string error;         // non-empty: entry cannot be extracted (unsafe path, unsupported, ...)
    uint64_t comp_size = 0;
    uint64_t size = 0;
    uint64_t local_offset = 0; // absolute offset of local header in the file
    uint64_t data_offset = 0;  // filled by resolve_data()
    uint64_t mtime = 0;        // FILETIME (UTC), 0 = unknown
    uint32_t crc = 0;
    uint32_t attrs = 0;        // FILE_ATTRIBUTE_READONLY/HIDDEN/SYSTEM subset
    uint16_t method = 0;
    uint16_t flags = 0;
    bool is_dir = false;
};

// Parses the central directory. Throws std::runtime_error on structural corruption.
std::vector<Entry> parse_central_directory(const uint8_t* base, uint64_t size);

// Validates the local header and sets e.data_offset. Returns false and sets err on failure.
bool resolve_data(const uint8_t* base, uint64_t size, Entry& e, std::string& err);

}  // namespace xx
