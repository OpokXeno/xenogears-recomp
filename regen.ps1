<#
.SYNOPSIS
    Regenerate the recompiled C output for the Xenogears main EXE (Windows).
.DESCRIPTION
    Runs psxrecomp-game with game.toml to produce the generated C source
    under generated/. Requires the recompiler to be built first (run build.ps1
    or build the psxrecomp-game target manually).
.NOTES
    Requires: psxrecomp/recompiler/build/psxrecomp-game.exe
    Inputs:   game.toml, seeds/slus_00664_seeds.txt, game/slus_006.64
    Outputs:  generated/slus_006.64_full_*.c
#>

$ROOT = Split-Path -Parent $MyInvocation.MyCommand.Path
$RECOMPILER_BIN = Join-Path $ROOT "psxrecomp/recompiler/build/psxrecomp-game.exe"

if (-not (Test-Path $RECOMPILER_BIN)) {
    # Multi-config generators (Visual Studio) place the binary under Release/
    $RECOMPILER_BIN = Join-Path $ROOT "psxrecomp/recompiler/build/Release/psxrecomp-game.exe"
}

if (-not (Test-Path $RECOMPILER_BIN)) {
    Write-Host "ERROR: psxrecomp-game not found at psxrecomp/recompiler/build/psxrecomp-game.exe" -ForegroundColor Red
    Write-Host "Build the recompiler first: .\build.ps1" -ForegroundColor Yellow
    exit 1
}

$CONFIG = Join-Path $ROOT "game.toml"
if (-not (Test-Path $CONFIG)) {
    Write-Host "ERROR: game.toml not found at $CONFIG" -ForegroundColor Red
    exit 1
}

Write-Host "==> Regenerating game C code..."
Write-Host "    Recompiler: $RECOMPILER_BIN"
Write-Host "    Config:     $CONFIG"
& $RECOMPILER_BIN "--config" $CONFIG
if ($LASTEXITCODE -ne 0) {
    throw "Regeneration failed (exit code $LASTEXITCODE)"
}
Write-Host "==> Done. Generated sources in generated/"
