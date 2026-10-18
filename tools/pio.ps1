# Run PlatformIO for this repo with its OWN toolchain.
#
# The pioarduino platforms that support the ESP32-C5 / ESP-IDF 6 need
# PlatformIO Core >= 6.2 (the pioarduino fork). The machine-wide
# ~/.platformio install is older and is shared with FPV_Lap_Timer, so this
# repo uses a private core instead:
#   - the CLI lives in .venv (pip install pioarduino)
#   - platforms/packages live in ~/.pio-c5 (short path: ESP-IDF breaks on
#     long Windows paths)
#
# One-time setup (from the repo root, PowerShell):
#   python -m venv .venv
#   .\.venv\Scripts\python.exe -m pip install pioarduino
#
# Usage:  .\tools\pio.ps1 run
#         .\tools\pio.ps1 run -t upload --upload-port COM8
#         .\tools\pio.ps1 device monitor --port COM8
#
# Run from PowerShell or cmd, not Git Bash: the ESP-IDF tool installer
# refuses to run under MSYS.

$repo = Split-Path -Parent $PSScriptRoot
$pio  = Join-Path $repo ".venv\Scripts\pio.exe"
if (-not (Test-Path $pio)) {
    Write-Error "No .venv\Scripts\pio.exe - run the one-time setup in tools\pio.ps1"
    exit 1
}
$env:PLATFORMIO_CORE_DIR = Join-Path $env:USERPROFILE ".pio-c5"
Push-Location $repo
try {
    & $pio @args
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
