# Extreme Extract

Native Windows ZIP extractor built for speed. Stored + Deflate, ZIP64.

## Download
Get `ExtremeExtract-<version>-win64.zip` from [Releases](../../releases). No installer; unzip and run.
Binaries are unsigned, so Windows SmartScreen may warn on first launch ("More info" > "Run anyway").

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
Tests and benchmarks need [7-Zip](https://www.7-zip.org/) at `C:\Program Files\7-Zip`.
```
powershell -File tests\test.ps1                 # byte-for-byte comparison with 7-Zip + corrupt/malicious archives
python -I bench\corpus.py                       # builds bench\data\*.zip (~15 GB, deterministic)
powershell -File bench\bench.ps1 [-Runs 5]      # xx vs 7-Zip, WinRAR, tar, Expand-Archive -> bench\out\results.md
```
Median wall time in seconds, 3 runs, i7-13650HX (20 threads), NVMe SSD:

| Archive | xx | 7-Zip | WinRAR | tar | Expand-Archive |
|---|---|---|---|---|---|
| B: 10 x 500 MB, Deflate mx5 | **11.2** | 66.6 | 92.8 | 41.7 | 98.8 |
| D: 1 GB text in 200 files, mx9 | **0.54** | 14.6 | 16.6 | 2.9 | 34.5 |
| E: 2 GB stored | **0.96** | 1.32 | 17.1 | 4.7 | 1.78 |
| L: 10 x 500 MB, Deflate mx1 | **6.8** | 77.6 | 104.4 | 40.0 | 119.3 |

## License
MIT, see [LICENSE](LICENSE). Statically links [libdeflate](https://github.com/ebiggers/libdeflate) (MIT)
and [zlib-ng](https://github.com/zlib-ng/zlib-ng) (zlib license); their license texts ship in the release zip.
