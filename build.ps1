<#
.SYNOPSIS
    XenogearsRecomp — build script for Windows (PowerShell).
.DESCRIPTION
    Builds the recompiler, regenerates the bundled BIOS and game C sources,
    and builds the game runtime.
.PARAMETER BuildDir
    Build directory path (default: build), absolute or relative to this script.
.PARAMETER BuildType
    CMake build type: Release (default), ReleaseNoOpt, RelWithDebInfo, or Debug.
    ReleaseNoOpt keeps NDEBUG but uses /Od or -O0. Release modes disable
    developer tooling; Debug and RelWithDebInfo enable it.
.PARAMETER Generator
    CMake generator. If omitted, preserves each build tree's cached generator;
    new trees use auto-detection (Ninja or Visual Studio).
.PARAMETER DiscImage
    Disc 1 CUE, BIN, or ISO path. Defaults to XG_DISC, then local auto-detection.
.PARAMETER BuildJobs
    Maximum parallel build jobs. Defaults to BUILD_JOBS, CMAKE_BUILD_PARALLEL_LEVEL,
    or at most 16 logical processors.
.EXAMPLE
    .\build.ps1
    .\build.ps1 -BuildDir build-dbg -BuildType Debug
    .\build.ps1 -Generator "Visual Studio 17 2022"
    $env:CC = "clang-cl"; $env:CXX = "clang-cl"; .\build.ps1
.NOTES
    Environment overrides:
      PSX_RECOMPILER_BUILD: recompiler build directory (default: psxrecomp/recompiler/build).
      XG_DISC: Disc 1 image. Relative paths are resolved against this script's root.
      XG_RENDER_NATIVE: ON (default) or OFF, matching runtime identity metadata.
      PSX_RECOMPILER_BUILD also accepts absolute or root-relative paths.
    Prerequisites:
      - CMake 3.20+
      - Visual Studio 2022 (with C++ tools; add the C++ Clang Compiler for
        Windows component to use clang-cl) or MinGW/MSYS2
      - SDL3 3.4+ development library (vcpkg, MSYS2, or manually)
      - Python 3.11+
      - For source builds, place your legally obtained PlayStation BIOS dump at .\psxrecomp\bios\SCPH1001.BIN
      - Place your legally owned Xenogears Disc 1 at .\game\disc1.cue, disc1.bin, or disc1.iso
      - Place its EXE at .\game\slus_006.64; extracted overlay binaries are not required
#>
param(
    [string]$BuildDir = "build",
    [ValidateSet("Release", "ReleaseNoOpt", "RelWithDebInfo", "Debug")]
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

function Get-CMakeCacheValue([string]$Directory, [string]$Name) {
    $Cache = Join-Path $Directory "CMakeCache.txt"
    if (Test-Path -LiteralPath $Cache -PathType Leaf) {
        foreach ($Line in Get-Content -LiteralPath $Cache) {
            if ($Line -match ("^" + [regex]::Escape($Name) + ":[^=]+=(.*)$")) {
                return $Matches[1]
            }
        }
    }
    return ""
}

function Get-GeneratorArgs([string]$Directory) {
    $CachedGenerator = Get-CMakeCacheValue $Directory "CMAKE_GENERATOR"
    if ($CachedGenerator) {
        if ($ExplicitGenerator -and $ExplicitGenerator -ne $CachedGenerator) {
            throw "Generator mismatch in ${Directory}: cached '$CachedGenerator', requested '$ExplicitGenerator'. Use another build directory."
        }
        return
    }
    "-G"
    $Generator
    if ($Generator -like "Visual Studio*") {
        "-A"
        "x64"
    }
}

# Canonicalize casing because CMake configuration names are case-sensitive.
$BuildType = @("Release", "ReleaseNoOpt", "RelWithDebInfo", "Debug") |
    Where-Object { $_ -eq $BuildType } | Select-Object -First 1
$RuntimeBuildType = $BuildType
$DeveloperTools = if ($BuildType -in @("Debug", "RelWithDebInfo")) { "ON" } else { "OFF" }
$RuntimeCMakeExtraArgs = @(
    "-UCMAKE_C_FLAGS_RELEASE",
    "-UCMAKE_CXX_FLAGS_RELEASE",
    "-DBUILD_TESTING=OFF",
    "-DPSX_SDL_BACKEND=SDL3",
    "-DXG_RENDER_VALIDATE_OVERLAYS=OFF",
    "-DPSX_DEBUG_TOOLS=$DeveloperTools",
    "-DPSX_DEBUG_OVERLAY=$DeveloperTools"
)
if ($BuildType -eq "ReleaseNoOpt") {
    $RuntimeBuildType = "Release"
}

$ROOT = Split-Path -Parent $MyInvocation.MyCommand.Path
$BUILD_DIR = $BuildDir
if (-not [System.IO.Path]::IsPathRooted($BUILD_DIR)) {
    $BUILD_DIR = Join-Path $ROOT $BUILD_DIR
}
$BUILD_DIR = [System.IO.Path]::GetFullPath($BUILD_DIR)
$RECOMPILER_DIR = Join-Path $ROOT "psxrecomp/recompiler"
$RECOMPILER_BUILD = if ($env:PSX_RECOMPILER_BUILD) { $env:PSX_RECOMPILER_BUILD } else { Join-Path $RECOMPILER_DIR "build" }
if (-not [System.IO.Path]::IsPathRooted($RECOMPILER_BUILD)) {
    $RECOMPILER_BUILD = Join-Path $ROOT $RECOMPILER_BUILD
}
$RECOMPILER_BUILD = [System.IO.Path]::GetFullPath($RECOMPILER_BUILD)
$MANIFEST_TOOL = Join-Path $ROOT "tools/native_render_manifest.py"
$RENDER_MANIFEST = Join-Path $ROOT "native_renderer/xg_render_manifest.toml"
$GAME_EXE = Join-Path $ROOT "game/slus_006.64"
foreach ($RequiredFile in @($GAME_EXE,
        (Join-Path $ROOT "psxrecomp/bios/openbios.bin"),
        (Join-Path $ROOT "psxrecomp/bios/SCPH1001.BIN"))) {
    if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
        throw "Required source input not found: $RequiredFile"
    }
}
$NativeRender = if ($env:XG_RENDER_NATIVE) { $env:XG_RENDER_NATIVE.ToUpperInvariant() } else { "ON" }
if ($NativeRender -notin @("ON", "OFF")) {
    throw "XG_RENDER_NATIVE must be ON or OFF"
}
if (-not $DiscImage) { $DiscImage = $env:XG_DISC }
if (-not $DiscImage) {
    foreach ($CandidateName in @("disc1.cue", "disc1.bin", "disc1.iso")) {
        $Candidate = Join-Path $ROOT "game/$CandidateName"
        if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
            $DiscImage = $Candidate
            break
        }
    }
}
if ($DiscImage -and -not [System.IO.Path]::IsPathRooted($DiscImage)) {
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

# clang-cl needs the Visual Studio developer environment (Windows SDK, rc.exe),
# which the cl.exe/gcc/clang checks below would otherwise skip.
if (($env:CC -like "*clang-cl*") -or ($env:CXX -like "*clang-cl*")) {
    Initialize-MSVCEnvironment
}

# --- Auto-detect generator and initialize an explicit Ninja toolchain ---
$ExplicitGenerator = $Generator
if (-not $Generator) {
    $Generator = Get-CMakeCacheValue $RECOMPILER_BUILD "CMAKE_GENERATOR"
    if (-not $Generator) { $Generator = Get-CMakeCacheValue $BUILD_DIR "CMAKE_GENERATOR" }
}
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
if (($Generator -like "Ninja*" -or
        (Get-CMakeCacheValue $BUILD_DIR "CMAKE_GENERATOR") -like "Ninja*") -and
        -not $env:CC -and -not $env:CXX -and
        -not (Get-Command cl.exe -ErrorAction SilentlyContinue) -and
        -not (Get-Command gcc.exe -ErrorAction SilentlyContinue) -and
        -not (Get-Command clang.exe -ErrorAction SilentlyContinue)) {
    Initialize-MSVCEnvironment
}
Write-Host "==> CMake generator for new trees: $Generator (existing trees retain their generator)"
$RecompilerGeneratorArgs = @(Get-GeneratorArgs $RECOMPILER_BUILD)
$RuntimeGeneratorArgs = @(Get-GeneratorArgs $BUILD_DIR)

if ($NativeRender -eq "OFF") {
    $GAME_IDENTITY_SHA256 = (Get-FileHash -LiteralPath $GAME_EXE -Algorithm SHA256).Hash.ToLowerInvariant()
    $MANIFEST_IDENTITY_SHA256 = (Get-FileHash -LiteralPath $RENDER_MANIFEST -Algorithm SHA256).Hash.ToLowerInvariant()
}
else {
    $MANIFEST_METADATA_JSON = & $PYTHON.Source $MANIFEST_TOOL "metadata-declared" $RENDER_MANIFEST
    if ($LASTEXITCODE -ne 0) { throw "Native renderer manifest metadata validation failed" }
    $MANIFEST_METADATA = ($MANIFEST_METADATA_JSON -join [Environment]::NewLine) | ConvertFrom-Json
    $GAME_IDENTITY_SHA256 = $MANIFEST_METADATA.game_identity
    $MANIFEST_IDENTITY_SHA256 = $MANIFEST_METADATA.manifest_identity
}

# --- Step 1: Build the recompiler ---
Write-Host "==> Building recompiler..."
& cmake -S $RECOMPILER_DIR -B $RECOMPILER_BUILD @RecompilerGeneratorArgs `
    "-DCMAKE_BUILD_TYPE=Release" "-DBUILD_TESTING=OFF" `
    "-DPSX_GAME_EXTRA_IDENTITY_SHA256=$GAME_IDENTITY_SHA256" `
    "-DPSX_GAME_MANIFEST_DIGEST_SHA256=$MANIFEST_IDENTITY_SHA256"
if ($LASTEXITCODE -ne 0) { throw "Recompiler configuration failed" }
& cmake --build $RECOMPILER_BUILD --config Release --target psxrecomp-game psxrecomp-bios --parallel $BuildJobs
if ($LASTEXITCODE -ne 0) { throw "Recompiler build failed" }

$RECOMPILER_OUTPUT_DIR = $RECOMPILER_BUILD
if (Get-CMakeCacheValue $RECOMPILER_BUILD "CMAKE_CONFIGURATION_TYPES") {
    $RECOMPILER_OUTPUT_DIR = Join-Path $RECOMPILER_BUILD "Release"
}
$BIOS_RECOMPILER_BIN = Join-Path $RECOMPILER_OUTPUT_DIR "psxrecomp-bios.exe"
$RECOMPILER_BIN = Join-Path $RECOMPILER_OUTPUT_DIR "psxrecomp-game.exe"
foreach ($Executable in @($BIOS_RECOMPILER_BIN, $RECOMPILER_BIN)) {
    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
        throw "Recompiler executable not found after build: $Executable"
    }
}

Push-Location (Join-Path $ROOT "psxrecomp")
try {
    # --- Step 2: Regenerate bundled BIOS C sources ---
    foreach ($BiosStem in @("OpenBIOS", "SCPH1001")) {
        $BiosConfig = Join-Path $ROOT "psxrecomp/bios/$BiosStem.toml"

        Write-Host "==> Regenerating $BiosStem C source..."
        & $BIOS_RECOMPILER_BIN "--config" $BiosConfig
        if ($LASTEXITCODE -ne 0) { throw "$BiosStem code regeneration failed" }
        $Bash = Get-Command bash -ErrorAction SilentlyContinue
        if ($Bash) {
            try {
                $FingerprintTool = (Join-Path $ROOT "psxrecomp/tools/bios_emitter_fingerprint.sh").Replace('\', '/')
                $Fingerprint = (& $Bash.Source $FingerprintTool ($BiosConfig.Replace('\', '/'))) -join "`n"
                if ($LASTEXITCODE -ne 0 -or $Fingerprint -notmatch '^[0-9a-f]{64}$') {
                    throw "Canonical fingerprint command failed or returned an invalid digest"
                }
                Set-Content -LiteralPath (Join-Path $ROOT "psxrecomp/generated/$BiosStem.emitter.sha") `
                    -Value $Fingerprint -Encoding ASCII
            }
            catch {
                Write-Warning "Canonical BIOS fingerprint unavailable for ${BiosStem}: $_. Staleness checks remain enabled."
            }
        }
    }

    # --- Step 3: Regenerate game C source from the EXE ---
    Write-Host "==> Regenerating game C code from game/slus_006.64..."
    & $RECOMPILER_BIN "--config" (Join-Path $ROOT "game.toml") `
        "--source-observation-plan" (Join-Path $ROOT "native_renderer/xg_render_resident_plan.txt")
    if ($LASTEXITCODE -ne 0) { throw "Game code regeneration failed" }

    # --- Step 4: Build the game runtime ---
    Write-Host "==> Building game runtime ($BuildType) in $BuildDir..."
    $RUNTIME_CMAKE_ARGS = @(
        "-S", $ROOT,
        "-B", $BUILD_DIR,
        "-DCMAKE_BUILD_TYPE=$RuntimeBuildType",
        "-DPSX_RECOMP_UI=ON",
        "-DRECOMP_UI_ROOT=$(Join-Path $ROOT 'recomp-ui')",
        "-DXG_DISC_IMAGE=$DiscImage",
        "-DXG_RENDER_NATIVE=$NativeRender",
        "-DXG_RECOMPILER_EXECUTABLE=$RECOMPILER_BIN"
    )
    $RUNTIME_CMAKE_ARGS += $RuntimeGeneratorArgs
    $RUNTIME_CMAKE_ARGS += $RuntimeCMakeExtraArgs
    & cmake @RUNTIME_CMAKE_ARGS
    if ($LASTEXITCODE -ne 0) { throw "Runtime configuration failed" }
    if ($BuildType -eq "ReleaseNoOpt") {
        # Configure first so even a new Ninja tree reports its actual frontend.
        $CMakeVersion = (@("MAJOR", "MINOR", "PATCH") | ForEach-Object {
            Get-CMakeCacheValue $BUILD_DIR "CMAKE_CACHE_$($_)_VERSION"
        }) -join "."
        foreach ($Language in @("C", "CXX")) {
            $CompilerFile = Join-Path $BUILD_DIR "CMakeFiles/$CMakeVersion/CMake${Language}Compiler.cmake"
            $CompilerInfo = Get-Content -LiteralPath $CompilerFile -Raw
            if ($CompilerInfo -match "set\(CMAKE_${Language}_COMPILER_ID `"MSVC`"\)" -or
                $CompilerInfo -match "set\(CMAKE_${Language}_COMPILER_FRONTEND_VARIANT `"MSVC`"\)") {
                $Flags = "/Od /DNDEBUG"
            }
            elseif ($CompilerInfo -match "set\(CMAKE_${Language}_COMPILER_ID `"(GNU|Clang|AppleClang)`"\)") {
                $Flags = "-O0 -DNDEBUG"
            }
            else {
                throw "Unsupported $Language compiler for ReleaseNoOpt: $CompilerFile"
            }
            $RUNTIME_CMAKE_ARGS += "-DCMAKE_${Language}_FLAGS_RELEASE=$Flags"
        }
        & cmake @RUNTIME_CMAKE_ARGS
        if ($LASTEXITCODE -ne 0) { throw "ReleaseNoOpt runtime configuration failed" }
    }
    & cmake --build $BUILD_DIR --config $RuntimeBuildType --target psx-runtime --parallel $BuildJobs
    if ($LASTEXITCODE -ne 0) { throw "Runtime build failed" }
}
finally {
    Pop-Location
}

if (Get-CMakeCacheValue $BUILD_DIR "CMAKE_CONFIGURATION_TYPES") {
    $RUNTIME_OUTPUT_DIR = Join-Path $BUILD_DIR $RuntimeBuildType
}
else {
    $RUNTIME_OUTPUT_DIR = $BUILD_DIR
}

if (-not (Test-Path -LiteralPath (Join-Path $RUNTIME_OUTPUT_DIR "XenogearsRecomp.exe") -PathType Leaf)) {
    throw "XenogearsRecomp.exe not found after runtime build in $RUNTIME_OUTPUT_DIR"
}

Write-Host "==> Done. Binary: $(Join-Path $RUNTIME_OUTPUT_DIR 'XenogearsRecomp.exe')"
Write-Host "    Bundled OpenBIOS is staged under $(Join-Path $RUNTIME_OUTPUT_DIR 'bios') and used by default."
Write-Host "    Retail SCPH1001.BIN remains optional at runtime."
