# GBFRUltrawide - one-shot configure + build script
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

cmake -S $root -B "$root\build" -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed."; exit 1 }

cmake --build "$root\build" --config Release
if ($LASTEXITCODE -ne 0) { Write-Error "Build failed."; exit 1 }

# Collect artifacts into dist\
New-Item -ItemType Directory -Force -Path "$root\dist" | Out-Null
Copy-Item "$root\build\Release\GBFRUltrawide.asi" "$root\dist\" -Force
Copy-Item "$root\ini\GBFRUltrawide.ini" "$root\dist\" -Force

Write-Host ""
Write-Host "Build OK -> $root\dist\GBFRUltrawide.asi"
