# Benchmark: extracts every bench/data/*.zip with each tool, N runs, interleaved.
# Archive reads are warm (page cache); destination is deleted before every run.
# Output: bench/out/results.csv (every run) and bench/out/results.md (median/min table).
#
# powershell -File bench\bench.ps1 [-Runs 5] [-Archives A,C] [-Tools xx,7z] [-Dest D:\xxbench]
#   [-FzipCmd '"C:\fzip\fzip.exe" x "{archive}" "{dest}"']   # any extra tool, {archive}/{dest} placeholders
#   [-XxArgs '-t 8']                                         # extra xx.exe flags for experiments
param(
    [int]$Runs = 5,
    [string[]]$Archives,
    [string[]]$Tools = @('xx', '7z', 'winrar', 'tar', 'expand-archive'),
    [string]$Dest = "$env:TEMP\xx-bench",
    [string]$FzipCmd,
    [string]$XxArgs = ''
)
$ErrorActionPreference = 'Continue'
$Tools = $Tools -split ','; if ($Archives) { $Archives = $Archives -split ',' }  # -File passes "a,b" as one string
$here = $PSScriptRoot
$out = "$here\out"; New-Item -ItemType Directory -Force $out | Out-Null
$xx = (Resolve-Path "$here\..\build\xx.exe").Path

$cmds = [ordered]@{
    'xx'             = "`"$xx`" `"{archive}`" -o `"{dest}`" -q $XxArgs"
    '7z'             = "`"C:\Program Files\7-Zip\7z.exe`" x -y -bso0 -bsp0 `"-o{dest}`" `"{archive}`""
    'winrar'         = "`"C:\Program Files\WinRAR\WinRAR.exe`" x -ibck -inul -y `"{archive}`" `"{dest}\`""
    'tar'            = "`"$env:SystemRoot\System32\tar.exe`" -xf `"{archive}`" -C `"{dest}`""
    'expand-archive' = "powershell -NoProfile -Command Expand-Archive -LiteralPath '{archive}' -DestinationPath '{dest}' -Force"
}
if ($FzipCmd) { $cmds['fzip'] = $FzipCmd; if ($Tools -notcontains 'fzip') { $Tools += 'fzip' } }

$zips = Get-ChildItem "$here\data\*.zip" | Where-Object { -not $Archives -or $Archives -contains $_.BaseName } | Sort-Object Name
if (-not $zips) { throw "no archives in $here\data - run: python -I bench\corpus.py" }

function Manifest($dir) {  # digest of (path, size, content hash) for every file
    python -I -c @"
import hashlib, os, sys
root = sys.argv[1]; h = hashlib.blake2b(); n = 0; b = 0
for dp, dn, fn in os.walk(root):
    for f in fn:
        p = os.path.join(dp, f); n += 1
        with open(p, 'rb') as fh: d = hashlib.file_digest(fh, 'blake2b').hexdigest()
        s = os.path.getsize(p); b += s
        h.update(f'{os.path.relpath(p, root).lower()}|{s}|{d}\n'.encode())
print(f'{n} {b} {h.hexdigest()[:16]}')
"@ $dir
}

# Writes are cached by Windows and flushed later; without this, each tool would pay for the
# previous tool's background flush. Waits until disk writes drop below 20 MB/s for 2 s (max 3 min).
function Wait-DiskIdle {
    $quiet = 0
    for ($i = 0; $i -lt 180 -and $quiet -lt 2; $i++) {
        $w = (Get-Counter '\PhysicalDisk(_Total)\Disk Write Bytes/sec' -SampleInterval 1 -MaxSamples 1).CounterSamples[0].CookedValue
        if ($w -lt 20MB) { $quiet++ } else { $quiet = 0 }
    }
}

function Measure-Run($cmdline) {
    $exe, $argline = if ($cmdline.StartsWith('"')) { $i = $cmdline.IndexOf('"', 1); $cmdline.Substring(1, $i - 1), $cmdline.Substring($i + 1) }
                     else { $i = $cmdline.IndexOf(' '); $cmdline.Substring(0, $i), $cmdline.Substring($i + 1) }
    $psi = [Diagnostics.ProcessStartInfo]::new($exe, $argline)
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = [Diagnostics.Process]::Start($psi)
    $peak = 0L
    while (-not $p.WaitForExit(15)) { try { $p.Refresh(); $peak = [Math]::Max($peak, $p.PeakWorkingSet64) } catch {} }
    $sw.Stop()
    [pscustomobject]@{ Seconds = $sw.Elapsed.TotalSeconds; CpuSeconds = $p.TotalProcessorTime.TotalSeconds; PeakMB = [Math]::Round($peak / 1MB); Exit = $p.ExitCode }
}

$cores = [Environment]::ProcessorCount
$rows = [Collections.Generic.List[object]]::new()
$reference = @{}
foreach ($z in $zips) {
    Write-Host "== $($z.Name) ($([Math]::Round($z.Length / 1MB)) MiB)"
    [IO.File]::OpenRead($z.FullName).CopyTo([IO.Stream]::Null)  # warm the page cache once
    for ($r = 1; $r -le $Runs; $r++) {
        foreach ($t in $Tools) {
            $d = "$Dest\$t"
            if (Test-Path $d) { cmd /c rd /s /q "`"$d`"" }
            New-Item -ItemType Directory -Force $d | Out-Null
            Wait-DiskIdle
            $m =Measure-Run ($cmds[$t].Replace('{archive}', $z.FullName).Replace('{dest}', $d))
            $verify = ''
            if ($r -eq 1) {  # integrity check once per tool/archive, against 7-Zip's output
                $man = Manifest $d
                if ($t -eq '7z') { $reference[$z.Name] = $man }
                $verify = $man
            }
            $bytes = [long](($verify -split ' ')[1])
            $rows.Add([pscustomobject]@{
                Archive = $z.BaseName; Tool = $t; Run = $r; Seconds = [Math]::Round($m.Seconds, 3)
                CpuPct = [Math]::Round(100 * $m.CpuSeconds / $m.Seconds / $cores); PeakMB = $m.PeakMB; Exit = $m.Exit
                Manifest = $verify; ArchiveMB = [Math]::Round($z.Length / 1MB)
            })
            Write-Host ("  run {0} {1,-15} {2,8:N3} s  cpu {3,3}%  peak {4,5} MB  exit {5}" -f $r, $t, $m.Seconds, $rows[-1].CpuPct, $m.PeakMB, $m.Exit)
        }
    }
    cmd /c rd /s /q "`"$Dest`"" 2>$null
}
$rows | Export-Csv "$out\results.csv" -NoTypeInformation

# Summary: median and best time per archive/tool; throughput = uncompressed bytes / median time.
$md = @("| Archive | Tool | Median s | Best s | MiB/s (median) | Archive read MiB/s | CPU % | Peak MB | Verified |", "|---|---|---|---|---|---|---|---|---|")
foreach ($g in $rows | Group-Object Archive, Tool) {
    $s = $g.Group.Seconds | Sort-Object
    $med = $s[[int][Math]::Floor(($s.Count - 1) / 2)]
    $first = $g.Group | Where-Object Run -eq 1
    $ref = $reference[$first.Archive]
    $ok = if (-not $ref) { '?' } elseif ($first.Manifest -eq $ref) { 'yes' } else { "NO ($($first.Manifest))" }
    $mib = [long](($ref -split ' ')[1]) / 1MB
    $md += "| $($first.Archive) | $($first.Tool) | $med | $($s[0]) | $([Math]::Round($mib / $med)) | $([Math]::Round($first.ArchiveMB / $med)) | $(($g.Group.CpuPct | Measure-Object -Average).Average.ToString('0')) | $(($g.Group.PeakMB | Measure-Object -Maximum).Maximum) | $ok |"
}
$cpu = (Get-CimInstance Win32_Processor).Name
$md = @("Machine: $cpu, $cores logical CPUs. Runs: $Runs. Dest: $Dest", "") + $md
$md | Set-Content "$out\results.md" -Encoding utf8
$md -join "`n"
