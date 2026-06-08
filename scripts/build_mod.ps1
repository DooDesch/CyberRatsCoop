<#
.SYNOPSIS
  Builds the CyberRatsCoop UE4SS C++ mod (xmake + clang) and deploys the dll into the game.
.DESCRIPTION
  Forces -p windows (xmake otherwise picks mingw under Git Bash). Builds against the UE4SS source
  in third_party/RE-UE4SS. First build also compiles UE4SS + deps (long; needs Rust nightly for
  patternsleuth). See docs/build.md.
#>
[CmdletBinding()]
param(
    [ValidateSet('Game__Shipping__Win64','Game__Debug__Win64','Game__Development__Win64')]
    [string]$Mode = 'Game__Shipping__Win64',
    [switch]$Rebuild
)
. "$PSScriptRoot\common.ps1"

# Tools onto PATH (cargo for patternsleuth; MSVC auto-detected). Use the pinned xmake 2.9.9 (UE4SS
# pins 2.9.3; xmake 3.0.x changed cmake.install and breaks raw_pdb), else fall back to PATH xmake.
$pinnedXmake = Join-Path $CR_ProjectRoot 'third_party\xmake293\xmake'
$env:PATH = "$pinnedXmake;$env:USERPROFILE\.cargo\bin;$env:USERPROFILE\scoop\shims;$env:PATH"
# Build from the UE4SS ROOT so `ue4ssRoot` is set and UE4SS's custom package repo (deps/third-repo,
# providing a working raw_pdb) is registered. The mod is junctioned into cppmods/CyberRatsCoop.
$ue4ssRoot = Join-Path $CR_ProjectRoot 'third_party\RE-UE4SS'
if (-not (Test-Path (Join-Path $ue4ssRoot 'cppmods\CyberRatsCoop'))) { CR-Err "cppmods\CyberRatsCoop junction fehlt"; exit 1 }
if (-not (Get-Command xmake -ErrorAction SilentlyContinue)) { CR-Err "xmake fehlt (scoop install xmake)"; exit 1 }

Push-Location $ue4ssRoot
try {
    CR-Info "Configure ($Mode, native MSVC) from UE4SS root..."
    & xmake f -p windows -a x64 -m $Mode -y
    if ($Rebuild) { & xmake clean CyberRatsCoop }
    CR-Info "Build (first run compiles UE4SS + deps; long)..."
    & xmake build -y CyberRatsCoop
    if ($LASTEXITCODE -ne 0) { CR-Err "Build fehlgeschlagen ($LASTEXITCODE)"; exit 1 }
    $dll = Get-ChildItem -Recurse -Filter 'CyberRatsCoop.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $dll) { CR-Err "Keine CyberRatsCoop.dll erzeugt"; exit 1 }
    CR-Ok "Gebaut: $($dll.FullName)"
    # Deploy into the game as Mods/CyberRatsCoop/dlls/main.dll
    $dstDir = Join-Path $CR_ModsDir 'CyberRatsCoop\dlls'
    New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
    Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $dstDir 'main.dll') -Force
    New-Item -ItemType File -Force -Path (Join-Path $CR_ModsDir 'CyberRatsCoop\enabled.txt') | Out-Null
    Copy-Item -LiteralPath (Join-Path $CR_ProjectRoot 'config\coop.ini') -Destination (Join-Path $CR_ModsDir 'CyberRatsCoop') -Force
    CR-EnableInModsTxt (Join-Path $CR_ModsDir 'mods.txt') 'CyberRatsCoop'
    CR-Ok "Deployed -> $dstDir\main.dll (+ coop.ini, enabled in mods.txt)"
} finally { Pop-Location }
