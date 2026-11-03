# Builds ext4easy and the installer:
#   out\installer\ext4easy-<version>.msi        (needs WinFsp already installed)
#   out\installer\ext4easy-setup-<version>.exe  (installs WinFsp + ext4easy)
#
# Requirements: CMake + VS Build Tools, WiX 5:
#   dotnet tool install --global wix --version 5.0.2
#   wix extension add -g WixToolset.BootstrapperApplications.wixext/5.0.2
# and the WinFsp MSI in third_party\winfsp\ (see README).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$env:PATH += ";$env:USERPROFILE\.dotnet\tools"

# Version from CMakeLists.txt
$version = (Select-String -Path CMakeLists.txt -Pattern 'project\(ext4easy VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$winfspMsi = Get-ChildItem third_party\winfsp\winfsp-*.msi | Sort-Object Name | Select-Object -Last 1
if (-not $winfspMsi) { throw "WinFsp MSI missing in third_party\winfsp\" }

Write-Host "== building ext4easy $version"
cmake -S . -B out -A x64 | Out-Null
cmake --build out --config Release
if ($LASTEXITCODE) { throw "build failed" }

$dest = Join-Path $root 'out\installer'
New-Item -ItemType Directory -Force $dest | Out-Null
$msi = Join-Path $dest "ext4easy-$version.msi"
$setup = Join-Path $dest "ext4easy-setup-$version.exe"

Write-Host "== building $msi"
Push-Location installer
try {
    wix build ext4easy.wxs -arch x64 -d "Version=$version" -d "BinDir=$root\out\Release" -o $msi
    if ($LASTEXITCODE) { throw "MSI build failed" }

    Write-Host "== building $setup"
    wix build bundle.wxs -arch x64 -ext WixToolset.BootstrapperApplications.wixext `
        -d "Version=$version" -d "WinFspMsi=$($winfspMsi.FullName)" -d "Ext4easyMsi=$msi" -o $setup
    if ($LASTEXITCODE) { throw "bundle build failed" }
} finally {
    Pop-Location
}
Remove-Item (Join-Path $dest '*.wixpdb') -ErrorAction SilentlyContinue
Get-ChildItem $dest | Select-Object Name, Length
