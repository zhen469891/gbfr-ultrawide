# build_tools.ps1
# Build the offline analysis tool gbfr_analyze.exe.
# Invokes MSVC cl.exe directly and compiles the repo's vendored amalgamated
# Zydis (vendor\safetyhook\Zydis.h / Zydis.c) alongside gbfr_analyze.cpp.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\build_tools.ps1
#
# How cl.exe is located:
#   1. If the current shell is already a VS Developer environment (cl.exe on
#      PATH), use it directly.
#   2. Otherwise use vswhere to find the VS install, load vcvars64.bat, then
#      invoke cl.exe.

$ErrorActionPreference = 'Stop'

$toolsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $toolsDir
$zydisDir = Join-Path $repoRoot 'vendor\safetyhook'
$zydisC   = Join-Path $zydisDir 'Zydis.c'
$zydisH   = Join-Path $zydisDir 'Zydis.h'
$srcCpp   = Join-Path $toolsDir 'gbfr_analyze.cpp'
$outExe   = Join-Path $toolsDir 'gbfr_analyze.exe'

foreach ($f in @($srcCpp, $zydisC, $zydisH)) {
    if (-not (Test-Path $f)) { throw "Missing required file: $f" }
}

# ---- locate cl.exe / vcvars64.bat ----
function Find-VcVars64 {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        $alt = Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path $alt) { $vswhere = $alt } else { return $null }
    }
    $vsPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
    if ([string]::IsNullOrWhiteSpace($vsPath)) { return $null }
    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path $vcvars) { return $vcvars }
    return $null
}

# Compile flags (mirrors scratchpad: cl /O2 /EHsc /I. /DZYDIS_STATIC_BUILD ... Zydis.c)
# Zydis.h already #defines ZYDIS_STATIC_BUILD / ZYCORE_STATIC_BUILD, so we do not
# pass them again (avoids C4005 redefinition warnings).
# Quote each path. Object dir goes to a subfolder ending in '\\' so the trailing
# backslash before the closing quote is not treated as an escaped quote by cl.
$objDir = Join-Path $toolsDir 'obj'
New-Item -ItemType Directory -Force $objDir | Out-Null
$clArgLine = @(
    '/nologo', '/O2', '/EHsc', '/MT',
    "/I`"$zydisDir`"",
    "/Fe`"$outExe`"",
    "/Fo`"$objDir\\`"",
    "`"$srcCpp`"", "`"$zydisC`""
) -join ' '

# Always load a full MSVC environment via vcvars64: cl.exe may be on PATH without
# INCLUDE/LIB being set (e.g. missing cstdio/assert.h), so we cannot rely on PATH.
$vcvars = Find-VcVars64
if ($null -eq $vcvars) {
    # Fall back to a PATH cl.exe only if the shell already looks like a full
    # Developer environment (INCLUDE set); otherwise fail with a clear message.
    if ((Get-Command cl.exe -ErrorAction SilentlyContinue) -and $env:INCLUDE) {
        Write-Host "Using cl.exe from current Developer environment."
        & cmd.exe /c "cl.exe $clArgLine"
        $code = $LASTEXITCODE
    } else {
        throw 'vcvars64.bat not found. Install Visual Studio (with C++ tools) or run this from a VS x64 Developer prompt.'
    }
} else {
    Write-Host "Loading MSVC env via vcvars64: $vcvars"
    & cmd.exe /c "call `"$vcvars`" >nul 2>&1 && cl.exe $clArgLine"
    $code = $LASTEXITCODE
}

# clean intermediate .obj
Remove-Item -Recurse -Force $objDir -ErrorAction SilentlyContinue

if ($code -ne 0) {
    throw "Build failed (exit code $code)."
}
if (-not (Test-Path $outExe)) {
    throw "Build finished but output not found: $outExe"
}

Write-Host ''
Write-Host "Build OK -> $outExe"
