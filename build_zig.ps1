param([Parameter(Mandatory=$true)][string]$Zig, [switch]$Dev)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $out | Out-Null
& $Zig cc -target x86_64-windows-gnu -O2 "-I$root/vendor/safetyhook" -c "$root/vendor/safetyhook/Zydis.c" -o "$out/Zydis.o"
if ($LASTEXITCODE) { throw 'Zydis compilation failed' }
$defs = @('-DGBFR_VERSION_STRING="0.3.1"')
if ($Dev) { $defs += '-DGBFR_DEVBUILD' }
& $Zig c++ -target x86_64-windows-gnu -std=c++23 -O2 -shared -s @defs "-I$root/vendor" "-I$root/vendor/safetyhook" "-I$root/src" "$root/src/dllmain.cpp" "$root/vendor/safetyhook/safetyhook.cpp" "$out/Zydis.o" -o "$out/GBFRUltrawide.asi"
if ($LASTEXITCODE) { throw 'Plugin compilation failed' }
Copy-Item -LiteralPath "$root/ini/GBFRUltrawide.ini" -Destination $out -Force
