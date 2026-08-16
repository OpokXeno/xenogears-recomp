<#
.SYNOPSIS
    XenogearsRecomp — build script for Windows (PowerShell).
.DESCRIPTION
    Builds the recompiler, regenerates the bundled BIOS and game C sources,
    and builds the game runtime.
.PARAMETER BuildDir
    Build directory path (default: build).
.PARAMETER BuildType
    CMake build type: Release (default), ReleaseNoOpt, RelWithDebInfo, or Debug.
    ReleaseNoOpt keeps NDEBUG but uses -O0 and disables developer tooling.
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
      - Python 3.11+
      - For source builds, place your legally obtained PlayStation BIOS dump at .\psxrecomp\bios\SCPH1001.BIN
      - Place your legally owned Xenogears (Disc 1) EXE at .\game\slus_006.64
#>
param(
    [string]$BuildDir = "build",
    [string]$BuildType = "Release",
    [string]$Generator = ""
)

$ErrorActionPreference = "Stop"

$RuntimeBuildType = $BuildType
$RuntimeCMakeExtraArgs = @()
if ($BuildType -eq "ReleaseNoOpt") {
    $RuntimeBuildType = "Release"
    $RuntimeCMakeExtraArgs = @(
        "-DCMAKE_C_FLAGS_RELEASE=-O0 -DNDEBUG",
        "-DCMAKE_CXX_FLAGS_RELEASE=-O0 -DNDEBUG",
        "-DPSX_DEBUG_TOOLS=OFF",
        "-DPSX_DEBUG_OVERLAY=OFF",
        "-DBUILD_TESTING=OFF"
    )
}

$ROOT = Split-Path -Parent $MyInvocation.MyCommand.Path
$RECOMPILER_DIR = Join-Path $ROOT "psxrecomp/recompiler"
$RECOMPILER_BUILD = Join-Path $RECOMPILER_DIR "build"
$MANIFEST_TOOL = Join-Path $ROOT "tools/native_render_manifest.py"
$RENDER_MANIFEST = Join-Path $ROOT "native_renderer/xg_render_manifest.toml"
$GAME_EXE = Join-Path $ROOT "game/slus_006.64"
$PYTHON = Get-Command python3 -ErrorAction SilentlyContinue
if (-not $PYTHON) {
    $PYTHON = Get-Command python -ErrorAction SilentlyContinue
}
if (-not $PYTHON) {
    throw "Python 3.11 or newer is required"
}

# --- Auto-detect generator if not specified ---
if (-not $Generator) {
    $ninja = Get-Command ninja -ErrorAction SilentlyContinue
    if ($ninja) {
        $Generator = "Ninja"
    }
    else {
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

$MANIFEST_METADATA_JSON = & $PYTHON.Source $MANIFEST_TOOL "metadata-declared" $RENDER_MANIFEST
if ($LASTEXITCODE -ne 0) { throw "Native renderer manifest metadata validation failed" }
$MANIFEST_METADATA = ($MANIFEST_METADATA_JSON -join [Environment]::NewLine) | ConvertFrom-Json
$GAME_IDENTITY_SHA256 = $MANIFEST_METADATA.game_identity
$MANIFEST_IDENTITY_SHA256 = $MANIFEST_METADATA.manifest_identity

# --- Step 1: Build the recompiler ---
Write-Host "==> Building recompiler..."
& cmake -S $RECOMPILER_DIR -B $RECOMPILER_BUILD -G $Generator `
    -DCMAKE_BUILD_TYPE=Release `
    -DPSX_GAME_EXTRA_IDENTITY_SHA256=$GAME_IDENTITY_SHA256 `
    -DPSX_GAME_MANIFEST_DIGEST_SHA256=$MANIFEST_IDENTITY_SHA256
if ($LASTEXITCODE -ne 0) { throw "Recompiler configuration failed" }
& cmake --build $RECOMPILER_BUILD --config Release
if ($LASTEXITCODE -ne 0) { throw "Recompiler build failed" }

$BIOS_RECOMPILER_BIN = Join-Path $RECOMPILER_BUILD "Release/psxrecomp-bios.exe"
if (-not (Test-Path $BIOS_RECOMPILER_BIN)) {
    $BIOS_RECOMPILER_BIN = Join-Path $RECOMPILER_BUILD "psxrecomp-bios.exe"
}
if (-not (Test-Path $BIOS_RECOMPILER_BIN)) {
    throw "psxrecomp-bios.exe not found after recompiler build"
}

Push-Location (Join-Path $ROOT "psxrecomp")
try {
    # --- Step 2: Regenerate bundled BIOS C sources ---
    foreach ($BiosStem in @("OpenBIOS", "SCPH1001")) {
        $BiosConfig = Join-Path $ROOT "psxrecomp/bios/$BiosStem.toml"

        Write-Host "==> Regenerating $BiosStem C source..."
        & $BIOS_RECOMPILER_BIN "--config" $BiosConfig
        if ($LASTEXITCODE -ne 0) { throw "$BiosStem code regeneration failed" }
    }

    # --- Step 3: Regenerate game C source from the EXE ---
    $RECOMPILER_BIN = Join-Path $RECOMPILER_BUILD "Release/psxrecomp-game.exe"
    if (-not (Test-Path $RECOMPILER_BIN)) {
        $RECOMPILER_BIN = Join-Path $RECOMPILER_BUILD "psxrecomp-game.exe"
    }
    if (-not (Test-Path $RECOMPILER_BIN)) {
        throw "psxrecomp-game.exe not found after recompiler build"
    }

    if (Test-Path $GAME_EXE) {
        Write-Host "==> Regenerating game C code from game/slus_006.64..."
        & $RECOMPILER_BIN "--config" (Join-Path $ROOT "game.toml") `
            "--source-observation-plan" (Join-Path $ROOT "native_renderer/xg_render_resident_plan.txt")
        if ($LASTEXITCODE -ne 0) { throw "Game code regeneration failed" }
    }
    else {
        Write-Host "!!> WARNING: game/slus_006.64 not found."
        Write-Host "    Place your legally owned Xenogears (Disc 1) EXE at:"
        Write-Host "      $GAME_EXE"
        Write-Host "    Then regenerate with:"
        Write-Host "      $RECOMPILER_BIN --config $ROOT\game.toml --source-observation-plan $ROOT\native_renderer\xg_render_resident_plan.txt"
    }

    # --- Step 4: Build the game runtime ---
    Write-Host "==> Building game runtime ($BuildType) in $BuildDir..."
    $BUILD_DIR = Join-Path $ROOT $BuildDir
    $RUNTIME_CMAKE_ARGS = @(
        "-S", $ROOT,
        "-B", $BUILD_DIR,
        "-G", $Generator,
        "-DCMAKE_BUILD_TYPE=$RuntimeBuildType",
        "-DPSX_RECOMP_UI=ON",
        "-DRECOMP_UI_ROOT=$(Join-Path $ROOT 'recomp-ui')"
    )
    $RUNTIME_CMAKE_ARGS += $RuntimeCMakeExtraArgs
    & cmake @RUNTIME_CMAKE_ARGS
    if ($LASTEXITCODE -ne 0) { throw "Runtime configuration failed" }
    & cmake --build $BUILD_DIR --config $RuntimeBuildType
    if ($LASTEXITCODE -ne 0) { throw "Runtime build failed" }
}
finally {
    Pop-Location
}

if (Test-Path (Join-Path $BUILD_DIR "$RuntimeBuildType/XenogearsRecomp.exe")) {
    $RUNTIME_OUTPUT_DIR = Join-Path $BUILD_DIR $RuntimeBuildType
}
else {
    $RUNTIME_OUTPUT_DIR = $BUILD_DIR
}

Write-Host "==> Done. Binary: $(Join-Path $RUNTIME_OUTPUT_DIR 'XenogearsRecomp.exe')"
Write-Host "    Bundled OpenBIOS is staged under $(Join-Path $RUNTIME_OUTPUT_DIR 'bios') and used by default."
Write-Host "    Retail SCPH1001.BIN remains optional at runtime."
