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
.PARAMETER DiscImage
    Disc 1 CUE, BIN, or ISO path. Auto-detected under .\game when omitted.
.PARAMETER BuildJobs
    Maximum parallel build jobs. Defaults to BUILD_JOBS, CMAKE_BUILD_PARALLEL_LEVEL,
    or at most 16 logical processors.
.EXAMPLE
    .\build.ps1
    .\build.ps1 -BuildDir build-dbg -BuildType Debug
    .\build.ps1 -Generator "Visual Studio 17 2022"
.NOTES
    Prerequisites:
      - CMake 3.20+
      - Visual Studio 2022 (with C++ tools) or MinGW/MSYS2
      - SDL3 3.4+ development library (vcpkg, MSYS2, or manually)
      - Python 3.11+
      - For source builds, place your legally obtained PlayStation BIOS dump at .\psxrecomp\bios\SCPH1001.BIN
      - Place your legally owned Xenogears Disc 1 at .\game\disc1.cue, disc1.bin, or disc1.iso
      - Place its EXE at .\game\slus_006.64; extracted overlay binaries are not required
#>
param(
    [string]$BuildDir = "build",
    [string]$BuildType = "Release",
    [string]$Generator = "",
    [string]$DiscImage = "",
    [int]$BuildJobs = 0
)

# NOT "Stop": every step below is a native exe (cmake/ninja/psxrecomp-*)
# checked via an explicit $LASTEXITCODE test below it. Under "Stop",
# PowerShell 5.1 promotes the *first line* a native tool writes to stderr
# (even routine CMake/ninja status text, with exit code 0) into a
# terminating NativeCommandError, aborting the script on false positives.
$ErrorActionPreference = "Continue"

if ($BuildJobs -le 0) {
    $ConfiguredBuildJobs = if ($env:BUILD_JOBS) {
        $env:BUILD_JOBS
    }
    elseif ($env:CMAKE_BUILD_PARALLEL_LEVEL) {
        $env:CMAKE_BUILD_PARALLEL_LEVEL
    }
    else {
        [Math]::Min(16, [Environment]::ProcessorCount)
    }
    if (-not [int]::TryParse($ConfiguredBuildJobs, [ref]$BuildJobs) -or $BuildJobs -le 0) {
        throw "BUILD_JOBS must be a positive integer"
    }
}

function Initialize-MSVCEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        return
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} `
        "Microsoft Visual Studio/Installer/vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "MSVC is not available in PATH and vswhere.exe was not found"
    }

    $vsInstall = (& $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1)
    if (-not $vsInstall) {
        throw "No Visual Studio installation with the MSVC x64 build tools was found"
    }

    $vsDevCmd = Join-Path $vsInstall "Common7/Tools/VsDevCmd.bat"
    if (-not (Test-Path -LiteralPath $vsDevCmd)) {
        throw "Visual Studio developer environment script not found: $vsDevCmd"
    }

    Write-Host "==> Initializing MSVC environment from: $vsInstall"
    $devCmd = "call `"$vsDevCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    $devEnv = & $env:ComSpec /d /s /c $devCmd
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to initialize the Visual Studio developer environment"
    }
    foreach ($line in $devEnv) {
        if ($line -match '^([^=][^=]*)=(.*)$') {
            [Environment]::SetEnvironmentVariable(
                $Matches[1], $Matches[2], "Process")
        }
    }

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "Visual Studio developer environment did not provide cl.exe"
    }
}

$RuntimeBuildType = $BuildType
$RuntimeCMakeExtraArgs = @(
    "-DBUILD_TESTING=OFF",
    "-DPSX_SDL_BACKEND=SDL3",
    "-DXG_RENDER_VALIDATE_OVERLAYS=OFF"
)
if ($BuildType -eq "ReleaseNoOpt") {
    $RuntimeBuildType = "Release"
    $RuntimeCMakeExtraArgs += @(
        "-DCMAKE_C_FLAGS_RELEASE=-O0 -DNDEBUG",
        "-DCMAKE_CXX_FLAGS_RELEASE=-O0 -DNDEBUG",
        "-DPSX_DEBUG_TOOLS=OFF",
        "-DPSX_DEBUG_OVERLAY=OFF"
    )
}

$ROOT = Split-Path -Parent $MyInvocation.MyCommand.Path
$RECOMPILER_DIR = Join-Path $ROOT "psxrecomp/recompiler"
$RECOMPILER_BUILD = Join-Path $RECOMPILER_DIR "build"
$MANIFEST_TOOL = Join-Path $ROOT "tools/native_render_manifest.py"
$RENDER_MANIFEST = Join-Path $ROOT "native_renderer/xg_render_manifest.toml"
$GAME_EXE = Join-Path $ROOT "game/slus_006.64"
if (-not $DiscImage) {
    foreach ($CandidateName in @("disc1.cue", "disc1.bin", "disc1.iso")) {
        $Candidate = Join-Path $ROOT "game/$CandidateName"
        if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
            $DiscImage = $Candidate
            break
        }
    }
}
elseif (-not [System.IO.Path]::IsPathRooted($DiscImage)) {
    $DiscImage = Join-Path $ROOT $DiscImage
}
if (-not $DiscImage -or -not (Test-Path -LiteralPath $DiscImage -PathType Leaf)) {
    throw "Xenogears Disc 1 image not found. Place disc1.cue, disc1.bin, or disc1.iso under $ROOT\game, or pass -DiscImage."
}
$DiscImage = (Resolve-Path -LiteralPath $DiscImage).Path
$PYTHON = Get-Command python3 -ErrorAction SilentlyContinue
if (-not $PYTHON) {
    $PYTHON = Get-Command python -ErrorAction SilentlyContinue
}
if (-not $PYTHON) {
    throw "Python 3.11 or newer is required"
}

# --- Auto-detect generator and initialize an explicit Ninja toolchain ---
if (-not $Generator) {
    $ninja = Get-Command ninja -ErrorAction SilentlyContinue
    $compilerReady = (
        $env:CC -or $env:CXX -or
        (Get-Command cl.exe -ErrorAction SilentlyContinue) -or
        (Get-Command gcc.exe -ErrorAction SilentlyContinue) -or
        (Get-Command clang.exe -ErrorAction SilentlyContinue)
    )
    if ($ninja -and $compilerReady) {
        $Generator = "Ninja"
    }
    else {
        $vsTest = & cmake --help 2>&1 | Select-String "Visual Studio 17 2022"
        if ($vsTest) {
            $Generator = "Visual Studio 17 2022"
        }
        elseif ($ninja) {
            Initialize-MSVCEnvironment
            $Generator = "Ninja"
        }
        else {
            throw "No supported CMake generator was found (Visual Studio 2022 or Ninja)"
        }
    }
}
elseif ($Generator -like "Ninja*" -and -not $env:CC -and -not $env:CXX -and
        -not (Get-Command cl.exe -ErrorAction SilentlyContinue) -and
        -not (Get-Command gcc.exe -ErrorAction SilentlyContinue) -and
        -not (Get-Command clang.exe -ErrorAction SilentlyContinue)) {
    Initialize-MSVCEnvironment
}
Write-Host "==> Using CMake generator: $Generator"
$CMakeGeneratorArgs = @("-G", $Generator)
if ($Generator -like "Visual Studio*") {
    $CMakeGeneratorArgs += @("-A", "x64")
}

$MANIFEST_METADATA_JSON = & $PYTHON.Source $MANIFEST_TOOL "metadata-declared" $RENDER_MANIFEST
if ($LASTEXITCODE -ne 0) { throw "Native renderer manifest metadata validation failed" }
$MANIFEST_METADATA = ($MANIFEST_METADATA_JSON -join [Environment]::NewLine) | ConvertFrom-Json
$GAME_IDENTITY_SHA256 = $MANIFEST_METADATA.game_identity
$MANIFEST_IDENTITY_SHA256 = $MANIFEST_METADATA.manifest_identity

# --- Step 1: Build the recompiler ---
Write-Host "==> Building recompiler..."
& cmake -S $RECOMPILER_DIR -B $RECOMPILER_BUILD @CMakeGeneratorArgs `
    "-DCMAKE_BUILD_TYPE=Release" `
    "-DPSX_GAME_EXTRA_IDENTITY_SHA256=$GAME_IDENTITY_SHA256" `
    "-DPSX_GAME_MANIFEST_DIGEST_SHA256=$MANIFEST_IDENTITY_SHA256"
if ($LASTEXITCODE -ne 0) { throw "Recompiler configuration failed" }
& cmake --build $RECOMPILER_BUILD --config Release --parallel $BuildJobs
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
        "-DCMAKE_BUILD_TYPE=$RuntimeBuildType",
        "-DPSX_RECOMP_UI=ON",
        "-DRECOMP_UI_ROOT=$(Join-Path $ROOT 'recomp-ui')",
        "-DXG_DISC_IMAGE=$DiscImage",
        "-DXG_RECOMPILER_EXECUTABLE=$RECOMPILER_BIN"
    )
    $RUNTIME_CMAKE_ARGS += $CMakeGeneratorArgs
    $RUNTIME_CMAKE_ARGS += $RuntimeCMakeExtraArgs
    & cmake @RUNTIME_CMAKE_ARGS
    if ($LASTEXITCODE -ne 0) { throw "Runtime configuration failed" }
    & cmake --build $BUILD_DIR --config $RuntimeBuildType --target psx-runtime --parallel $BuildJobs
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
