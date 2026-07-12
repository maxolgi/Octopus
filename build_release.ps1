# build_release.ps1 — Build all release binaries for Windows
#
# Usage: powershell -ExecutionPolicy Bypass -File build_release.ps1

$ErrorActionPreference = "Continue"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

if (-not (Test-Path "dist")) { New-Item -ItemType Directory -Path "dist" | Out-Null }

Write-Host "=== Building Octopus (C) ===" -ForegroundColor Green
& .\build_win.ps1
Copy-Item "build\octopus.exe" "dist\" -Force

Write-Host "=== Building Nemo (C) ===" -ForegroundColor Green
& .\build_win.ps1 -Nemo
Copy-Item "build\nemo.exe" "dist\" -Force

Write-Host "=== Building octopus_ui (Rust) ===" -ForegroundColor Green
Push-Location ui
cargo build --release
Pop-Location
Copy-Item "ui\target\release\octopus_ui.exe" "dist\" -Force

Write-Host "=== Building web_gui (Rust) ===" -ForegroundColor Green
Push-Location cli_ui
cargo build --release
Pop-Location
Copy-Item "cli_ui\target\release\web_gui.exe" "dist\" -Force

Write-Host ""
Write-Host "=== Build complete ===" -ForegroundColor Green
Write-Host "dist\ contents:"
Get-ChildItem "dist\"
