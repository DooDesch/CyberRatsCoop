# Recon — Cyber Rats (verified facts)

Source of truth for everything we know about the game. Update as M0 introspection confirms more.

## Game / engine

| | |
|---|---|
| Title | Cyber Rats |
| Developer / Publisher | Outpost Games |
| Steam App ID | `3565080` (Demo: `3728970`) |
| Released | 2025-10-27 |
| Engine | Unreal Engine **5.6** (`++UE5+Release-5.6-CL-44394996`, `engineversion=5.6.1-44394996`) |
| Genre | Third-person procedural-maze survival roguelite |
| Multiplayer | **None.** Dev publicly stated they won't add MP; community requests co-op. |
| Anti-cheat | **None** (only standard engine plugins mounted). |
| Online subsystem | `OnlineSubsystemNull` only. Iris `NetActorFactory`/`NetSubObjectFactory` register then unregister; **no NetDriver / listen server ever created.** Level travel = `UEngine::Browse` (single-player). |

## Install paths

```
Game root:   F:\Launcher\SteamLibrary\steamapps\common\Cyber Rats\
Bootstrap:   Cyber Rats.exe                                  (171 KB packed wrapper -> relaunches engine exe)
Shipping:    Engine\Binaries\Win64\UnrealGame-Win64-Shipping.exe   (~143.7 MB)  <-- Steam launches this; UE4SS target
Development: Engine\Binaries\Win64\UnrealGame.exe                  (~302.6 MB)  <-- richer logging for early RE
Paks:        Cyber_Rats\Content\Paks\Cyber_Rats-Windows.{pak,ucas,utoc} + global.{ucas,utoc}
Config(in pak): Cyber_Rats\Config\Default{Engine,Game,Input}.ini
Save:        Cyber_Rats\Saved\SaveGames\Save1.sav  (GVAS)
Logs:        Cyber_Rats\Saved\Logs\Cyber_Rats.log
Manifest:    appmanifest_3565080.acf  (SharedDepots include 228980 = Steamworks Common Redistributables)
```

- **No `.pdb` on disk.** `UnrealGame.pdb` is listed in `Manifest_DebugFiles_Win64.txt` but Steam
  did not download it (verified: `Win64\` contains only the two exes + tbb DLLs). Do **not** depend
  on symbols. UE4SS reflection + Live View are the RE tools; if AOBs ever fail, the PDB can be
  fetched from the Steam depot to author overrides.
- **IoStore `SignatureHash=0`** → no AES key needed to extract paks.

## Screen / map flow

All transitions via `UEngine::Browse`:

```
Logo_LVL -> Intro_LVL -> Press_Start_LVL -> Rat_Select_LVL -> /Game/Procedural_Maze/Maze_LVL
```

`Maze_LVL` is the single gameplay map. On `LoadMap(Maze_LVL)`, after match state
`WaitingToStart -> InProgress`, `Maze_Generator_C_1` runs **synchronously on the game thread
(~96 ms)**, spawning all walls/rooms (`BeginDeferredActorSpawnFromClass` + `FinishSpawningActor`),
printing `COLORS HAS BEEN FIXED!`. → one clean hook point on the generate function, but it fires on
map BeginPlay, so maze hooks must be registered before the client travels to `Maze_LVL`.

## Key classes (cooked names are readable)

| Role | Class | Notes |
|---|---|---|
| GameInstance | `GameInstance_LabRats_C` | persists across maps; holds save/run state, cheese count? |
| GameMode (run) | `GameMode_LabRats_C` | pawn spawn / win / death logic |
| GameMode (menu) | `Menu_Gamemode_C` | |
| PlayerController | `Player_Controller_CyberRats_C` | |
| Player pawn | `BP_LabRat_C` (Character) | `ABP_LabRat` animBP, `BS_LabRat` blendspace, `ELabratSprintStates` |
| Rat select | `BP_SelectLabRat`, `ABP_LabRat_Select` | variant selection |
| Dead rat | `BP_Dead_LabRat`, `Dead_LabRat_Info_Struct` | |
| Maze generator | `Maze_Generator_C` (instance `..._C_1`) | seed source TBD (M0) |
| Maze pieces | `Master_Maze_Room`, `Biomes`, `Wall_Styles`, `Light_Configurations`, `BP_New_EndWall_C` | |
| Exit / objective | `BP_EnterExit` | + `LabRat_Exit_Maze_A` anim |
| Cheese | `Chese_Pickup`, `Last_Chese_Pickup`, `Interact_Interface` | last cheese likely triggers exit |
| Enemies | `BP_Cyborg_Spawner`, `BP_GatlingCyborg_Spawner_v2`, `BP_PropellerCyborg_Spawner_v2`, `BP_SpiderCyborg_Spawner_v2`, `Cyborg_Activated` | |
| Bosses | `DAVE` (washing-machine robot, voice/music), `Krueger_Claw_Tentacle` | |
| Allies / critters | `BP_TeamRat` (+`_Spawner`), `BP_Rat_Critter` | |
| Traps | `BP_TrapSpawner_OnBegin` | death-by: firetrap, spikes, poison bomb, propellerhead |
| Revive | `LabRat_Revive_A` (anim) | → co-op downed/revive mechanic |
| Save | `Save_Game_Object`, `GameInstance_Info_Struct`, `Enum_Current_Input` | |
| Player actions | Bite montages `LabRat_Bite_A/B/C(_Montage)` | |

## Implications for the mod

1. **No UE replication path** — co-op is a fully external overlay (our transport + puppet pawns).
2. **One shared map + standard Character pawn** → transform/anim sync is straightforward.
3. **Procedural maze** is the core sync challenge → share the seed (primary) with a host-streamed
   layout fallback (auto-selected by layout-hash compare).
4. **Revive animation already exists** → natural co-op downed/revive.
5. **UE4SS + Steamworks target the shipping exe** (`Engine\Binaries\Win64\`), not the root wrapper.
