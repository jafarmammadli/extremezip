#include "engine.hpp"

#include "inflate.hpp"
#include "zip.hpp"

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace xx {
namespace {

std::string win_error(DWORD code = GetLastError()) {
    char buf[512];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, sizeof buf, nullptr);
    while (n && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == '.')) --n;
    return n ? std::string(buf, n) : "Windows error " + std::to_string(code);
}

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }  // messages are ASCII

struct MappedFile {
    HANDLE file = INVALID_HANDLE_VALUE, map = nullptr;
    const uint8_t* data = nullptr;
    uint64_t size = 0;

    explicit MappedFile(const std::wstring& path) {
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open archive: " + win_error());
        LARGE_INTEGER sz;
        GetFileSizeEx(file, &sz);
        size = (uint64_t)sz.QuadPart;
        if (size == 0) return;
        map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (map) data = (const uint8_t*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
        if (!data) throw std::runtime_error("cannot map archive: " + win_error());
    }
    ~MappedFile() {
        if (data) UnmapViewOfFile(data);
        if (map) CloseHandle(map);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

// CloseHandle on a freshly written file is slow on Windows (filter drivers such as
// Defender scan on close), so workers hand handles to dedicated closer threads.
class Closer {
public:
    explicit Closer(unsigned n) { for (unsigned i = 0; i < n; ++i) t_.emplace_back([this] { run(); }); }
    ~Closer() {
        { std::lock_guard l(m_); done_ = true; }
        cv_.notify_all();
        t_.clear();  // joins
    }
    void push(HANDLE h) {
        if (t_.empty()) { CloseHandle(h); return; }
        {
            std::lock_guard l(m_);
            if (q_.size() < 8192) { q_.push_back(h); h = nullptr; }
        }
        if (h) CloseHandle(h);  // queue full: back-pressure on the worker
        else cv_.notify_one();
    }

private:
    void run() {
        for (;;) {
            HANDLE h;
            {
                std::unique_lock l(m_);
                cv_.wait(l, [&] { return done_ || !q_.empty(); });
                if (q_.empty()) return;
                h = q_.back();
                q_.pop_back();
            }
            CloseHandle(h);
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<HANDLE> q_;
    bool done_ = false;
    std::vector<std::jthread> t_;
};

void no_power_throttling() {
    THREAD_POWER_THROTTLING_STATE s{THREAD_POWER_THROTTLING_CURRENT_VERSION, THREAD_POWER_THROTTLING_EXECUTION_SPEED, 0};
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &s, sizeof s);
}

std::wstring make_root(const std::wstring& dest) {
    DWORD n = GetFullPathNameW(dest.c_str(), 0, nullptr, nullptr);
    std::wstring full(n, L'\0');
    full.resize(GetFullPathNameW(dest.c_str(), n, full.data(), nullptr));
    if (full.empty()) throw std::runtime_error("bad destination path");
    if (full.back() != L'\\') full += L'\\';
    if (full.starts_with(L"\\\\?\\")) return full;
    if (full.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + full.substr(2);
    return L"\\\\?\\" + full;
}

void create_dir_chain(const std::wstring& root) {
    // Skip "\\?\C:\" or "\\?\UNC\server\share\".
    size_t start = root.starts_with(L"\\\\?\\UNC\\") ? root.find(L'\\', root.find(L'\\', 8) + 1) : 7;
    for (size_t i = root.find(L'\\', start); i != std::wstring::npos; i = root.find(L'\\', i + 1))
        CreateDirectoryW(root.substr(0, i).c_str(), nullptr);
    DWORD a = GetFileAttributesW(root.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY))
        throw std::runtime_error("cannot create destination directory: " + win_error());
}

bool write_all(HANDLE h, const uint8_t* p, uint64_t n, std::string& err) {
    while (n) {
        DWORD w;
        if (!WriteFile(h, p, (DWORD)std::min<uint64_t>(n, 1u << 30), &w, nullptr)) { err = win_error(); return false; }
        p += w;
        n -= w;
    }
    return true;
}

void set_times(HANDLE h, uint64_t mtime, uint32_t attrs) {
    if (!mtime && !attrs) return;
    FILE_BASIC_INFO bi{};
    bi.LastWriteTime.QuadPart = (LONGLONG)mtime;
    bi.FileAttributes = attrs;
    SetFileInformationByHandle(h, FileBasicInfo, &bi, sizeof bi);
}

struct Job {
    const MappedFile& mf;
    const std::wstring& root;
    const Options& opt;
    Progress& p;
    Closer& closer;
    unsigned helpers;  // threads available per worker for parallel decompression of one entry
};

class Worker {
public:
    explicit Worker(Job& j) : j_(j) {}

    void extract(Entry& e) {
        std::string err;
        if (!e.error.empty()) return fail(e, e.error);
        if (!resolve_data(j_.mf.data, j_.mf.size, e, err)) return fail(e, err);

        const std::wstring path = j_.root + e.path;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return fail(e, win_error());

        bool ok = write_entry(h, e, err);
        if (ok) {
            set_times(h, e.mtime, e.attrs);
            j_.closer.push(h);
            j_.p.files_done.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        FILE_DISPOSITION_INFO del{TRUE};  // never leave a half-written or corrupt file behind
        SetFileInformationByHandle(h, FileDispositionInfo, &del, sizeof del);
        CloseHandle(h);
        fail(e, err);
    }

private:
    bool checkpoint(std::string& err) {
        while (j_.p.pause.load(std::memory_order_relaxed) && !j_.p.cancel.load()) j_.p.pause.wait(true);
        if (j_.p.cancel.load(std::memory_order_relaxed)) { err = "cancelled"; return false; }
        return true;
    }

    // Writes one chunk, updating CRC and progress. Used by stored and streamed paths.
    bool emit(HANDLE h, const uint8_t* d, size_t n, uint32_t& crc, std::string& err) {
        if (!checkpoint(err)) return false;
        crc = crc32(crc, d, n);
        if (!write_all(h, d, n, err)) return false;
        j_.p.bytes_done.fetch_add(n, std::memory_order_relaxed);
        return true;
    }

    bool write_entry(HANDLE h, const Entry& e, std::string& err) {
        if (e.size >= j_.opt.prealloc_min) {
            FILE_END_OF_FILE_INFO eof{};
            eof.EndOfFile.QuadPart = (LONGLONG)e.size;
            SetFileInformationByHandle(h, FileEndOfFileInfo, &eof, sizeof eof);
        }
        const uint8_t* src = j_.mf.data + e.data_offset;
        uint32_t crc = 0;
        if (e.method == 0) {
            for (uint64_t off = 0; off < e.size; off += Inflater::kChunk)
                if (!emit(h, src + off, (size_t)std::min<uint64_t>(Inflater::kChunk, e.size - off), crc, err)) return false;
        } else if (e.size <= j_.opt.one_shot_limit) {
            if (!checkpoint(err)) return false;
            if (cap_ < e.size || !buf_) {
                cap_ = std::max<size_t>((size_t)e.size, 64 << 10);
                buf_ = std::make_unique_for_overwrite<uint8_t[]>(cap_);
            }
            if (!inf_.one_shot(src, (size_t)e.comp_size, buf_.get(), (size_t)e.size, err)) return false;
            crc = crc32(0, buf_.get(), (size_t)e.size);
            if (crc == e.crc && !write_all(h, buf_.get(), e.size, err)) return false;
            if (crc == e.crc) j_.p.bytes_done.fetch_add(e.size, std::memory_order_relaxed);
        } else {
            uint64_t emitted = 0;
            auto sink = [&](const uint8_t* d, size_t n) { emitted += n; return emit(h, d, n, crc, err); };
            if (j_.helpers >= 2 && e.size >= j_.opt.pinflate_min) {
                if (parallel_inflate(src, e.comp_size, e.size, j_.helpers, j_.opt.pinflate_chunk, sink, err)) goto check;
                if (err != "retry") return false;
                // Stream has a block too big for the parallel decoder: start over sequentially.
                LARGE_INTEGER zero{};
                if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) { err = win_error(); return false; }
                j_.p.bytes_done.fetch_sub(emitted, std::memory_order_relaxed);
                crc = 0, emitted = 0;
                err.clear();
            }
            if (!inf_.stream(src, e.comp_size, e.size, sink, err)) return false;
        }
    check:
        if (crc != e.crc) { err = "CRC mismatch (corrupt data)"; return false; }
        return true;
    }

    void fail(const Entry& e, const std::string& err) {
        j_.p.files_done.fetch_add(1, std::memory_order_relaxed);
        if (err == "cancelled") return;
        j_.p.add_error(e.path + L": " + widen(err));
        if (j_.opt.stop_on_error) j_.p.cancel = true;
    }

    Job& j_;
    Inflater inf_;
    std::unique_ptr<uint8_t[]> buf_;
    size_t cap_ = 0;
};

std::wstring fold(const std::wstring& s) {
    std::wstring r = s;
    CharUpperBuffW(r.data(), (DWORD)r.size());
    return r;
}

}  // namespace

size_t extract(const std::wstring& archive, const std::wstring& dest, const Options& opt, Progress& p) {
    struct Done { Progress& p; ~Done() { p.finished = true; p.finished.notify_all(); } } done{p};

    MappedFile mf(archive);
    std::vector<Entry> entries = parse_central_directory(mf.data, mf.size);
    const std::wstring root = make_root(dest);
    create_dir_chain(root);

    // Plan: last entry wins for duplicate names (Windows names are case-insensitive).
    std::unordered_map<std::wstring, uint32_t> by_name;
    std::vector<uint32_t> files, dirs_with_time;
    std::unordered_set<std::wstring> dirs;
    for (uint32_t i = 0; i < entries.size(); ++i) {
        Entry& e = entries[i];
        if (e.error.empty())
            for (size_t k = e.path.find(L'\\'); k != std::wstring::npos; k = e.path.find(L'\\', k + 1))
                dirs.insert(e.path.substr(0, k));
        if (e.is_dir) {
            if (!e.error.empty()) { p.add_error(e.path + L": " + widen(e.error)); continue; }
            dirs.insert(e.path);
            if (e.mtime) dirs_with_time.push_back(i);
            continue;
        }
        auto [it, fresh] = by_name.try_emplace(fold(e.path), (uint32_t)files.size());
        if (fresh) files.push_back(i);
        else files[it->second] = i;
    }
    std::vector<std::wstring> dir_list(dirs.begin(), dirs.end());
    std::sort(dir_list.begin(), dir_list.end());  // parents sort before children
    for (auto& d : dir_list) CreateDirectoryW((root + d).c_str(), nullptr);

    // Longest-processing-time-first: big entries start early so they don't finish last alone.
    std::sort(files.begin(), files.end(), [&](uint32_t a, uint32_t b) { return entries[a].size > entries[b].size; });
    uint64_t total = 0;
    for (uint32_t i : files) total += entries[i].size;
    p.files_total = files.size();
    p.bytes_total = total;

    MEMORYSTATUSEX ms{sizeof ms};
    GlobalMemoryStatusEx(&ms);
    const uint64_t mem_threads = std::max<uint64_t>(1, ms.ullAvailPhys / 2 / std::max<uint64_t>(opt.one_shot_limit, 1));
    unsigned n = opt.threads ? opt.threads : std::max(1u, std::thread::hardware_concurrency());
    n = (unsigned)std::min<uint64_t>({n, mem_threads, std::max<size_t>(files.size(), 1)});
    p.threads = n;

    {
        Closer closer(opt.closers ? opt.closers : n);
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        Job job{mf, root, opt, p, closer, std::max(1u, (opt.threads ? opt.threads : hw) / n)};
        std::atomic<size_t> next{0};
        auto run = [&] {
            no_power_throttling();
            Worker w(job);
            for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < files.size() && !p.cancel.load(std::memory_order_relaxed);)
                w.extract(entries[files[i]]);
        };
        std::vector<std::jthread> pool;
        for (unsigned t = 1; t < n; ++t) pool.emplace_back(run);
        run();
    }  // joins workers, then drains the closer queue

    for (uint32_t i : dirs_with_time) {  // after files, since creating files touches dir mtimes
        HANDLE h = CreateFileW((root + entries[i].path).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        set_times(h, entries[i].mtime, 0);
        CloseHandle(h);
    }
    return p.error_count.load();
}

}  // namespace xx
