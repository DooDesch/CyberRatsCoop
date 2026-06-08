# Shared paths/helpers for the Cyber Rats Co-op tooling scripts.
# Dot-source this:  . "$PSScriptRoot\common.ps1"

$ErrorActionPreference = 'Stop'

# --- Project + game locations -------------------------------------------------
$Global:CR_ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Global:CR_GameRoot    = 'F:\Launcher\SteamLibrary\steamapps\common\Cyber Rats'
$Global:CR_Win64       = Join-Path $CR_GameRoot 'Engine\Binaries\Win64'
$Global:CR_ShippingExe = Join-Path $CR_Win64 'UnrealGame-Win64-Shipping.exe'
$Global:CR_DevExe      = Join-Path $CR_Win64 'UnrealGame.exe'
# Modern UE4SS (3.0.1+) layout: dwmapi.dll next to exe, UE4SS.dll inside a 'ue4ss\' folder,
# mods default to <ue4ss>\Mods.
$Global:CR_Ue4ssDir    = Join-Path $CR_Win64 'ue4ss'
$Global:CR_ModsDir     = Join-Path $CR_Ue4ssDir 'Mods'
$Global:CR_AppId       = 3565080

function CR-Info  ($m) { Write-Host "[setup] $m"  -ForegroundColor Cyan }
function CR-Ok    ($m) { Write-Host "[ ok ] $m"   -ForegroundColor Green }
function CR-Warn  ($m) { Write-Host "[warn] $m"   -ForegroundColor Yellow }
function CR-Err   ($m) { Write-Host "[fail] $m"   -ForegroundColor Red }

function CR-AssertGame {
    if (-not (Test-Path -LiteralPath $CR_Win64)) {
        throw "Game Win64 folder not found: $CR_Win64  (ist das Spiel an anderem Ort installiert? CR_GameRoot in common.ps1 anpassen.)"
    }
    if (-not (Test-Path -LiteralPath $CR_ShippingExe)) {
        CR-Warn "Shipping exe nicht gefunden: $CR_ShippingExe"
    }
}

# Copy a tree, creating parents, overwriting files (idempotent).
function CR-CopyTree ($Src, $Dst) {
    if (-not (Test-Path -LiteralPath $Src)) { throw "Source not found: $Src" }
    New-Item -ItemType Directory -Force -Path $Dst | Out-Null
    Copy-Item -LiteralPath $Src -Destination $Dst -Recurse -Force
}

# Ensure a 'Name : 1' line exists in a UE4SS mods.txt (enable a mod).
function CR-EnableInModsTxt ($ModsTxt, $ModName) {
    $lines = @()
    if (Test-Path -LiteralPath $ModsTxt) { $lines = Get-Content -LiteralPath $ModsTxt }
    $pattern = "^\s*$([regex]::Escape($ModName))\s*:"
    $found = $false
    $out = foreach ($l in $lines) {
        if ($l -match $pattern) { $found = $true; "$ModName : 1" } else { $l }
    }
    if (-not $found) {
        # Insert before any trailing built-in lines; simplest: append.
        $out = @($out) + "$ModName : 1"
    }
    Set-Content -LiteralPath $ModsTxt -Value $out -Encoding UTF8
}
