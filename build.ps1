<#
.SYNOPSIS
    XenogearsRecomp — build script for Windows (PowerShell).
.DESCRIPTION
    Builds the recompiler, regenerates the game C source from the game EXE,
    and builds the game runtime.
.PARAMETER BuildDir
    Build directory path (default: build).
.PARAMETER BuildType
    CMake build type: Release (default), RelWithDebInfo, or Debug.
.PARAMETER Generator
    CMake generator. Auto-detected if omitted (Ninja or Visual Studio).
.EXAMPLE
    .\build.ps1
    .\build.ps1 -BuildDir build-dbg -BuildType Debug
    .\build.ps1 -Generator "Visual Studio 17 2022"
.NOTES
    Prerequisites:
      - CMake 3.20+
      - Visual Studio 2022 (with C++ tools) or MinGW/MSYS2
      - SDL2 development library (vcpkg, MSYS2, or manually)
      - Place your legally owned Xenogears (Disc 1) EXE at .\game\slus_006.64
#>
param(
    [string]$BuildDir = "build",
    [string]$BuildType = "Release",
    [string]$Generator = ""
)

$ROOT = Split-Path -Parent $MyInvocation.MyCommand.Path
$RECOMPILER_DIR = Join-Path $ROOT "psxrecomp/recompiler"
$RECOMPILER_BUILD = Join-Path $RECOMPILER_DIR "build"

# --- Auto-detect generator if not specified ---
if (-not $Generator) {
    # Prefer Ninja if available (fastest)
    $ninja = Get-Command ninja -ErrorAction SilentlyContinue
    if ($ninja) {
        $Generator = "Ninja"
    }
    else {
        # Fall back to Visual Studio
        $vsTest = & cmake --help 2>&1 | Select-String "Visual Studio 17 2022"
        if ($vsTest) {
            $Generator = "Visual Studio 17 2022"
        }
        else {
            $Generator = "Ninja"
        }
    }
}
Write-Host "==> Using CMake generator: $Generator"

# --- Step 1: Build the recompiler (psxrecomp-game) ---
Write-Host "==> Building recompiler (psxrecomp-game)..."
& cmake -S $RECOMPILER_DIR -B $RECOMPILER_BUILD -G $Generator -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "Recompiler configuration failed" }
& cmake --build $RECOMPILER_BUILD --config Release
if ($LASTEXITCODE -ne 0) { throw "Recompiler build failed" }

# --- Step 2: Regenerate game C source from the EXE ---
$GAME_EXE = Join-Path $ROOT "game/slus_006.64"
$RECOMPILER_BIN = Join-Path $RECOMPILER_BUILD "Release/psxrecomp-game.exe"
if (-not (Test-Path $RECOMPILER_BIN)) {
    $RECOMPILER_BIN = Join-Path $RECOMPILER_BUILD "psxrecomp-game.exe"
}

if (Test-Path $GAME_EXE) {
    Write-Host "==> Regenerating game C code from game/slus_006.64..."
    & $RECOMPILER_BIN "--config" (Join-Path $ROOT "game.toml")
    if ($LASTEXITCODE -ne 0) { throw "Game code regeneration failed" }
}
else {
    Write-Host "!!> WARNING: game/slus_006.64 not found."
    Write-Host "    Place your legally owned Xenogears (Disc 1) EXE at:"
    Write-Host "      $GAME_EXE"
    Write-Host "    Then regenerate with:"
    Write-Host "      $RECOMPILER_BIN --config $ROOT\game.toml"
}

# --- Step 3: Build the game runtime ---
Write-Host "==> Building game runtime ($BuildType) in $BuildDir..."
$BUILD_DIR = Join-Path $ROOT $BuildDir
& cmake -S $ROOT -B $BUILD_DIR -G $Generator -DCMAKE_BUILD_TYPE=$BuildType
if ($LASTEXITCODE -ne 0) { throw "Runtime configuration failed" }
& cmake --build $BUILD_DIR --config $BuildType
if ($LASTEXITCODE -ne 0) { throw "Runtime build failed" }

Write-Host "==> Done. Binary: $BUILD_DIR/XenogearsRecomp.exe"
Write-Host "    Provide your legally owned SCPH1001.BIN BIOS when prompted."
