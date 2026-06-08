<#
.SYNOPSIS
  Copies the Mappings.usmap that UE4SS' `Dumper_Usmap` generated in the game folder
  into the project's mappings\ directory.
#>
[CmdletBinding()] param()
. "$PSScriptRoot\common.ps1"

$candidates = @(
    (Join-Path $CR_Win64 'Mappings.usmap'),
    (Join-Path $CR_Win64 'Dumper\Mappings.usmap'),
    (Join-Path $CR_Win64 'UE4SS\Mappings.usmap')
)
$src = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $src) {
    CR-Err "Keine Mappings.usmap gefunden. In der UE4SS GUI-Console `Dumper_Usmap` ausfuehren."
    CR-Info "Gesucht in:`n  $($candidates -join "`n  ")"
    exit 1
}
$dstDir = Join-Path $CR_ProjectRoot 'mappings'
New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
Copy-Item -LiteralPath $src -Destination (Join-Path $dstDir 'Mappings.usmap') -Force
CR-Ok "Mappings.usmap -> $dstDir  (Quelle: $src)"
CR-Info "In FModel als Mapping-Datei laden (Settings -> General -> Mappings)."
