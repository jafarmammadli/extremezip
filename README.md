# Extreme Extract

Native Windows ZIP extractor built for speed. Stored + Deflate, ZIP64.

## Build
`build.cmd` (needs Visual Studio with C++ workload, CMake, Ninja; fetches libdeflate and zlib-ng).
Outputs `build\xx.exe` (CLI) and `build\ExtremeExtract.exe` (GUI).

## Use
```
xx archive.zip [-o dest] [-t threads] [-c closers] [--one-shot-mb N] [--stop-on-error] [-q]
ExtremeExtract.exe [archive.zip]      # or drag an archive onto the window
```
Exit code: 0 ok, 1 some entries failed (listed on stderr), 2 archive unreadable.

## How it is fast
- Archive is memory-mapped; parsing and stored entries are zero-copy.
- Entries are sorted largest-first and pulled from one atomic counter by N workers (N from CPU count, memory, entry count).
- Entries up to 64 MB are inflated in one call by libdeflate into a reused per-thread buffer and written with one `WriteFile`; bigger ones stream through zlib-ng.
- CRC32 uses libdeflate's PCLMUL/AVX path (runtime CPU dispatch in both libraries).
- `CloseHandle` (slow on Windows because antivirus scans on close) runs on separate closer threads.
- Directories are created once, up front; large files get their final size set before writing.

## Safety
CRC and size checked for every entry; corrupt or cancelled files are deleted, never left half-written.
Paths with `..` are rejected; drive letters, `:` (alternate streams), reserved device names and invalid characters are neutralized; nothing can be written outside the destination.

## Tests and benchmarks
```
powershell -File tests\test.ps1                 # byte-for-byte comparison with 7-Zip + corrupt/malicious archives
python -I bench\corpus.py                       # builds bench\data\*.zip (~15 GB, deterministic)
powershell -File bench\bench.ps1 [-Runs 5]      # xx vs 7-Zip, WinRAR, tar, Expand-Archive -> bench\out\results.md
```
See `bench/RESULTS.md` for measured numbers and which optimizations were kept.
