# Cyber Rats Co-op — 2-instance test plan (M4/M5/M6)

This is the procedure to validate the co-op milestones with **two game instances on one machine**.
It needs the foreground (both windows + input), so run it when you're not gaming on this PC.
M1 (handshake) and M2 (puppet rats) are already verified; this focuses on M4 (cheese), M5 (enemies),
M6 (death).

## 0. Prerequisites

- Mod deployed: `…/Cyber Rats/Engine/Binaries/Win64/ue4ss/Mods/CyberRatsCoop/dlls/main.dll`
  (latest = M6 build, `0.6.0`). Re-run the build/deploy if unsure (xmake from `third_party/RE-UE4SS`,
  then copy the dll — see `scripts/build_mod.ps1`).
- **Force a fixed maze seed so both instances build the identical maze** (de-risks ID matching).
  In `config/coop.ini` set `[maze] force_seed = 1337` (any fixed value). Redeploy `coop.ini`.
- Both instances share the **same save** (`Save1`). If the rats in the save are **dead**, you must
  click **NEW RAT** on the loadout screen first (dead rat blocks START).

## 1. Launch two instances

**Instance 1 — HOST** (via Steam, role from coop.ini or arg):
```
steam://rungameid/3565080      # ensure coop.ini role=host, OR add -CRCoopRole=host via Steam launch options
```
**Instance 2 — CLIENT** (via the bootstrap exe so it inherits Steam context):
```powershell
$env:SteamAppId='3565080'; $env:SteamGameId='3565080'
& 'F:\Launcher\SteamLibrary\steamapps\common\Cyber Rats\Cyber Rats.exe' `
    -CRCoopRole=client -CRCoopConnect=127.0.0.1
```
Both share one `…/Win64/ue4ss/UE4SS.log`. Confirm the handshake:
```
[CRCoop] hosting on UDP 7777
[CRCoop] connecting to 127.0.0.1:7777
[CRCoop] peer Hello ok; co-op link established
```

## 2. Both into the SAME maze

In each instance: CONTINUE → (skip Intro) → Press Start → main menu **START GAME** →
rat-select → loadout → **START** (or it auto-starts if CRToolkit `CONFIG.autoStart=true`).
If a rat is dead → **NEW RAT** first. Expect in the log (each instance):
```
[CRCoop] forced maze seed = 1337
[CRCoop] cheese registry: N pickups
```
✅ **M3 check:** both mazes look identical (same corridors/rooms). The seed is forced + shared.

## 3. M4 — cheese / objective

- Walk one rat into a cheese (or near it + Interact). Expect on the collector:
  `[CRCoop] cheese <id> collected locally -> sync`; on the peer: `[CRCoop] replayed peer cheese <id>`.
- ✅ The **same** cheese disappears in **both** games; both cheese counters advance together.
- Collect all cheese (split between players) → the **exit (`BP_EnterExit`) opens in both** games.
- ❓ Edge to watch: two players grabbing the *same* cheese within ~1 frame (dedup should make each
  count it once; no double-count, no ghost cheese).

## 4. M5 — host-authoritative enemies

(enemies appear after the ~40 s cyborg delay / when you reach DAVE etc.)
- ✅ On the **client**, enemies are **host-driven puppets** — they move identically to the host's
  enemies (snapped from `EnemyState`). The client's own AI enemies are hidden.
- Watch the host log for `EnemySpawn`/`EnemyDespawn` traffic (enable verbose if needed).
- ❗ **Known risk:** with all client enemies neutralized (collision off), the **client rat may be
  unkillable**. That's expected for this scaffold — the fix is host-authoritative damage (next pass).
  Note whether enemy *visuals* track well (position/rotation); jitter is OK to note for tuning.

## 5. M6 — death

- Let the **host** rat die to a host enemy. Expect host log `[CRCoop] local rat died -> broadcast
  death`, client log `[CRCoop] peer rat 0 died`.
- (Client-rat death needs host-auth damage — see M5 risk; may not trigger yet.)

## 6. What to capture for the next pass

- Does cheese sync stay consistent for a full run (start → all cheese → exit)? Any desync?
- Do client enemy puppets look right, or do they fight their own AI (jitter/teleport)?
- Confirm the death message path both directions.
- Decide host-auth-damage design (so the client rat can die / downed-revive works).

## Log greps

```powershell
$log="F:\Launcher\SteamLibrary\steamapps\common\Cyber Rats\Engine\Binaries\Win64\ue4ss\UE4SS.log"
Get-Content $log | Select-String 'CRCoop'      # all co-op events
```
