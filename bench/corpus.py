"""Builds the benchmark archives in bench/data with 7-Zip. Deterministic (fixed seed).

A  one 4.5 GB file, Deflate mx5           (ZIP64 by size)
B  10 x 500 MB files, Deflate mx5
C  70,000 small files (1-64 KB), mx5      (ZIP64 by entry count)
D  1 GB of text in 200 files, Deflate mx9 (highly compressed)
E  2 GB mostly incompressible, stored     (no compression)
L  same data as B, Deflate mx1            (low compression)

Usage: python -I bench/corpus.py [--only A,C]
"""
import os, random, subprocess, shutil, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent / "data"
SEVENZIP = r"C:\Program Files\7-Zip\7z.exe"
rng = random.Random(1234)

WORDS = [''.join(rng.choice('etaoinshrdlcumwfgypbvkjxqz') for _ in range(rng.randint(2, 10))) for _ in range(5000)]
TEXT = ' '.join(rng.choice(WORDS) for _ in range(3_000_000)).encode()[: 16 << 20]  # 16 MB text pool
RAND = rng.randbytes(16 << 20)


def mixed(n):
    """Realistic-ish data: alternating text and random runs (~2:1 deflate ratio)."""
    out, size = [], 0
    while size < n:
        pool = TEXT if rng.random() < 0.6 else RAND
        ln = min(rng.randint(4 << 10, 1 << 20), n - size)
        off = rng.randrange(0, len(pool) - ln)
        out.append(pool[off:off + ln]); size += ln
    return b''.join(out)


def text(n):
    off = rng.randrange(0, len(TEXT) - n)  # n is always < 16 MB here
    return TEXT[off:off + n]


def write_big(path, total, gen, chunk=64 << 20):
    with open(path, 'wb') as f:
        left = total
        while left:
            n = min(chunk, left); f.write(gen(n)); left -= n


def zip_dir(name, src, args):
    out = ROOT / f"{name}.zip"
    if out.exists(): out.unlink()
    subprocess.run([SEVENZIP, 'a', '-tzip', *args, str(out), str(src / '*')], check=True, stdout=subprocess.DEVNULL)
    shutil.rmtree(src)
    print(f"{name}: {out.stat().st_size / 2**20:.0f} MiB", flush=True)


def build(name):
    src = ROOT / f"src-{name}"
    shutil.rmtree(src, ignore_errors=True); src.mkdir(parents=True)
    if name == 'A':
        write_big(src / 'huge.bin', 4608 << 20, mixed); zip_dir(name, src, ['-mm=Deflate', '-mx5'])
    elif name in ('B', 'L'):
        for i in range(10): write_big(src / f'large{i}.bin', 500 << 20, mixed)
        zip_dir(name, src, ['-mm=Deflate', '-mx5' if name == 'B' else '-mx1'])
    elif name == 'C':
        for i in range(70_000):
            d = src / f'd{i % 37}' / f's{i % 11}'
            d.mkdir(parents=True, exist_ok=True)
            n = rng.randint(1 << 10, 64 << 10)
            (d / f'file{i}.{"txt" if i % 3 else "bin"}').write_bytes(mixed(n) if i % 3 == 0 else text(n))
        zip_dir(name, src, ['-mm=Deflate', '-mx5'])
    elif name == 'D':
        for i in range(200): (src / f'doc{i}.txt').write_bytes(text(5 << 20))
        zip_dir(name, src, ['-mm=Deflate', '-mx9'])
    elif name == 'E':
        for i in range(20): write_big(src / f'media{i}.bin', 100 << 20, lambda n: rng.randbytes(n))
        zip_dir(name, src, ['-mx0'])


if __name__ == '__main__':
    only = sys.argv[sys.argv.index('--only') + 1].split(',') if '--only' in sys.argv else ['A', 'B', 'C', 'D', 'E', 'L']
    ROOT.mkdir(exist_ok=True)
    for n in only: build(n)
