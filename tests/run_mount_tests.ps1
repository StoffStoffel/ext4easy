# Write tests through a drive letter (WinFsp): normal Windows file APIs on a
# writable mount, checked by remounting and by Linux (e2fsck + contents).
# Crash runs kill the mount process instead of unmounting; replaying the
# journal must be enough and no data may be missing.
#
#   powershell -ExecutionPolicy Bypass -File tests\run_mount_tests.ps1 [-Variants default,...] [-CrashRuns 2]
param(
    [string]$Variants = 'default,block1k,nojournal',
    [int]$CrashRuns = 2
)
$variantList = $Variants -split ',' | ForEach-Object { $_.Trim() }
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$exe = Join-Path $root 'out\Release\ext4easy.exe'
New-Item -ItemType Directory -Force build\mount | Out-Null
$failures = @()

# a free drive letter, searching from Z: downwards
$used = (Get-PSDrive -PSProvider FileSystem).Name
$drive = ([char[]](90..68) | Where-Object { $used -notcontains [string]$_ } | Select-Object -First 1) + ':'

function Test-LinuxContent($img, $manifest) {
    $check = wsl -e bash tests/verify_image.sh $img $manifest 2>&1 | Where-Object { $_ -notmatch 'translate' }
    if ($LASTEXITCODE -ne 0) {
        $check | Select-Object -First 30 | ForEach-Object { Write-Host "    $_" }
        return $false
    }
    return $true
}

$seed = 1
foreach ($v in $variantList) {
    $img = "build/mount/$v.img"
    $manifest = "build/mount/$v.manifest"
    wsl -e bash tests/make_stress_image.sh $v $img 2>$null
    $out = python tests/mount_write_test.py $exe ($img -replace '/', '\') $drive ($manifest -replace '/', '\') --seed $seed 2>&1
    $seed++
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[$v] FAIL on Windows" -ForegroundColor Red
        $out | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }
        $failures += $v
        continue
    }
    if (-not (Test-LinuxContent $img $manifest)) {
        Write-Host "[$v] LINUX CHECK FAIL" -ForegroundColor Red
        $failures += "$v linux"
        continue
    }
    Write-Host "[$v] ok - $(($out | Select-Object -Last 1)); $(($out | Select-Object -First 1))" -ForegroundColor Green
    Remove-Item ($img -replace '/', '\') -ErrorAction SilentlyContinue
}

for ($i = 1; $i -le $CrashRuns; $i++) {
    $img = "build/mount/crash-$i.img"
    $manifest = "build/mount/crash-$i.manifest"
    wsl -e bash tests/make_stress_image.sh default $img 2>$null
    $out = python tests/mount_write_test.py $exe ($img -replace '/', '\') $drive ($manifest -replace '/', '\') --seed (100 + $i) --crash 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[crash $i] FAIL on Windows" -ForegroundColor Red
        $out | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }
        $failures += "crash $i"
        continue
    }
    # Same rules as crash_test.ps1: replaying the journal is fine, repairs are not
    # (except the superblock's free counters and extent tree optimization hints).
    $fsck = wsl -e bash -c "e2fsck -fy '$img' 2>&1" 2>&1 | Where-Object { $_ -notmatch 'translate' }
    $text = $fsck -join "`n"
    $text = $text -replace "Free (blocks|inodes) count wrong \(\d+, counted=\d+\)\.\s*Fix\? yes", ''
    $text = $text -replace "could be (shorter|narrower)\.\s*Optimize\? yes", ''
    $fixes = @($text -split "`n" | Where-Object { $_ -match '\? yes' })
    if ($fixes.Count -gt 0) {
        Write-Host "[crash $i] FAIL: $($fixes.Count) repairs needed" -ForegroundColor Red
        $fsck | Select-Object -First 25 | ForEach-Object { Write-Host "    $_" }
        $failures += "crash $i fsck"
        continue
    }
    if (-not (Test-LinuxContent $img $manifest)) {
        Write-Host "[crash $i] FAIL: data missing after the crash" -ForegroundColor Red
        $failures += "crash $i data"
        continue
    }
    $how = if (($fsck -join "`n") -match 'recovering journal') { 'journal replayed' } else { 'no recovery needed' }
    Write-Host "[crash $i] ok - killed, $how, no repairs, all data there" -ForegroundColor Green
    Remove-Item ($img -replace '/', '\') -ErrorAction SilentlyContinue
}

if ($failures.Count) {
    Write-Host "FAILED: $($failures -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host "All drive letter tests passed." -ForegroundColor Green
