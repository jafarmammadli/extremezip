<div align="center">

# ⚡ Extreme Extract

**A native Windows ZIP extractor built for one thing: speed.**

Up to **27× faster than 7-Zip** on real archives. Stored + Deflate, ZIP64, CLI and GUI.

[![build](https://github.com/jafarmammadli/extremezip/actions/workflows/release.yml/badge.svg)](https://github.com/jafarmammadli/extremezip/actions/workflows/release.yml)
[![release](https://img.shields.io/github/v/release/jafarmammadli/extremezip?color=blue)](https://github.com/jafarmammadli/extremezip/releases)
[![license](https://img.shields.io/github/license/jafarmammadli/extremezip?color=green)](LICENSE)
![platform](https://img.shields.io/badge/platform-Windows%20x64-0078D6?logo=windows)

[Download](#-download) · [Usage](#-usage) · [Benchmarks](#-benchmarks) · [How it works](#-how-it-is-fast) · [Build](#-build)

</div>

---

## 📊 Benchmarks

Median wall time in seconds (lower is better), 3 runs, i7-13650HX (20 threads), NVMe SSD.

| Archive | **xx** | 7-Zip | WinRAR | tar | Expand-Archive | vs 7-Zip |
|:--|--:|--:|--:|--:|--:|--:|
| 10 × 500 MB, Deflate mx5 | **11.2** | 66.6 | 92.8 | 41.7 | 98.8 | 🚀 **5.9×** |
| 1 GB text in 200 files, mx9 | **0.54** | 14.6 | 16.6 | 2.9 | 34.5 | 🚀 **27×** |
| 2 GB stored | **0.96** | 1.32 | 17.1 | 4.7 | 1.78 | 🚀 **1.4×** |
| 10 × 500 MB, Deflate mx1 | **6.8** | 77.6 | 104.4 | 40.0 | 119.3 | 🚀 **11.4×** |

<sub>Reproduce with the scripts in <a href="#-tests-and-benchmarks">Tests and benchmarks</a>.</sub>

## 📦 Download

Grab `ExtremeExtract-<version>-win64.zip` from [**Releases**](../../releases). No installer: unzip and run.

> [!NOTE]
> Binaries are unsigned, so Windows SmartScreen may warn on first launch. Click **More info → Run anyway**.

## 🚀 Usage

**GUI:** run `ExtremeExtract.exe`, or drag an archive onto the window.

**CLI:**

```bat
xx archive.zip [-o dest] [-t threads] [-c closers] [--one-shot-mb N] [--stop-on-error] [-q]
```

| Exit code | Meaning |
|:-:|:--|
| `0` | All entries extracted |
| `1` | Some entries failed (listed on stderr) |
| `2` | Archive unreadable |

## 🧠 How it is fast

| | Technique |
|:-:|:--|
| 🗺️ | **Memory-mapped archive.** Parsing and stored entries are zero-copy. |
| ⚖️ | **Largest-first scheduling.** Entries are sorted by size and pulled from one atomic counter by N workers (N from CPU count, memory and entry count). |
| 💨 | **One-shot inflate.** Entries up to 64 MB are inflated in a single libdeflate call into a reused per-thread buffer and written with one `WriteFile`; bigger ones stream through zlib-ng. |
| 🧮 | **Hardware CRC32.** libdeflate's PCLMUL/AVX path, with runtime CPU dispatch in both libraries. |
| 🧵 | **Async closing.** `CloseHandle` (slow on Windows because antivirus scans on close) runs on dedicated closer threads. |
| 📁 | **Up-front layout.** Directories are created once; large files get their final size set before writing. |

## 🛡️ Safety

- **Integrity:** CRC and size are checked for every entry. Corrupt or cancelled files are deleted, never left half-written.
- **Path safety:** paths with `..` are rejected; drive letters, `:` (alternate streams), reserved device names and invalid characters are neutralized. Nothing can be written outside the destination.

## 🔧 Build

Requires Visual Studio with the C++ workload, CMake and Ninja. libdeflate and zlib-ng are fetched automatically.

```bat
build.cmd
```

Outputs `build\xx.exe` (CLI) and `build\ExtremeExtract.exe` (GUI).

## 🧪 Tests and benchmarks

Both need [7-Zip](https://www.7-zip.org/) installed at `C:\Program Files\7-Zip`.

```powershell
powershell -File tests\test.ps1              # byte-for-byte comparison with 7-Zip + corrupt/malicious archives
python -I bench\corpus.py                    # builds bench\data\*.zip (~15 GB, deterministic)
powershell -File bench\bench.ps1 [-Runs 5]   # xx vs 7-Zip, WinRAR, tar, Expand-Archive -> bench\out\results.md
```

## 📄 License

MIT, see [LICENSE](LICENSE). Statically links [libdeflate](https://github.com/ebiggers/libdeflate) (MIT)
and [zlib-ng](https://github.com/zlib-ng/zlib-ng) (zlib license); their license texts ship in the release zip.
