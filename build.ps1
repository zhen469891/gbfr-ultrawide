# GBFRUltrawide - one-shot configure + build script
# -Dev builds a development build (GBFR_DEV=ON -> honors the [Debug - Span HUD]
# ini section); default is a release build. The option is passed explicitly on
# every configure so it never sticks from a previous build directory.
param([switch]$Dev)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

$devFlag = if ($Dev) { "ON" } else { "OFF" }
cmake -S $root -B "$root\build" -G "Visual Studio 17 2022" -A x64 "-DGBFR_DEV=$devFlag"
if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed."; exit 1 }

cmake --build "$root\build" --config Release
if ($LASTEXITCODE -ne 0) { Write-Error "Build failed."; exit 1 }

# Collect artifacts into dist\
New-Item -ItemType Directory -Force -Path "$root\dist" | Out-Null
Copy-Item "$root\build\Release\GBFRUltrawide.asi" "$root\dist\" -Force
Copy-Item "$root\ini\GBFRUltrawide.ini" "$root\dist\" -Force

Write-Host ""
if ($Dev) {
    Write-Host "Build OK (DEV) -> $root\dist\GBFRUltrawide.asi"
} else {
    Write-Host "Build OK -> $root\dist\GBFRUltrawide.asi"
}
