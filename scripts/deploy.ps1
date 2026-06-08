<#
.SYNOPSIS
  Deploys the project's UE4SS mods into the game's Mods folder and enables them.

.DESCRIPTION
  Copies (or links) the Lua mods from lua\ and the built C++ mod from
  mod\CyberRatsCoop\dlls\ into <Game>\Engine\Binaries\Win64\Mods\, copies config\coop.ini
  next to the coop mod, and enables them in mods.txt.

  -Link creates directory junctions for the Lua mods instead of copying, so edits
  hot-reload in-game (use during development). Default is copy (for distribution).

.PARAMETER Only
  Deploy a subset: 'toolkit' (CRToolkit only), 'coop' (CyberRatsCoop only), or 'all' (default).
#>
[CmdletBinding()]
param(
    [switch]$Link,
    [ValidateSet('all','toolkit','coop')] [string]$Only = 'all'
)

. "$PSScriptRoot\common.ps1"
CR-AssertGame
New-Item -ItemType Directory -Force -Path $CR_ModsDir | Out-Null

function Deploy-LuaMod($name, $srcDir) {
    $dst = Join-Path $CR_ModsDir $name
    if (Test-Path -LiteralPath $dst) {
        # remove an existing junction or copied folder
        $item = Get-Item -LiteralPath $dst -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            (Get-Item -LiteralPath $dst).Delete()
        } else {
            Remove-Item -LiteralPath $dst -Recurse -Force
        }
    }
    if ($Link) {
        New-Item -ItemType Junction -Path $dst -Target $srcDir | Out-Null
        CR-Ok "$name -> junction -> $srcDir"
    } else {
        Copy-Item -LiteralPath $srcDir -Destination $dst -Recurse -Force
        CR-Ok "$name -> copied"
    }
    CR-EnableInModsTxt (Join-Path $CR_ModsDir 'mods.txt') $name
}

if ($Only -in @('all','toolkit')) {
    Deploy-LuaMod 'CRToolkit' (Join-Path $CR_ProjectRoot 'lua\CRToolkit')
}

if ($Only -in @('all','coop')) {
    $coopSrc = Join-Path $CR_ProjectRoot 'lua\CyberRatsCoop'
    if (Test-Path -LiteralPath (Join-Path $coopSrc 'Scripts\main.lua')) {
        Deploy-LuaMod 'CyberRatsCoop' $coopSrc

        # Built C++ DLL (optional until M1 is compiled)
        $dll = Join-Path $CR_ProjectRoot 'mod\CyberRatsCoop\dlls\main.dll'
        if (Test-Path -LiteralPath $dll) {
            $dstDlls = Join-Path $CR_ModsDir 'CyberRatsCoop\dlls'
            New-Item -ItemType Directory -Force -Path $dstDlls | Out-Null
            Copy-Item -LiteralPath $dll -Destination $dstDlls -Force
            CR-Ok "CyberRatsCoop\dlls\main.dll"
        } else {
            CR-Warn "Noch keine kompilierte main.dll (mod\CyberRatsCoop\dlls\main.dll) — nur Lua-Anteil deployed."
        }

        # Runtime config next to the mod
        $cfgSrc = Join-Path $CR_ProjectRoot 'config\coop.ini'
        if (Test-Path -LiteralPath $cfgSrc) {
            Copy-Item -LiteralPath $cfgSrc -Destination (Join-Path $CR_ModsDir 'CyberRatsCoop') -Force
            CR-Ok "coop.ini"
        }
    } else {
        CR-Info "CyberRatsCoop noch nicht implementiert — uebersprungen."
    }
}

# Steam P2P needs steam_api64.dll + steam_appid.txt next to the shipping exe (M7).
$steamDll = Join-Path $CR_ProjectRoot 'third_party\steamworks_sdk\redistributable_bin\win64\steam_api64.dll'
if (Test-Path -LiteralPath $steamDll) {
    Copy-Item -LiteralPath $steamDll -Destination $CR_Win64 -Force
    Set-Content -LiteralPath (Join-Path $CR_Win64 'steam_appid.txt') -Value $CR_AppId -Encoding ascii
    CR-Ok "steam_api64.dll + steam_appid.txt"
}

CR-Ok "Deploy fertig. Spiel starten und in der UE4SS GUI-Console pruefen, dass die Mods geladen sind."
