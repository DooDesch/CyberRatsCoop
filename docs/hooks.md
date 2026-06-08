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

## Class-function dump (2026-06-08) — M4/M5 targets

Dumped target class function lists in-maze (CRToolkit `dumpClassByPath`). Confirmed:
- **Cheese collect = `Interact`**: `Interact_Interface_C` has exactly one method, **`fn Interact`**. So the
  pickup-collect hook is **`Chese_Pickup_C:Interact`** (and `Last_Chese_Pickup_C:Interact`). ✅
- **`Chese_Pickup_C` / `Last_Chese_Pickup_C`: NOT loaded at difficulty-0** (`open Maze_LVL` spawns 0 cheese).
  They load only in a **real run** (rat-select → start). Resolve/cache the class lazily when it loads;
  dump its full functions in a real run to confirm `Interact` + the count-mutation path.
- **`BP_EnterExit_C`** ✅: fns `ReceiveBeginPlay`, `ExecuteUbergraph_BP_EnterExit`; prop **`Open?` (bool)** —
  the exit-open/objective state. No standalone overlap fn (logic is in the ubergraph / via the rat's Interact).
- **Cyborg spawners** ✅ (loaded): `BP_Cyborg_Spawner_C:Spawn Cyborg Spawner` (+ prop `Cyborgs Spawner To Spawn`
  ClassProperty, `As Maze Generator`); the v2 spawners (`BP_GatlingCyborg_Spawner_v2_C` etc.) have
  `ReceiveBeginPlay`, **`Spawn Trap`** (the actual emit), `Setup Rat Info` (gatling), props `Selected Trap`
  (ClassProperty), `Trap to Spawn` (Array), `Spawn Delay`, `Original Location`. → M5: suppress `Spawn Cyborg
  Spawner` / `Spawn Trap` on client; host runs them + replicates.
- **`GameInstance_LabRats_C` run-lifecycle fns** ✅: `Check if Maze is Completed or Not`, `Maze Complete -
  Update and Save all to slot`, `Reset Maze`, `Reset Game Struct`, `Randomize LabRat 1/2/3` (rat-select),
  `Load Game Event`. Props: `Cheese Number Array`, `Token Number Array`, `Revive Number Array`,
  `Selected LabRat ID`, `Random Seed Roll`, `First Game Ever?`.

## Real-run capture (2026-06-08) — enemy/NPC classes + menu START fn ✅

Drove a **real run** (rat-select → loadout → START) with CRToolkit's per-spawn auto-capture.
Full function/property dumps of each are in `extracted/crtoolkit.log` (search `PER-SPAWN ENEMY:`).

**Enemy / NPC pawn classes** (M5 host-authoritative targets) — all are Characters/Pawns:
- `BP_DAVE_C` — the DAVE boss.
- `BP_Canister_Rat_C`, `BP_SmokeBomb_Rat_C`, `BP_SpiderRat_C` — cyborg-rat variants (the actual enemy pawns the spawners emit).
- `BP_Rat_Critter_C` — critter.
- `BP_TeamRat_C` — companion team rat (ally; spawned by generator `Spawn LabRat Ai`).
- (`SpectatorPawn` also appears at menu levels — ignore.)
→ M5: on client, suppress these (don't run their AI) and spawn transform-driven puppets; host replicates `EnemyState`.

**Menu START (programmatic run-start)** ✅: the loadout screen widget is **`Rat_Selected_Screen_C`**
(`/Game/Menus/UI/Rat_Selected_Screen.Rat_Selected_Screen_C`). Its START button calls the no-param
function **`Press Start`** (siblings: `Start Hover ON/OFF`, `Stop Start Button Animation`, `Maze Level`).
→ Call `widget["Press Start"](widget)` to start a run without the (unreliable) Slate mouse-click.
CRToolkit `CONFIG.autoStart` does this on the loadout screen. (Menu flow: CONTINUE → Intro → Press
Start → main menu `START GAME`/`NEW RUN` → `Rat_Select_Screen_C` → `Rat_Selected_Screen_C` loadout.)

### ⭐ Cheese pickup = `BP_Cheese_Pickup_C` ✅ (RESOLVED — the M4 collect target)

`Chese_Pickup_C`/`Last_Chese_Pickup_C` were a **red herring** (unused asset, 0 instances ever). The
real cheese is **`BP_Cheese_Pickup_C`** = `/Game/Interactions/Cheese_Pickup/BP_Cheese_Pickup.BP_Cheese_Pickup_C`
(captured live: 10 instances in a real run). Other pickups: **`BP_Revive_Coin_Pickup_C`**,
**`Toy_Collector_BP_C`** (toy), tokens = `BP_GreenToken_Pickup_C`.

`BP_Cheese_Pickup_C` essentials (full dump in `extracted/crtoolkit.log`):
| Need | Target | Notes |
|---|---|---|
| **Collect** | **`Interact ()`** — NO params | the collect action; safe to call via ProcessEvent(fn,nullptr) |
| Collect trigger | `Overlap : SphereComponent` + `BndEvt__...ComponentBeginOverlapSignature` | rat enters sphere → interact |
| Remove effect | **`Destroy Cheese ()`** | destroys actor + FX (called inside Interact path) |
| Total in maze | prop **`Cheese Amount : Int = 10`** | each cheese knows the maze total |
| Refs | `As Game Instance Lab Rats`, `As BP Lab Rat`, `As Exit Zone` | |
| Sensory/visual (local) | `X-Ray ON`, `Line of Sight`, `Smelling Particles`, `Floating Movements` | local-only, do NOT replicate |

**M4 design (confirmed feasible):** deterministic `pickupId` = index of each cheese in the
position-sorted `FindAllOf(BP_Cheese_Pickup_C)` list (identical on both peers via seed-sync). Hook
`BP_Cheese_Pickup_C:Interact` (ProcessEvent pre-cb) → on local collect, broadcast `PickupCollected{id}`.
On receive → call that cheese's `Interact()` (no params) to faithfully replay (count + destroy +
`Check for Dungeon Complete` → `BP_EnterExit.Open?`). Dedup by `pickupId`; re-entrancy flag so the
replay's own Interact doesn't re-broadcast. Exit opens on both sides automatically once all collected.

## Confirmed infra facts
- UE4SS injects on UE 5.6 via the custom `StaticConstructObject` AOB (see `third_party/ue4ss/overrides/`).
- Working in this build: `NotifyOnNewObject`, `ExecuteWithDelay`, `FindFirstOf/FindAllOf`, reflection (`ForEachProperty/ForEachFunction`), `ExecuteConsoleCommand` (`open Maze_LVL`).
- NOT firing in this build: `RegisterLoadMapPostCallback`, `RegisterKeyBind` (synthetic input). Drive via `NotifyOnNewObject` + `ExecuteWithDelay` instead.
- `bReplicates=false` on actors; `NetDriverName=GameNetDriver` exists but inert → external overlay confirmed.
