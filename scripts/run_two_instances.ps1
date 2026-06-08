<#
.SYNOPSIS
  Launches Cyber Rats twice locally for co-op testing (ENet/loopback transport).

.DESCRIPTION
  Starts the shipping exe (or -Dev for the Development exe) two times with separate
  -UserDir so saves/configs don't clash, passing co-op role args the C++ mod reads
  from the command line (FCommandLine):

      -CRCoopRole=host|client   -CRCoopPort=<port>   -CRCoopConnect=<ip>

  For M0 (single-instance introspection) you don't need this — just launch once via
  Steam. This script is for M1+ when the netcode exists.

  NOTE: if the game enforces a single-instance mutex (risk U7), the second instance
  may refuse to start; then clone the install or hook the mutex. We'll learn this at runtime.

.PARAMETER Dev
  Use UnrealGame.exe (Development, richer logging) instead of the Shipping exe.

.PARAMETER Port
  Loopback port for the host listen / client connect (default 7777).
#>
[CmdletBinding()]
param(
    [switch]$Dev,
    [int]$Port = 7777
)

. "$PSScriptRoot\common.ps1"
CR-AssertGame

$exe = if ($Dev) { $CR_DevExe } else { $CR_ShippingExe }
if (-not (Test-Path -LiteralPath $exe)) { throw "Exe nicht gefunden: $exe" }

$base = Join-Path $env:TEMP 'CyberRatsCoop'
$hostUserDir   = Join-Path $base 'host'
$clientUserDir = Join-Path $base 'client'
New-Item -ItemType Directory -Force -Path $hostUserDir, $clientUserDir | Out-Null

CR-Info "Exe: $exe"
CR-Info "Host  -> Port $Port  (UserDir: $hostUserDir)"
CR-Info "Client-> 127.0.0.1:$Port (UserDir: $clientUserDir)"

$hostArgs = @(
    "-UserDir=`"$hostUserDir`"",
    "-CRCoopRole=host",
    "-CRCoopPort=$Port"
)
$clientArgs = @(
    "-UserDir=`"$clientUserDir`"",
    "-CRCoopRole=client",
    "-CRCoopConnect=127.0.0.1",
    "-CRCoopPort=$Port"
)

CR-Info "Starte Host..."
Start-Process -FilePath $exe -ArgumentList $hostArgs
Start-Sleep -Seconds 8   # Host zuerst hochfahren lassen, bevor Client verbindet
CR-Info "Starte Client..."
Start-Process -FilePath $exe -ArgumentList $clientArgs

CR-Ok "Beide Instanzen gestartet. UE4SS.log in den jeweiligen UserDirs pruefen."
CR-Warn "Falls die zweite Instanz nicht startet: Single-Instance-Mutex (U7) -> Install klonen oder Mutex hooken."
