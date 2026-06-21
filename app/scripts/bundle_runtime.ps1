#requires -Version 5
<#
.SYNOPSIS
    Stages every runtime DLL remote-desktop.exe needs to launch on a fresh
    box (no VS, no Qt, no FFmpeg installed).

.DESCRIPTION
    Runs as a stand-alone step AFTER cmake --build. CMake's POST_BUILD only
    fires when the target relinks, which is silently wrong on incremental
    builds — the .exe stays current but its bundled DLLs go stale. Doing
    it as a VS Code task keeps the dev cycle reproducible.

    Copies into $OutputDir:
      1. FFmpeg DLLs from $FfmpegBin (avcodec / avutil / swscale / libx264 / ...)
      2. Qt runtime + plugin subdirs via windeployqt from $QtBin
      3. MSVC runtime (vcruntime140.dll, msvcp140.dll, concrt140.dll, ...)
         from the latest VS install detected via vswhere

    Always exits 0. Real failures only print a WARNING; the missing-DLLs
    summary at the end is what you actually act on.

.PARAMETER OutputDir
    Where remote-desktop.exe lives (e.g. app/build/Release).

.PARAMETER FfmpegBin
    vcpkg's installed/x64-windows/bin directory.

.PARAMETER QtBin
    Qt msvc2022_64/bin directory (contains windeployqt.exe).
#>

param(
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [Parameter(Mandatory=$true)][string]$FfmpegBin,
    [Parameter(Mandatory=$true)][string]$QtBin
)

$ErrorActionPreference = "Continue"

function Step($msg) { Write-Host "[bundle] $msg" }

if (-not (Test-Path $OutputDir)) {
    Step "ERROR: OutputDir does not exist: $OutputDir"
    exit 1
}
$exe = Join-Path $OutputDir "remote-desktop.exe"
if (-not (Test-Path $exe)) {
    Step "ERROR: $exe not found — compile the app first"
    exit 1
}

# 1. FFmpeg DLLs + transitive vcpkg deps
if (Test-Path $FfmpegBin) {
    Step "copying FFmpeg DLLs from $FfmpegBin"
    Get-ChildItem -Path $FfmpegBin -Filter "*.dll" -EA SilentlyContinue | ForEach-Object {
        Copy-Item -Path $_.FullName -Destination $OutputDir -Force -EA SilentlyContinue
    }
} else {
    Step "WARNING: FfmpegBin not found: $FfmpegBin"
}

# 2. Qt runtime + plugins
$deployer = Join-Path $QtBin "windeployqt.exe"
if (Test-Path $deployer) {
    Step "running windeployqt..."
    & $deployer --no-translations --no-system-d3d-compiler --no-opengl-sw $exe 2>&1 | Out-Null
} else {
    Step "WARNING: windeployqt.exe not found at $deployer"
}

# 3. MSVC runtime via vswhere → Redist\MSVC\<latest>\x64\Microsoft.VC*.CRT
$vsInstall = $null
$vswhere = Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
}
if (Test-Path $vswhere) {
    $vsInstall = & $vswhere -latest -property installationPath
}
if ($vsInstall) {
    $redist = Join-Path $vsInstall "VC\Redist\MSVC"
    if (Test-Path $redist) {
        # Skip alias dirs like "v145" — they don't actually hold the CRT
        # files. The real ones look like "14.51.36231". Look for any subdir
        # whose x64\Microsoft.VC*.CRT exists, prefer the highest version.
        $crt = Get-ChildItem $redist -Directory -EA SilentlyContinue |
               Where-Object { $_.Name -match '^\d' } |
               Sort-Object Name -Descending |
               ForEach-Object {
                   Get-ChildItem (Join-Path $_.FullName "x64") -Filter "Microsoft.VC*.CRT" -Directory -EA SilentlyContinue
               } | Select-Object -First 1
        if ($crt) {
            Step "copying MSVC runtime from $($crt.FullName)"
            Get-ChildItem (Join-Path $crt.FullName "*.dll") -EA SilentlyContinue |
                Copy-Item -Destination $OutputDir -Force -EA SilentlyContinue
        } else {
            Step "WARNING: no numeric-version dir with Microsoft.VC*.CRT under $redist"
        }
    }
} else {
    Step "WARNING: vswhere couldn't find a VS install — MSVC runtime not copied"
}

# 4. Verify everything you actually need is now in place
$required = @(
    "avcodec-61.dll", "avutil-59.dll", "swscale-8.dll", "swresample-5.dll",
    "libx264-164.dll",
    "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll",
    "vcruntime140.dll", "msvcp140.dll"
)
$missing = @()
foreach ($n in $required) {
    if (-not (Test-Path (Join-Path $OutputDir $n))) { $missing += $n }
}
if ($missing.Count -gt 0) {
    Step "WARNING: still missing after bundling: $($missing -join ', ')"
} else {
    Step "OK — all critical DLLs present"
}

exit 0
