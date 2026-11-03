# Runs every ext4scenario on a fresh image and checks it with e2fsck.
#   powershell -ExecutionPolicy Bypass -File tests\run_scenarios.ps1 [-Variant default]
param([string]$Variant = 'default')
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$exe = Join-Path $root 'out\Release\ext4scenario.exe'
New-Item -ItemType Directory -Force build\stress | Out-Null
$failed = 0
foreach ($name in (& $exe x --list)) {
    $img = 'build/stress/scenario.img'
    wsl -e bash tests/make_stress_image.sh $Variant $img 2>$null
    & $exe ($img -replace '/', '\') $name
    if ($LASTEXITCODE -ne 0) { Write-Host "[$name] scenario failed" -ForegroundColor Red; $failed++; continue }
    $out = wsl -e bash -c "e2fsck -fn $img 2>&1" 2>$null | Where-Object { $_ -match '\?' }
    if ($out) {
        Write-Host "[$name] e2fsck:" -ForegroundColor Red
        $out | ForEach-Object { Write-Host "    $_" }
        $failed++
    } else {
        Write-Host "[$name] ok" -ForegroundColor Green
    }
}
exit $failed
