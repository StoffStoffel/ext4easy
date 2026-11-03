# Runs ext4stress on fresh images of every variant and checks each result
# with e2fsck and a content comparison on Linux (WSL).
#
#   powershell -ExecutionPolicy Bypass -File tests\run_stress.ps1 [-Seeds 1,2,3] [-Ops 3000]
#              [-Variants default,block1k,...] [-Fill]
param(
    [string]$Seeds = '1,2,3',
    [int]$Ops = 3000,
    [string]$Variants = 'default,block1k,nocsum,nojournal,classic',
    [switch]$Fill
)
# comma lists as strings: "powershell -File" does not pass arrays reliably
$seedList = $Seeds -split ',' | ForEach-Object { [int]$_.Trim() }
$variantList = $Variants -split ',' | ForEach-Object { $_.Trim() }
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$exe = Join-Path $root 'out\Release\ext4stress.exe'
$work = Join-Path $root 'build\stress'
New-Item -ItemType Directory -Force $work | Out-Null
$failures = @()

foreach ($v in $variantList) {
    foreach ($s in $seedList) {
        $img = "build/stress/$v-$s.img"
        $manifest = "build/stress/$v-$s.manifest"
        wsl -e bash tests/make_stress_image.sh $v $img 2>$null
        $extra = @()
        if ($Fill) { $extra += '--fill' }
        $out = & $exe ($img -replace '/', '\') --seed $s --ops $Ops --quiet --manifest ($manifest -replace '/', '\') @extra 2>&1
        $line = ($out | Select-Object -Last 1)
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[$v seed $s] STRESS FAIL: $line" -ForegroundColor Red
            $failures += "$v/$s stress"
            continue
        }
        $check = wsl -e bash tests/verify_image.sh $img $manifest 2>&1 | Where-Object { $_ -notmatch 'translate' }
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[$v seed $s] LINUX CHECK FAIL" -ForegroundColor Red
            $check | Select-Object -First 30 | ForEach-Object { Write-Host "    $_" }
            $failures += "$v/$s linux"
            continue
        }
        $summary = ($check | Select-Object -Last 1)
        Write-Host "[$v seed $s] ok - $($line -replace '^OK ', '') | $summary" -ForegroundColor Green
        Remove-Item $img, $manifest -ErrorAction SilentlyContinue
    }
}

# ext3 (no extents) must be refused for writing
wsl -e bash tests/make_stress_image.sh ext3 build/stress/ext3.img 2>$null
$out = & $exe build\stress\ext3.img --ops 1 --quiet 2>&1
if ($LASTEXITCODE -ne 0 -and ($out -join ' ') -match 'can only be read') {
    Write-Host "[ext3] ok - writable mount refused" -ForegroundColor Green
} else {
    Write-Host "[ext3] FAIL - writable mount was not refused: $out" -ForegroundColor Red
    $failures += 'ext3 refuse'
}
Remove-Item build\stress\ext3.img -ErrorAction SilentlyContinue

if ($failures.Count) {
    Write-Host "FAILED: $($failures -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host "All stress runs passed." -ForegroundColor Green
