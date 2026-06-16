# Cyber Rats Co-op

> 🛟 **Need help or found a bug?** Get support at [support.doodesch.de](https://support.doodesch.de).


A 2-player shared-maze **co-op multiplayer mod** for **Cyber Rats** (Outpost Games, Steam App
`3565080`), an Unreal Engine 5.6 single-player roguelite. The game ships with no networking
(`OnlineSubsystemNull`, no NetDriver) and no anti-cheat, so this mod adds co-op as an **external
overlay** via [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS): every player runs the game locally,
one is the host/authority, and we synchronize state over our own transport (Steam P2P in
production, ENet/LAN for development), drawing the remote player as a "puppet" rat.

> Status: **M0 — Fundament & Introspektion** (bootstrap). See `docs/` and the milestone plan.

## Layout

| Path | Purpose |
|---|---|
| `docs/` | `recon.md` (verified game facts), `hooks.md` (UFunction targets — filled in M0), `protocol.md` (wire format) |
| `lua/CRToolkit/` | M0 UE4SS **Lua** introspection + maze-determinism test mod |
| `lua/CyberRatsCoop/` | Lua prototyping layer for the co-op mod (policy/hooks, hot-reloadable) |
| `mod/CyberRatsCoop/` | The native **C++** UE4SS mod (transport, puppet sync, Steam) |
| `third_party/` | UE4SS build, Steamworks SDK, ENet (fetched by `scripts/setup.ps1`) |
| `mappings/` | `Mappings.usmap` (generated at runtime; needed for readable UE5.6 property names) |
| `extracted/` | FModel/CUE4Parse JSON exports of target Blueprints |
| `config/coop.ini` | Runtime config (transport, send rates, dev host id/ip) |
| `scripts/` | `setup.ps1`, `deploy.ps1`, `run_two_instances.ps1` |

## Quick start (development)

```powershell
# 1. Fetch UE4SS (experimental, UE5.6-capable) and place the proxy + settings into the game.
pwsh scripts/setup.ps1

# 2. Deploy the CRToolkit introspection mod (M0) into the game's Mods folder.
pwsh scripts/deploy.ps1

# 3. Launch the game (Development exe gives richer logging for RE).
#    In-game: open the UE4SS GUI console, then use the CRToolkit keybinds (F5-F8, F9 determinism).
```

The mod's install target is the **shipping exe folder**:
`F:\Launcher\SteamLibrary\steamapps\common\Cyber Rats\Engine\Binaries\Win64\`.

## Legal / scope

Personal & community co-op mod for an offline single-player game the user owns. No anti-cheat is
present or bypassed; no online service is defeated. The mod ships **no game assets** — it only
loads alongside the user's own installation.