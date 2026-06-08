<#
.SYNOPSIS
  Installs UE4SS (UE5.6-capable experimental build) into the Cyber Rats shipping-exe folder
  and patches UE4SS-settings.ini for this game.

.DESCRIPTION
  UE4SS must live next to the exe that runs the UE module: <Game>\Engine\Binaries\Win64\.

  Modern UE4SS (3.0.1+) layout installed by this script:
      Win64\dwmapi.dll                 (proxy loader)
      Win64\ue4ss\UE4SS.dll
      Win64\ue4ss\UE4SS-settings.ini   (patched: EngineVersionOverride 5.6, GUI console on)
      Win64\ue4ss\Mods\                (deploy.ps1 puts mods here)

  Zip source resolution order:
    1) -Ue4ssZip <path>          explicit local zip
    2) -Ue4ssZipUrl <url>        explicit URL
    3) newest UE4SS_*.zip in third_party\ue4ss\_download\   (pre-downloaded; pinned)
    4) GitHub API: newest non-zDEV UE4SS_*.zip from the latest (pre)release

  Pinned/tested build: see third_party\ue4ss\VERSION.txt
#>
[CmdletBinding()]
param(
    [string]$Ue4ssZip,
    [string]$Ue4ssZipUrl,
    [switch]$Force
)

. "$PSScriptRoot\common.ps1"
CR-AssertGame

$stage = Join-Path $CR_ProjectRoot 'third_party\ue4ss'
$dl    = Join-Path $stage '_download'
New-Item -ItemType Directory -Force -Path $stage, $dl | Out-Null

# --- 1. Obtain a UE4SS zip ----------------------------------------------------
$zipPath = $null
if ($Ue4ssZip) {
    if (-not (Test-Path -LiteralPath $Ue4ssZip)) { throw "Zip not found: $Ue4ssZip" }
    $zipPath = (Resolve-Path -LiteralPath $Ue4ssZip).Path
    CR-Info "Using local UE4SS zip: $zipPath"
}
elseif ($Ue4ssZipUrl) {
    $zipPath = Join-Path $dl 'ue4ss.zip'
    CR-Info "Downloading UE4SS: $Ue4ssZipUrl"
    Invoke-WebRequest -Uri $Ue4ssZipUrl -OutFile $zipPath
}
else {
    $pre = Get-ChildItem -LiteralPath $dl -Filter 'UE4SS_*.zip' -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -notmatch 'zDEV' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($pre) {
        $zipPath = $pre.FullName
        CR-Info "Using pre-downloaded zip: $($pre.Name)"
    } else {
        CR-Info "Querying GitHub for the latest UE4SS (incl. prereleases / experimental)..."
        try {
            $rel = Invoke-RestMethod -Uri 'https://api.github.com/repos/UE4SS-RE/RE-UE4SS/releases' `
                                     -Headers @{ 'User-Agent' = 'CyberRatsCoop-setup' }
            $asset = $null
            foreach ($r in $rel) {
                $a = $r.assets | Where-Object { $_.name -match '^UE4SS_.*\.zip$' -and $_.name -notmatch 'zDEV' } | Select-Object -First 1
                if ($a) { $asset = $a; CR-Info "Picked $($a.name) from '$($r.tag_name)'"; break }
            }
            if (-not $asset) { throw "No suitable UE4SS_*.zip asset found." }
            $zipPath = Join-Path $dl $asset.name
            Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zipPath
        } catch {
            CR-Err "Auto-download failed: $($_.Exception.Message)"
            CR-Warn "Lade UE4SS manuell (experimental, UE5.6) von https://github.com/UE4SS-RE/RE-UE4SS/releases und nutze -Ue4ssZip <pfad>."
            throw
        }
    }
}

# --- 2. Extract ---------------------------------------------------------------
$extract = Join-Path $stage 'extracted'
if (Test-Path -LiteralPath $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
New-Item -ItemType Directory -Force -Path $extract | Out-Null
CR-Info "Extracting..."
Expand-Archive -LiteralPath $zipPath -DestinationPath $extract -Force

# Locate the proxy (dwmapi.dll, at zip root) and the ue4ss\ payload folder (contains UE4SS.dll).
$proxySrc = Get-ChildItem -LiteralPath $extract -Recurse -Filter 'dwmapi.dll' | Select-Object -First 1
$ue4ssDll = Get-ChildItem -LiteralPath $extract -Recurse -Filter 'UE4SS.dll'  | Select-Object -First 1
if (-not $ue4ssDll) { throw "UE4SS.dll not found in zip — is this a UE4SS release?" }
if (-not $proxySrc) { throw "dwmapi.dll (proxy) not found in zip." }
$ue4ssPayload = $ue4ssDll.Directory.FullName   # the 'ue4ss' folder
CR-Ok "UE4SS payload: $ue4ssPayload"

# Pin version
@"
source_zip=$([IO.Path]::GetFileName($zipPath))
ue4ss_dll_sha256=$((Get-FileHash -LiteralPath $ue4ssDll.FullName -Algorithm SHA256).Hash)
installed_utc=$(Get-Date -AsUTC -Format o)
layout=modern (dwmapi.dll + ue4ss\)
"@ | Set-Content -LiteralPath (Join-Path $stage 'VERSION.txt') -Encoding UTF8

# --- 3. Install into the game -------------------------------------------------
CR-Info "Installing into $CR_Win64"

# Proxy DLL next to the exe
$proxyDst = Join-Path $CR_Win64 'dwmapi.dll'
if ((Test-Path -LiteralPath $proxyDst) -and -not $Force) {
    CR-Warn "dwmapi.dll existiert bereits — ueberspringe (mit -Force ueberschreiben)."
} else {
    Copy-Item -LiteralPath $proxySrc.FullName -Destination $proxyDst -Force
    CR-Ok "dwmapi.dll"
}

# ue4ss\ folder next to the exe (preserve an existing patched settings unless -Force)
$existingSettings = Join-Path $CR_Ue4ssDir 'UE4SS-settings.ini'
$savedSettings = $null
if ((Test-Path -LiteralPath $existingSettings) -and -not $Force) {
    $savedSettings = Get-Content -LiteralPath $existingSettings -Raw
}
New-Item -ItemType Directory -Force -Path $CR_Ue4ssDir | Out-Null
Copy-Item -Path (Join-Path $ue4ssPayload '*') -Destination $CR_Ue4ssDir -Recurse -Force
if ($savedSettings) { Set-Content -LiteralPath $existingSettings -Value $savedSettings -Encoding UTF8; CR-Info "vorhandene UE4SS-settings.ini erhalten" }
New-Item -ItemType Directory -Force -Path $CR_ModsDir | Out-Null
CR-Ok "ue4ss\ (UE4SS.dll, settings, Mods\)"

# --- 4. Patch UE4SS-settings.ini (in-place; keys already exist in their sections) ---
function Set-IniKeyInPlace([string]$path, [string]$key, [string]$value) {
    $content = Get-Content -LiteralPath $path
    $re = "^\s*$([regex]::Escape($key))\s*="
    if ($content -match $re) {
        ($content -replace "$re.*", "$key = $value") | Set-Content -LiteralPath $path -Encoding UTF8
        return $true
    }
    return $false   # don't append: would land in the wrong INI section
}

$settings = Join-Path $CR_Ue4ssDir 'UE4SS-settings.ini'
if (Test-Path -LiteralPath $settings) {
    CR-Info "Patching UE4SS-settings.ini"
    foreach ($kv in @(
        @('MajorVersion','5'), @('MinorVersion','6'),
        @('ConsoleEnabled','1'), @('GuiConsoleEnabled','1'), @('GuiConsoleVisible','1')
    )) {
        if (Set-IniKeyInPlace $settings $kv[0] $kv[1]) { CR-Ok "  $($kv[0]) = $($kv[1])" }
        else { CR-Warn "  Key $($kv[0]) nicht gefunden — manuell setzen." }
    }
} else {
    CR-Warn "UE4SS-settings.ini fehlt im Payload — Spiel einmal starten, dann setup -Force erneut."
}

# --- 5. Deploy local UE4SS overrides (custom AOB signatures for this UE5.6 build) ---
$overrides = Join-Path $CR_ProjectRoot 'third_party\ue4ss\overrides'
if (Test-Path -LiteralPath $overrides) {
    Copy-Item -Path (Join-Path $overrides '*') -Destination $CR_Ue4ssDir -Recurse -Force
    CR-Ok "overrides (UE4SS_Signatures\StaticConstructObject.lua)"
}

CR-Ok "UE4SS-Installation fertig."
CR-Info "Naechster Schritt:  pwsh scripts\deploy.ps1"
CR-Info "Steam -> Eigenschaften -> Startoptionen:  `"$CR_ShippingExe`" %command%"
