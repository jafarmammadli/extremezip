# Correctness suite: xx.exe output must match 7-Zip's output byte for byte,
# and corrupt / malicious archives must fail cleanly.
# Usage: powershell -File tests\test.ps1 [-Work <dir>]
param([string]$Work = "$env:TEMP\xx-test")
$ErrorActionPreference = 'Continue'  # native stderr must not throw (PS 5.1)
$xx = "$PSScriptRoot\..\build\xx.exe"
$7z = "C:\Program Files\7-Zip\7z.exe"
$fails = 0
function Check($ok, $msg) { if ($ok) { "PASS $msg" } else { "FAIL $msg"; $script:fails++ } }

function Tree($dir) {
    Get-ChildItem -LiteralPath $dir -Recurse -Force | Sort-Object FullName | ForEach-Object {
        $rel = $_.FullName.Substring($dir.Length)
        if ($_.PSIsContainer) { "D $rel" }
        else { "F $rel $($_.Length) $((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash) $($_.LastWriteTimeUtc.Ticks)" }
    }
}

Remove-Item $Work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory "$Work\src\a\b\c", "$Work\src\empty dir", "$Work\src\ünïcødé" | Out-Null
$rng = [Random]::new(42)
for ($i = 0; $i -lt 300; $i++) {
    $buf = [byte[]]::new($rng.Next(0, 20000)); $rng.NextBytes($buf)
    [IO.File]::WriteAllBytes("$Work\src\a\b\f$i.bin", $buf)
    [IO.File]::WriteAllText("$Work\src\a\t$i.txt", ("line $i " * $rng.Next(1, 3000)))
}
[IO.File]::WriteAllBytes("$Work\src\empty.bin", [byte[]]::new(0))
[IO.File]::WriteAllText("$Work\src\ünïcødé\名前.txt", "unicode")
$big = [byte[]]::new(96MB); $rng.NextBytes($big); for ($i = 0; $i -lt $big.Length; $i += 3) { $big[$i] = 0 }
[IO.File]::WriteAllBytes("$Work\src\a\b\c\big.bin", $big)   # > 64 MB: streaming path
(Get-Item "$Work\src\a\t1.txt").IsReadOnly = $true

& $7z a -tzip -mm=Deflate -mx5 "$Work\deflate.zip" "$Work\src\*" | Out-Null
& $7z a -tzip -mx0 "$Work\stored.zip" "$Work\src\*" | Out-Null

foreach ($z in 'deflate', 'stored') {
    & $7z x -y "-o$Work\ref-$z" "$Work\$z.zip" | Out-Null
    $ref = Tree "$Work\ref-$z"
    foreach ($cfg in @(@(), @('-t', '1'), @('--one-shot-mb', '0'))) {
        $out = "$Work\out-$z-$($cfg -join '')"
        & $xx "$Work\$z.zip" -o $out -q @cfg
        Check ($LASTEXITCODE -eq 0) "$z $cfg exit code"
        $diff = Compare-Object $ref (Tree $out)
        Check (-not $diff) "$z $cfg tree matches 7-Zip"
        if ($diff) { $diff | Select-Object -First 5 | Out-String | Write-Host }
    }
}
Check ((Get-Item "$Work\out-deflate-\a\t1.txt").IsReadOnly) "read-only attribute kept"

# Parallel decompression of one big Deflate stream: many tiny chunks, several encoders/levels.
foreach ($lvl in 'mx1', 'mx9') {
    & $7z a -tzip -mm=Deflate "-$lvl" "$Work\single-$lvl.zip" "$Work\src\a\b\c\big.bin" | Out-Null
    & $xx "$Work\single-$lvl.zip" -o "$Work\out-single-$lvl" -q -t 8 --one-shot-mb 0 --pinflate-min-mb 0 --pinflate-chunk-kb 64
    Check ($LASTEXITCODE -eq 0 -and (Get-FileHash "$Work\out-single-$lvl\big.bin").Hash -eq (Get-FileHash "$Work\src\a\b\c\big.bin").Hash) "parallel inflate $lvl"
}
# Corrupt data byte inside the big entry: CRC or deflate error, partial file removed.
$bytes = [IO.File]::ReadAllBytes("$Work\deflate.zip")
$bad = $bytes.Clone(); $bad[[int]($bad.Length / 2)] = $bad[[int]($bad.Length / 2)] -bxor 0xFF
[IO.File]::WriteAllBytes("$Work\flip.zip", $bad)
$err = & $xx "$Work\flip.zip" -o "$Work\out-flip" -q 2>&1 | Out-String
Check ($LASTEXITCODE -eq 1 -and $err -match 'CRC|corrupt|declared') "flipped byte detected"
Check ((Get-ChildItem "$Work\out-flip" -Recurse -File).Count -lt (Get-ChildItem "$Work\ref-deflate" -Recurse -File).Count) "corrupt entry not left on disk"

# Truncated archive: no end of central directory.
[IO.File]::WriteAllBytes("$Work\trunc.zip", $bytes[0..([int]($bytes.Length * 0.7))])
& $xx "$Work\trunc.zip" -o "$Work\out-trunc" -q 2>$null
Check ($LASTEXITCODE -eq 2) "truncated archive rejected"

# Garbage file.
[IO.File]::WriteAllBytes("$Work\garbage.zip", $big[0..99999])
& $xx "$Work\garbage.zip" -o "$Work\out-garbage" -q 2>$null
Check ($LASTEXITCODE -eq 2) "non-zip rejected"

# Zip-slip and forced ZIP64 records (built with Python's zipfile).
python -I -c @"
import zipfile, sys
w = sys.argv[1]
with zipfile.ZipFile(w + r'\slip.zip', 'w') as z:
    z.writestr('../evil.txt', 'x'); z.writestr('..\\evil2.txt', 'x'); z.writestr('ok.txt', 'fine')
    z.writestr('C:/abs.txt', 'drive'); z.writestr('CON', 'dev')
with zipfile.ZipFile(w + r'\z64.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for i in range(5):
        with z.open(f'd/{i}.txt', 'w', force_zip64=True) as f: f.write(b'zip64 ' * 1000 * (i + 1))
"@ $Work
& $xx "$Work\slip.zip" -o "$Work\out-slip\inner" -q 2>$null
Check ($LASTEXITCODE -eq 1) "zip-slip entries reported"
Check (-not (Test-Path "$Work\out-slip\evil.txt") -and -not (Test-Path "$Work\out-slip\evil2.txt")) "nothing written outside destination"
Check ((Test-Path "$Work\out-slip\inner\ok.txt") -and (Test-Path "$Work\out-slip\inner\C_\abs.txt") -and (Test-Path -LiteralPath "$Work\out-slip\inner\_CON")) "safe entries still extracted"
& $xx "$Work\z64.zip" -o "$Work\out-z64" -q
Check ($LASTEXITCODE -eq 0 -and (Get-Item "$Work\out-z64\d\4.txt").Length -eq 30000) "ZIP64 extra fields"

if ($fails) { "$fails FAILED"; exit 1 } else { "ALL PASSED" }
