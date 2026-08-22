# build_release.ps1 — Build release binaries for Windows
#
# Usage: powershell -ExecutionPolicy Bypass -File build_release.ps1
#
# Requires: MSYS2 ucrt64 gcc in PATH and the x86_64-pc-windows-gnu Rust
# target (the C engine objects are MinGW-built and link against the
# gnu toolchain):
#   rustup target add x86_64-pc-windows-gnu

$ErrorActionPreference = "Continue"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

if (-not (Test-Path "dist")) { New-Item -ItemType Directory -Path "dist" | Out-Null }

Write-Host "=== Building Octopus engine (C: standalone + static lib) ===" -ForegroundColor Green
& .\build_win.ps1 -Clean
Copy-Item "build\octopus.exe" "dist\" -Force

Write-Host "=== Building octopus_gui + octopus_cli (Rust, in-process engine) ===" -ForegroundColor Green
cargo build --release --target x86_64-pc-windows-gnu
Copy-Item "target\x86_64-pc-windows-gnu\release\octopus_gui.exe" "dist\" -Force
Copy-Item "target\x86_64-pc-windows-gnu\release\octopus_cli.exe" "dist\" -Force

Write-Host ""
Write-Host "=== Build complete ===" -ForegroundColor Green
Write-Host "dist\ contents:"
Get-ChildItem "dist\"
