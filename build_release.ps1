# build_release.ps1 — Build release binaries for Windows
#
# Usage: powershell -ExecutionPolicy Bypass -File build_release.ps1

$ErrorActionPreference = "Continue"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

if (-not (Test-Path "dist")) { New-Item -ItemType Directory -Path "dist" | Out-Null }

Write-Host "=== Building Octopus (C) ===" -ForegroundColor Green
& .\build_win.ps1
Copy-Item "build\octopus.exe" "dist\" -Force

Write-Host "=== Building octopus_ui (Rust) ===" -ForegroundColor Green
Push-Location ui
cargo build --release
Pop-Location
Copy-Item "ui\target\release\octopus_ui.exe" "dist\" -Force

Write-Host ""
Write-Host "=== Build complete ===" -ForegroundColor Green
Write-Host "dist\ contents:"
Get-ChildItem "dist\"
