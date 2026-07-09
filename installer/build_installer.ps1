# build_installer.ps1
# Compiles GBFRUltrawideSetup.exe with the csc.exe bundled in .NET Framework 4.x,
# and assembles the full release layout under installer\out\ (exe + payload\).
# Usage: powershell -ExecutionPolicy Bypass -File installer\build_installer.ps1

$ErrorActionPreference = 'Stop'

$installerDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot     = Split-Path -Parent $installerDir
$outDir       = Join-Path $installerDir 'out'
$payloadDir   = Join-Path $outDir 'payload'
$exePath      = Join-Path $outDir 'GBFRUltrawideSetup.exe'

New-Item -ItemType Directory -Force $outDir | Out-Null

# ---- Locate csc.exe (prefer x64, fall back to x86) ----
$cscCandidates = @(
    (Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'),
    (Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe')
)
$csc = $null
foreach ($c in $cscCandidates) {
    if (Test-Path $c) { $csc = $c; break }
}
if ($null -eq $csc) {
    throw 'Could not find .NET Framework csc.exe (v4.0.30319); cannot build.'
}
Write-Host "Using compiler: $csc"

# ---- Compile ----
# Glob every .cs in installer\ so new source files are picked up automatically
# (avoids the source list drifting out of sync with the CI workflows).
$sources = @(Get-ChildItem -Path (Join-Path $installerDir '*.cs') | ForEach-Object { $_.FullName })
if ($sources.Count -eq 0) { throw "No .cs source files found in $installerDir" }

$cscArgs = @(
    '/nologo',
    '/target:winexe',
    '/platform:anycpu',
    '/optimize+',
    '/codepage:65001',
    "/win32manifest:$(Join-Path $installerDir 'app.manifest')",
    '/r:System.dll',
    '/r:System.Core.dll',
    '/r:System.Windows.Forms.dll',
    '/r:System.Drawing.dll',
    "/out:$exePath"
) + $sources

& $csc @cscArgs
if ($LASTEXITCODE -ne 0) {
    throw "csc.exe build failed (exit code $LASTEXITCODE)."
}
Write-Host "Build complete: $exePath"

# ---- Assemble payload ----
# payload\ mirrors the game-directory layout: winmm.dll at the root, the GBFRUltrawide.*
# files under scripts\. The installer deploys this tree verbatim, and a manual user can just
# copy the contents of payload\ into their game folder.
$payloadScriptsDir = Join-Path $payloadDir 'scripts'
# Start from a clean payload dir so files from a previous build/layout don't linger.
if (Test-Path $payloadDir) { Remove-Item -Recurse -Force $payloadDir }
New-Item -ItemType Directory -Force $payloadDir | Out-Null
New-Item -ItemType Directory -Force $payloadScriptsDir | Out-Null

# winmm.dll (Ultimate ASI Loader x64) comes from redist\ -> payload\winmm.dll
$winmm = Join-Path $repoRoot 'redist\winmm.dll'
if (Test-Path $winmm) {
    Copy-Item $winmm (Join-Path $payloadDir 'winmm.dll') -Force
    Write-Host 'payload\winmm.dll  <-  redist\winmm.dll'
} else {
    Write-Warning "$winmm not found; payload is missing winmm.dll."
}

# GBFRUltrawide.asi / .ini: prefer dist\, otherwise the newest build\ artifact -> payload\scripts\
foreach ($name in @('GBFRUltrawide.asi', 'GBFRUltrawide.ini')) {
    $src = Join-Path $repoRoot ("dist\" + $name)
    if (-not (Test-Path $src)) {
        $found = Get-ChildItem -Path (Join-Path $repoRoot 'build') -Recurse -Filter $name -ErrorAction SilentlyContinue |
                 Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($null -ne $found) { $src = $found.FullName } else { $src = $null }
    }
    if ($null -ne $src -and (Test-Path $src)) {
        Copy-Item $src (Join-Path $payloadScriptsDir $name) -Force
        Write-Host ("payload\scripts\" + $name + "  <-  " + $src)
    } else {
        $note = Join-Path $payloadScriptsDir ($name + '.MISSING.txt')
        ("This build is missing " + $name + ". Run build.ps1 in the repo root first to produce dist\" + $name + ", then re-run installer\build_installer.ps1.") |
            Out-File -FilePath $note -Encoding utf8
        Write-Warning ($name + " not found; placed a note file in payload.")
    }
}

Write-Host ''
Write-Host "Release layout ready: $outDir"
Get-ChildItem -Recurse $outDir | ForEach-Object { Write-Host ('  ' + $_.FullName.Substring($outDir.Length + 1)) }
