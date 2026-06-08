# Remaining work — one plan per session (each run as its own dynamic workflow)

State after the foundation session: **M0/M1/M2 done & verified in-game; M3 seed-sync mechanism done.**
The C++ mod is `mod/CyberRatsCoop/dllmain.cpp` (monolithic `CyberRatsCoopMod : CppUserModBase`).
Build+deploy: `scripts/build_mod.ps1` (~6 s incremental). 2-instance test: host via Steam, client via
bootstrap `"Cyber Rats.exe" -CRCoopRole=client -CRCoopConnect=127.0.0.1`. Hooks/props: `docs/hooks.md`.
Wire format: `docs/protocol.md` (all message types already defined in `Net/Protocol.h`). Verified
UE4SS C++ API patterns are in `dllmain.cpp` (FindFirstOf, StaticFindObject<UClass*>, Cast, IsA,
GetValuePtrByPropertyName(InChain), K2_Get/SetActor*, GetFunctionByNameInChain+ProcessEvent,
RegisterProcessEventPreCallback, TArray Num/GetData). **Threading rule:** UObject work only on the game
thread (inside the ProcessEvent hook); sockets/handshake in `on_update`; share via the mutex.

**Module convention** (so each session's output integrates cleanly): each session delivers a
self-contained pair `Game/<Name>.{hpp,cpp}` exposing a small struct with:
`on_unreal_init()`, `on_process_event(UObject* ctx, UFunction* fn)` (game thread),
`on_net_tick()` (sends), `on_message(const crc::Frame&)` (inbound) — taking a `CoopCtx&` (transport,
role, m_peerReady, the shared mutex+snapshots, helpers for local pawn/world). Plus a markdown
runbook `docs/sessions/<id>.md` and the exact `dllmain.cpp` wiring snippet. Build+game-test is the
serial integration step after each workflow.

---

## S1 — M3 full validation (shared maze, real run)
**Goal:** prove both peers build the *identical* maze from the shared seed in a REAL run.
- Drive a proper run instead of `open Maze_LVL`: find + call the rat-select/"start game" path
  (inspect `Menu_Gamemode`, `BP_SelectLabRat`, `GameMode_LabRats`) so `Random Seed Roll` is actually
  rolled+consumed. Gate the client's `Maze_Generator:ReceiveBeginPlay` until the host's `MazeSeed`
  arrived (park/defer generation), then inject it.
- Robust fingerprint: enumerate the *actual* spawned room actors (the `MazeRunner_Biome_*` /
  `Master_Maze_Room` instances) by iterating `UObjectGlobals::ForEachUObject` and filtering by
  outer==`Maze_LVL` + class-name heuristics, XOR-hash their world positions. Compare host vs client.
- Deliverable: validated shared maze + seed-gating; updated `MazeModule`.

## S2 — M4 pickups & objective (host-authoritative)
**Goal:** synced cheese + maze exit/run lifecycle.
- Hooks (`docs/hooks.md`): generator `Spawn All Cheese` (host owns the set), `Chese_Pickup_C`/
  `Last_Chese_Pickup_C` via `Interact_Interface` collect, `BP_EnterExit_C` overlap, generator
  `Check for Dungeon Complete`, cheese count on `GameInstance_LabRats_C`/`GameMode_LabRats_C`.
- Net: `PickupCollected` (client req → host confirm), `PickupStateSync` (1 Hz keyframe),
  `ObjectiveReached`, `RunStart/RunEnd/Restart`. Stable pickup ids by maze cell (`docs/protocol.md`).
- Client suppresses local collection, sends request; host validates+broadcasts; both hide/destroy.
- Deliverable: `Game/Pickups.{hpp,cpp}` + runbook.

## S3 — M5 host-authoritative enemies
**Goal:** cyborgs/bosses identical and synced, host owns AI/damage.
- Suppress client spawns: pre-hook the generator's `Spawn Cyborgs At Location`/`Spawn DAVE`/
  `Spawn Critters`/`Spawn LabRat Ai` and the `BP_*Cyborg_Spawner_v2` spawn fns (client returns early).
- Host spawns + assigns monotonic `enemyId`, streams `EnemySpawn`/`EnemyState`(15-20 Hz Δ+1 Hz key)/
  `EnemyDespawn`; client creates AI-disabled enemy puppets, dead-reckons them. `EnemyHit` request→host.
- Deliverable: `Game/Enemies.{hpp,cpp}` + runbook. (Bosses DAVE/Krueger may need special-casing.)

## S4 — M6 death / downed / revive (co-op mechanic, mod-added)
**Goal:** death becomes a downed state revivable by the partner.
- Intercept `BP_LabRat_C:Kill Rat` / `ReceiveAnyDamage` → enter "downed" instead of dying (disable
  movement, play downed visual). Proximity revive: partner overlap+interact → host validates → both
  play `LabRat_Revive_A` and restore control. Both downed / timer → `RunEnd(loss)` → `Restart`.
- Net: `Death`, `DownedState`, `ReviveStart`, `ReviveComplete`.
- Deliverable: `Game/DeathRevive.{hpp,cpp}` + runbook.

## S5 — M7 Steam P2P + animation sync + polish
**Goal:** friend invites + smooth, shippable co-op.
- `Steam/SteamSocketsTransport` behind `INetTransport`: load `steam_api64.dll`+`steam_appid.txt`,
  `SteamAPI_Init`, lobby (FriendsOnly, size 2), overlay invite + `GameLobbyJoinRequested_t` +
  `GetLaunchCommandLine` cold-start join. Config `transport=steam`.
- Animation/montage sync in `PlayerState` (sprint flag, montage id table: Bite/Revive/Exit,
  velocity-driven locomotion). Interp ~100 ms + extrapolate ≤250 ms for puppets/enemies.
- Packaging: `scripts/package.ps1` (zip the mod for drop-in install) + a player-facing README.
- Deliverable: `Steam/`+`Game/AnimSync.*` + packaging + runbook.
