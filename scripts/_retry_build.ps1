# Delayed, serialized UE4SS + mod build retry (run in background).
# Waits for GitHub rate-limit/network to settle, installs packages serially, builds all targets so
# UE4SS's custom raw_pdb resolves, then builds + deploys the mod.
$ErrorActionPreference = "Continue"
Start-Sleep -Seconds 1200

$env:PATH = "$env:USERPROFILE\.cargo\bin;$env:USERPROFILE\scoop\shims;$env:PATH"
Get-Process -Name xmake,ninja -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

# Clear failed package-install markers so xmake retries cleanly (guarded to the xmake cache).
$pkgCache = Join-Path $env:LOCALAPPDATA '.xmake\cache\packages'
if (Test-Path -LiteralPath $pkgCache) {
    Get-ChildItem -LiteralPath $pkgCache -Recurse -Directory -Filter 'installdir.failed' -ErrorAction SilentlyContinue |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
}

# Authenticate git to GitHub (raises rate limits) if gh is logged in.
try { $tok = (& gh auth token) 2>$null; if ($tok) { git config --global url."https://x-access-token:$tok@github.com/".insteadOf "https://github.com/" } } catch {}

Set-Location "F:\Projects\Mods\CyberRats\third_party\RE-UE4SS"
Write-Output "=== configure ==="
& xmake f -p windows -a x64 -m "Game__Shipping__Win64" -y 2>&1 | Select-Object -Last 8
Write-Output "=== serial package install (avoids imgui parallel lock) ==="
& xmake require -y 2>&1 | Select-Object -Last 25
Write-Output "=== build all default targets (resolves custom raw_pdb via deps/third-repo) ==="
& xmake -y 2>&1 | Select-Object -Last 30
Write-Output "=== build mod target ==="
& xmake build -y CyberRatsCoop 2>&1 | Select-Object -Last 30
Write-Output "BUILD_EXIT=$LASTEXITCODE"
$dll = Get-ChildItem -Recurse -Filter "CyberRatsCoop.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($dll) {
    Write-Output "DLL_OK: $($dll.FullName)"
    # Deploy into the game
    $dstDir = "F:\Launcher\SteamLibrary\steamapps\common\Cyber Rats\Engine\Binaries\Win64\ue4ss\Mods\CyberRatsCoop\dlls"
    New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
    Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $dstDir 'main.dll') -Force
    Copy-Item -LiteralPath "F:\Projects\Mods\CyberRats\config\coop.ini" -Destination (Split-Path $dstDir) -Force
    Write-Output "DEPLOYED to $dstDir\main.dll"
} else { Write-Output "NO_DLL" }
