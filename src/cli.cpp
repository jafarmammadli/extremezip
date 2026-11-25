// xx.exe — headless extractor used by the test and benchmark scripts.
#include "engine.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cwchar>
#include <string>

static std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static int usage() {
    std::fputs("usage: xx <archive.zip> [-o dest] [-t threads] [-c closers] [--one-shot-mb N] [--pinflate-min-mb N] [--pinflate-chunk-kb N] [--stop-on-error] [--test] [-q]\n", stderr);
    return 2;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::wstring archive, dest;
    xx::Options opt;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        bool more = i + 1 < argc;
        if (a == L"-o" && more) dest = argv[++i];
        else if (a == L"-t" && more) opt.threads = (unsigned)_wtoi(argv[++i]);
        else if (a == L"-c" && more) opt.closers = (unsigned)_wtoi(argv[++i]);
        else if (a == L"--one-shot-mb" && more) opt.one_shot_limit = (uint64_t)_wtoi64(argv[++i]) << 20;
        else if (a == L"--pinflate-min-mb" && more) opt.pinflate_min = (uint64_t)_wtoi64(argv[++i]) << 20;
        else if (a == L"--pinflate-chunk-kb" && more) opt.pinflate_chunk = (size_t)_wtoi64(argv[++i]) << 10;
        else if (a == L"--stop-on-error") opt.stop_on_error = true;
        else if (a == L"--test") opt.test_only = true;
        else if (a == L"-q") quiet = true;
        else if (a[0] != L'-' && archive.empty()) archive = a;
        else return usage();
    }
    if (archive.empty()) return usage();
    if (dest.empty()) {  // default: folder named after the archive, next to it
        dest = archive;
        if (auto dot = dest.find_last_of(L'.'); dot != std::wstring::npos && dot > dest.find_last_of(L"\\/") + 1) dest.resize(dot);
    }

    xx::Progress p;
    auto t0 = std::chrono::steady_clock::now();
    size_t errors;
    try {
        errors = xx::extract(archive, dest, opt, p);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 2;
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (auto& e : p.errors()) std::fprintf(stderr, "error: %s\n", utf8(e).c_str());
    if (!quiet) {
        double mb = p.bytes_done / 1048576.0;
        std::printf("%llu files, %.1f MiB in %.3f s = %.1f MiB/s, %u threads, %zu errors\n",
                    (unsigned long long)p.files_done.load(), mb, s, mb / s, p.threads.load(), errors);
    }
    return errors ? 1 : 0;
}
