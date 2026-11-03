# Crash consistency test: runs ext4stress on a fresh image, kills it at a
# random moment and lets e2fsck recover the filesystem. Replaying the journal
# must be enough - any other repair ("Fix? yes") counts as a failure.
#
#   powershell -ExecutionPolicy Bypass -File tests\crash_test.ps1 [-Runs 10] [-Variant default]
#
# Note: killing the process keeps all writes already handed to Windows, in
# order. Real power loss can additionally lose or reorder unflushed writes in
# the drive's cache; that is not covered here.
param(
    [int]$Runs = 10,
    [string]$Variant = 'default'
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$exe = Join-Path $root 'out\Release\ext4stress.exe'
New-Item -ItemType Directory -Force build\stress | Out-Null
$failures = 0

for ($i = 1; $i -le $Runs; $i++) {
    $img = "build/stress/crash-$i.img"
    wsl -e bash tests/make_stress_image.sh $Variant $img 2>$null
    $seed = 1000 + $i
    $p = Start-Process -FilePath $exe -ArgumentList ($img -replace '/', '\'), '--seed', $seed, '--ops', '1000000', '--verify', '0', '--remount', '0', '--quiet' -PassThru -WindowStyle Hidden
    $delay = Get-Random -Minimum 2000 -Maximum 15000
    Start-Sleep -Milliseconds $delay
    if ($p.HasExited) {
        Write-Host "[run $i] stress ended early (exit $($p.ExitCode))" -ForegroundColor Red
        $failures++
        continue
    }
    Stop-Process -Id $p.Id -Force
    $p.WaitForExit()

    $out = wsl -e bash -c "e2fsck -fy '$img' 2>&1; echo EXIT=`$?" 2>&1 | Where-Object { $_ -notmatch 'translate' }
    # Not counted as repairs (Linux handles them silently on mount / in preen mode):
    # the superblock's total free block/inode counters, which lwext4 only
    # writes at unmount, and extent tree optimization hints.
    $text = $out -join "`n"
    $text = $text -replace "Free (blocks|inodes) count wrong \(\d+, counted=\d+\)\.\s*Fix\? yes", ''
    $text = $text -replace "could be (shorter|narrower)\.\s*Optimize\? yes", ''
    $fixes = @($text -split "`n" | Where-Object { $_ -match '\? yes' })
    $recovered = ($out -join "`n") -match 'recovering journal'
    $clean = wsl -e bash -c "e2fsck -fn '$img' > /dev/null 2>&1; echo `$?" 2>$null
    if ($fixes.Count -gt 0 -or $clean -ne '0') {
        Write-Host "[run $i] FAIL after $($delay) ms: $($fixes.Count) repairs needed" -ForegroundColor Red
        $out | Select-Object -First 25 | ForEach-Object { Write-Host "    $_" }
        $failures++
        continue
    }
    $how = if ($recovered) { 'journal replayed' } else { 'no recovery needed' }
    Write-Host "[run $i] ok - killed after $delay ms, $how, no repairs" -ForegroundColor Green
    Remove-Item ($img -replace '/', '\') -ErrorAction SilentlyContinue
}

if ($failures) {
    Write-Host "$failures of $Runs crash runs FAILED" -ForegroundColor Red
    exit 1
}
Write-Host "All $Runs crash runs passed." -ForegroundColor Green
