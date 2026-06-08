# Hooks — UFunction targets (M0 results, verified in-game)

Captured live via `CRToolkit` (UE4SS) by auto-jumping into `Maze_LVL` and dumping the real
objects. ✅ = confirmed in-game (UE4SS reflection). Blueprint function paths use display names with
spaces — in code, target them as `/Game/<Path>.<Class>_C:<Function Name>` (UE4SS accepts the
spaced name).

## ⭐ Determinism verdict (R-3) → SEED-SYNC

`Maze_Generator_C` owns a **`Stream : FRandomStream`** (`/Script/CoreUObject.RandomStream`) and
**all** world generation/spawning is centralized in that single actor. The master seed lives in
**`GameInstance_LabRats_C.Random Seed Roll : Int`**. → **Maze generation is deterministic from one
seed.** Strategy: host shares `Random Seed Roll` (+ run difficulty/level + `Selected LabRat ID`)
before generation; client writes the same and generates an identical world (maze + cheese + enemies
+ traps). The layout-stream fallback is retained but very likely unnecessary. Final empirical
validation happens in M3 (hook the generate entry, force the seed twice, compare room layouts).

## Maze generation — `Maze_Generator_C` ✅
Path: `/Game/Procedural_Maze/Maze_Generator.Maze_Generator_C` (instance `..._C_1` in `Maze_LVL`).

| Need | Target | Notes |
|---|---|---|
| Seed (FRandomStream) | property **`Stream`** (FRandomStream) | seed via `Stream.InitialSeed`; force before generation |
| Master seed (source) | `GameInstance_LabRats_C.Random Seed Roll : Int` | the value to sync host→client |
| Generation entry | **`ReceiveBeginPlay`** (+ `ExecuteUbergraph_Maze_Generator`) | gen kicks off on BeginPlay (~96 ms, game thread) |
| Cheese spawn | **`Spawn All Cheese`** | host-authoritative cheese; gives count/positions |
| Room spawn | `Spawn Start Room`, `Spawn Next Room`, `Move and rotate room in place`, `Close Holes`, `Check for Overlap` | maze structure |
| Enemy spawn | `Spawn Cyborgs At Location`, `Spawn Cyborg from LabRat BP`, `Spawn DAVE`, `Spawn Critters`, `Spawn LabRat Ai`, `Spawn Scary Cat` | host-authoritative enemies |
| Trap spawn | `Spawn Floor Traps At Location`, `Spawn Ceiling Stuff` | |
| Other spawns | `Spawn Tokens`, `Spawn Revive Coins`, `Spawn All Dead Rats`, `Spawn Toy at Location`, `Spawn Things on Spawn Points` | |
| Win check | **`Check for Dungeon Complete`** | run-complete condition |
| Difficulty setup | `Setup Dungeon Difficulty`, `Setup Room Amount`, `Setup Cyborg Amount`, `Setup Trap Amount pr Maze Level`, `Setup Critters Amount`, `Setup Token Amount pr Maze Level` | depend on level → must match across clients |
| Biome | `Change Biome`, `Check for New Biome`, `Reset Biome Counter`; prop `Biome Enum : Byte` | |
| Counts (read) | `Cheese Amount`, `Cyborg Amount`, `Traps Amount`, `Critters Amount`, `Spawned Room Amount`, `Token Amount in Maze`, `Revive Amount in Maze`, `DAVE Amount`, `Ceiling Amount` | all Int |
| Room lists | `Final Room List`, `Door List`, `Traps List`, `Cyborg Spawn Point List`, per-biome `*_Rooms` arrays | for fingerprint / layout fallback |
| GI ref | `As Game Instance Lab Rats` (→ `GameInstance_LabRats_C`) | |

> Note: on a direct `open Maze_LVL` (no menu run-setup) difficulty=0 → 0 cheese/cyborgs spawn, but
> 21 rooms still generate. For full pickup/enemy introspection, drive a real run (rat-select) later.

## Player pawn — `BP_LabRat_C` ✅ (a Character)
Path: `/Game/Characters/Lab_Rat/BP_LabRat.BP_LabRat_C`.

| Need | Target | Notes |
|---|---|---|
| Transform/movement | Character base: `K2_GetActorLocation/Rotation`, `GetVelocity`, CharacterMovementComponent | per-tick outbound; apply to puppet via `K2_SetActorLocationAndRotation` |
| Is moving | `IsRatMoving` | |
| Sprint state | property **`SprintStates : Byte`** (ELabratSprintStates) + `HoldingSprintInput`, `SprintTick`, `Use Stamina`, `Gain Stamina` | sync sprint flag |
| Stamina | `Stamina Amount/Max/Regen`, `StaminaUsePerSecond` | |
| Bite attack | property **`Is Biting : Bool`**; fns `Bite Damage`, `Bite Force`; input `InpActEvt_IA_Bite_...` | broadcast bite event |
| Death | property **`Is Dead : Bool`**; fns **`Kill Rat`**, `Play Death Animation and Effects`, `Death Camera Event`, `Death Sound And Effects`, `Lose Head`, `ReceiveAnyDamage` | host-confirmed death |
| Neutralize puppet | **`Turn Off Rat`** | disable remote rat's own logic |
| Anim instance | property `As Anim BP Lab Rat` (→ `ABP_LabRat`); `Death Animation Sequence`; `Bite Anim List` | drive puppet anim/montages |
| Input (local intent) | `InpActEvt_IA_Move/Look/Sprint/Interact/Bite/PauseGame_...` (EnhancedInput: `IA_Move`,`IA_Look`,`IA_Sprint`,`IA_Interact`,`IA_Bite`) | hook to detect local actions |
| Interact | `InpActEvt_IA_Interact_...` (+ `Interact_Interface`) | cheese pickup / exit |
| Upgrades/status | `Setup Upgrades`, `Update Rat Current Rat Status`, `Update Rat Current Rat Status With Cheese`, `Temp *Amount` | |
| Camera | `Camera`, `CineCamera`, `SpringArm`/`CameraBoom` | local-only; do NOT replicate |

## GameInstance — `GameInstance_LabRats_C` ✅
Path: `/Game/SaveGame/GameInstance_LabRats.GameInstance_LabRats_C` (persists across maps).

| Need | Target | Notes |
|---|---|---|
| **Master maze seed** | **`Random Seed Roll : Int`** | sync host→client before generation |
| Selected rat variant | **`Selected LabRat ID : Int`** | for puppet appearance |
| Rat loadouts | `Current LabRat Info`, `LabRat 1/2/3 Info`, `LabRat Hardcore Info` (UserDefinedStructs) | |
| Cheese/tokens per level | `Cheese Number Array`, `Token Number Array` | |
| Dead-rat meta (roguelike) | `Dead LabRat List`, `Dead LabRat Info Struct` | |
| Team rats (companions) | `Team LabRat 1 Info`, `Team LabRat 2 Info` | |
| Save | `Save Slot Name = "Save1"`, `Save Game Data` (→ `Save_Game_Object`) | |

## Pickups / objective
`Chese_Pickup_C`, `Last_Chese_Pickup_C` (spawned by `Spawn All Cheese`), `Interact_Interface`,
`BP_EnterExit_C` (exit). 0 instances on difficulty-0 direct-open — revisit in a real run. Pickup
collection likely via the Interact input + interface; host-authoritative count on GameInstance.

## Enemies / spawners
Spawned by the generator (`Spawn Cyborgs At Location` etc.), not by standalone spawner actors at
generation time (`BP_*Cyborg_Spawner*` = 0 on direct-open; cyborgs have `Cyborg Spawn Delay = 40 s`).
→ Host authority: suppress on client, host runs the generator's spawn calls, replicate enemy state.
Enemy classes still to dump in a real run: cyborg pawns, `DAVE`, `Krueger_Claw_Tentacle`, `BP_TeamRat`, `BP_Rat_Critter`.

## Revive (co-op) — NOT in base game
`BP_LabRat_C` has `Is Dead`/`Kill Rat` but **no in-run revive** (single-player death = run end).
`LabRat_Revive_A` anim + generator `Spawn Revive Coins`/`Revive Coins` are roguelike meta-revive,
not co-op downed/revive. → **Co-op downed+revive is a mod-added mechanic** (intercept `Kill Rat` to
enter a "downed" state instead of dying; add proximity revive playing `LabRat_Revive_A`).

## Confirmed infra facts
- UE4SS injects on UE 5.6 via the custom `StaticConstructObject` AOB (see `third_party/ue4ss/overrides/`).
- Working in this build: `NotifyOnNewObject`, `ExecuteWithDelay`, `FindFirstOf/FindAllOf`, reflection (`ForEachProperty/ForEachFunction`), `ExecuteConsoleCommand` (`open Maze_LVL`).
- NOT firing in this build: `RegisterLoadMapPostCallback`, `RegisterKeyBind` (synthetic input). Drive via `NotifyOnNewObject` + `ExecuteWithDelay` instead.
- `bReplicates=false` on actors; `NetDriverName=GameNetDriver` exists but inert → external overlay confirmed.
