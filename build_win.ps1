# build_win.ps1 — Build script for the Octopus Windows port (MinGW-w64)
#
# Usage (from project root, or anywhere — it finds the script dir):
#   powershell -ExecutionPolicy Bypass -File build_win.ps1           # standalone engine
#   powershell -ExecutionPolicy Bypass -File build_win.ps1 -Lib      # static lib (for Rust host)
#   powershell -ExecutionPolicy Bypass -File build_win.ps1 -Clean
#
# Requires: MSYS2 ucrt64 gcc in PATH (C:\msys64\ucrt64\bin)
#   $env:Path = "C:\msys64\ucrt64\bin;" + $env:Path

param(
    [switch]$Lib,
    [switch]$Clean,
    [switch]$Verbose
)

# Don't let stderr from gcc/cargo abort the script — we check $LASTEXITCODE instead
$ErrorActionPreference = "Continue"

# Ensure MSYS2 ucrt64 gcc is in PATH
if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) {
    $msysGcc = "C:\msys64\ucrt64\bin"
    if (Test-Path "$msysGcc\gcc.exe") {
        $env:Path = "$msysGcc;$env:Path"
        Write-Host "Using gcc from: $msysGcc" -ForegroundColor Cyan
    } else {
        Write-Host "ERROR: gcc not found. Install MSYS2 ucrt64 gcc:" -ForegroundColor Red
        Write-Host "  pacman -S mingw-w64-x86_64-gcc" -ForegroundColor Yellow
        exit 1
    }
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

if (-not (Test-Path "build")) {
    New-Item -ItemType Directory -Path "build" | Out-Null
}

# Clean
if ($Clean) {
    Write-Host "Cleaning..." -ForegroundColor Yellow
    Remove-Item -Path "src\*.o","build\octopus.exe","build\liboctopus.a" -ErrorAction SilentlyContinue
    if (-not $Lib) { exit 0 }
}

# Compiler flags — mirror the Makefile
$CC = "gcc"
$Target = "build\octopus.exe"

$CFlags = @(
    "-Wall",
    "-Wno-unused-function",
    "-Wno-unused-variable",
    "-Wno-unused-but-set-variable",
    "-std=gnu89",
    "-Wno-implicit-int",
    "-Wno-int-conversion",
    "-O2", "-g",
    "-D_WIN32_WINNT=0x0601",
    "-I", "include",
    "-I", "firmware/OCT_OS",
    "-I", "firmware/OCT_OS/_OCT_global",
    "-I", "firmware/OCT_OS/_OCT_objects",
    "-I", "firmware/OCT_OS/_OCT_Player",
    "-I", "firmware/OCT_OS/_OCT_Viewer",
    "-I", "firmware/OCT_OS/_OCT_exe_keys",
    "-I", "firmware/OCT_OS/_OCT_exe_rots",
    "-I", "firmware/OCT_OS/_OCT_init",
    "-I", "firmware/OCT_OS/_OCT_interrupts"
)

if ($Verbose) { $CFlags += "-v" }

# Engine core sources — engine.c is the single TU that includes the firmware.
# engine_main.c is only linked into the standalone binary, NOT the static lib
# (the Rust GUI/CLI hosts provide their own main).
$EngineSources = @(
    "src\engine.c",
    "src\hal_linux.c",
    "src\midi_winmm.c",
    "src\osc_server.c",
    "src\osc_render.c",
    "src\flash_file.c"
)

$StandaloneSources = $EngineSources + @("src\engine_main.c")

# Linker flags — statically link winpthread for a portable exe
$LDLibs = @("-lwinmm", "-lws2_32", "-Wl,-Bstatic,--whole-archive,-lwinpthread,--no-whole-archive,-Bdynamic", "-lm", "-lkernel32", "-lavrt")

function Compile-Sources($Sources) {
    $Objects = @()
    $ErrorCount = 0

    foreach ($src in $Sources) {
        $obj = $src -replace '\.c$', '.o'
        $Objects += $obj

        $args = @("-c") + $CFlags + @("-o", $obj, $src)
        Write-Host "  CC $src" -ForegroundColor DarkGray

        $output = & $CC @args 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  FAIL $src" -ForegroundColor Red
            $output | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
            $ErrorCount++
        } elseif ($output) {
            # Show warnings
            $output | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
        }
    }

    if ($ErrorCount -gt 0) { return $null }
    return $Objects
}

if ($Lib) {
    # ------------------------------------------------------------------
    # Static library for the Rust hosts (octopus_gui / octopus_cli)
    # ------------------------------------------------------------------
    Write-Host "Building build\liboctopus.a" -ForegroundColor Green
    $Objects = Compile-Sources $EngineSources
    if ($null -eq $Objects) {
        Write-Host "Build failed" -ForegroundColor Red
        exit 1
    }
    & ar rcs build\liboctopus.a $Objects
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ar failed" -ForegroundColor Red
        exit 1
    }
    Write-Host ""
    Write-Host "Build successful: build\liboctopus.a" -ForegroundColor Green
    exit 0
}

# ------------------------------------------------------------------
# Standalone engine binary
# ------------------------------------------------------------------
Write-Host "Building $Target" -ForegroundColor Green

$Objects = Compile-Sources ($EngineSources + @("src\engine_main.c"))
if ($null -eq $Objects) {
    Write-Host "Build failed" -ForegroundColor Red
    exit 1
}

# Link
Write-Host "  LINK $Target" -ForegroundColor DarkGray
$linkArgs = $CFlags + $Objects + @("-o", $Target) + $LDLibs
$linkOutput = & $CC @linkArgs 2>&1
if ($LASTEXITCODE -ne 0) {
    # If the target is locked (old process still running), try alternate name
    if (Test-Path $Target) {
        $altTarget = "build\octopus_new.exe"
        Write-Host "  Target locked, trying $altTarget" -ForegroundColor Yellow
        $linkArgs = $CFlags + $Objects + @("-o", $altTarget) + $LDLibs
        $linkOutput = & $CC @linkArgs 2>&1
        if ($LASTEXITCODE -eq 0) {
            $Target = $altTarget
            $linkOutput = $null
        }
    }
}
if ($LASTEXITCODE -ne 0) {
    Write-Host "Link failed" -ForegroundColor Red
    $linkOutput | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    exit 1
} elseif ($linkOutput) {
    $linkOutput | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
}

Write-Host ""
Write-Host "Build successful: $Target" -ForegroundColor Green
Write-Host "  Run: .\$Target" -ForegroundColor Cyan
