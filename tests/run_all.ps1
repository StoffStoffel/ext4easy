# Full write test suite: scenarios, stress matrix, disk-full and crash tests.
#   powershell -ExecutionPolicy Bypass -File tests\run_all.ps1 [-Seeds 1,2,3,4,5,6] [-CrashRuns 10]
param(
    [string]$Seeds = '1,2,3,4,5,6',
    [int]$Ops = 3000,
    [int]$CrashRuns = 10
)
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$failed = @()

Write-Host "=== scenarios"
powershell -ExecutionPolicy Bypass -File tests\run_scenarios.ps1
if ($LASTEXITCODE) { $failed += 'scenarios' }

Write-Host "=== stress matrix"
powershell -ExecutionPolicy Bypass -File tests\run_stress.ps1 -Seeds $Seeds -Ops $Ops
if ($LASTEXITCODE) { $failed += 'stress' }

Write-Host "=== disk full"
powershell -ExecutionPolicy Bypass -File tests\run_stress.ps1 -Seeds '11,12' -Ops 1500 -Variants 'default,block1k' -Fill
if ($LASTEXITCODE) { $failed += 'disk-full' }

Write-Host "=== crash"
powershell -ExecutionPolicy Bypass -File tests\crash_test.ps1 -Runs $CrashRuns
if ($LASTEXITCODE) { $failed += 'crash' }

if ($failed.Count) {
    Write-Host "SUITE FAILED: $($failed -join ', ')"
    exit 1
}
Write-Host "SUITE PASSED"
