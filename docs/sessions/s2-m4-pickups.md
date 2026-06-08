# S2 / M4 — Pickups & Objective (host-authoritative) — Integration + Test Runbook

Delivers `mod/CyberRatsCoop/Game/Pickups.{hpp,cpp}` — a self-contained `crc::game::PickupsModule`
that syncs cheese pickups and the maze-exit / run-lifecycle objective between the two peers, with the
**host as sole authority**. No edits to `dllmain.cpp` or `Net/Protocol.h`; the module owns its own M4
wire-message PODs (byte-compatible with `docs/protocol.md`) and (de)serializes them with the existing
`crc::ByteWriter`/`crc::ByteReader`.

Status: **builds clean** (`scripts/build_mod.ps1`, `compiling ... Game\Pickups.cpp` → `build ok`).
`Game/Pickups.cpp` is already listed in `mod/CyberRatsCoop/xmake.lua` `add_files(...)`. Functional
2-instance verification is the serial integration step below.

---

## 0. Authority model (recap)

- **Host** owns the canonical pickup set + run lifecycle. It decides when a cheese is collected and
  when the run ends, and broadcasts those facts.
- **Both peers** locally generate the identical cheese set (deterministic from the M3 shared seed, via
  the generator's `Spawn All Cheese`). We do **not** suppress spawning; we **bind** each local actor to
  a stable cell-derived id, then keep the two sets in agreement by hiding/destroying on command.
- **Collection = request → confirm.** Client suppresses its own collection (collision disabled on its
  cheese), detects pickup intent by proximity, sends `PickupCollected` (request); host validates +
  finalizes + broadcasts the confirm as a `PickupStateSync` delta. Both then hide/destroy that cheese.
- **Objective / run lifecycle = host-only emit.** Host hooks the generator's `Check for Dungeon
  Complete` and the exit overlap, and emits `ObjectiveReached` / `RunEnd`. `RunStart` is host-originated
  from the existing gen-begin path.

Stable cross-machine pickup ids are derived from the **maze grid cell** (`docs/protocol.md`:
`id = hash16(cellX, cellY, slot)`), never from pointer/FName — both peers build the identical maze
(M3), so a cheese in a given cell hashes to the same id on both. See `crc::game::hash16` in the header.

---

## 1. EXACT `dllmain.cpp` wiring (additive only — no existing logic changes)

All snippets are additions. Line anchors refer to the current `dllmain.cpp`.

### 1a. Include (top, with the other module/project includes, near line 24-26)

```cpp
#include "Game/Pickups.hpp"
```

### 1b. Member (in the private member block, near the other game-thread members ~line 343)

```cpp
crc::game::PickupsModule m_pickups;
```

### 1c. Bind + init — inside `on_unreal_init()`, AFTER the `RegisterProcessEventPreCallback` block
(i.e. after the existing `Output::send(... "ProcessEvent hook registered" ...)`, ~line 120):

```cpp
// M4 pickups & objective. Shares dllmain's transport/role/mutex; never edits player-sync state.
// getLocalPawn hands the module the controller-possessed local rat the player-sync code already
// tracks (m_localPawn), so the client's proximity-collect uses the same pawn.
m_pickups.bind(m_transport.get(), m_role, m_playerId, &m_peerReady, &m_mtx,
               [this]() -> U::AActor* { return m_localPawn; });
m_pickups.on_unreal_init();
```

> Note: `m_pickups` shares dllmain's **existing** `m_mtx`. The module only ever copies small PODs
> (ids/counts) in/out under that lock and does **no** UObject work inside it, so it cannot deadlock or
> race with the player-sync snapshots (which hold the same lock equally briefly).

### 1d. Game-thread hook — at the END of `onProcessEvent(ctx, fn, parms)` (after the player-sync work,
just before the closing brace ~line 237). It is safe to call unconditionally every event; the module
pointer-compares cached UFunctions internally and early-outs cheaply:

```cpp
m_pickups.on_process_event(ctx, fn);
```

### 1e. Run-begin notify — inside `onMazeGenBegin()`, at the very end (after the seed is chosen/forced
and the host's `MazeSeed` is sent, ~line 289). Pass the same seed value dllmain resolved:

```cpp
m_pickups.notify_run_begin((uint64_t)(uint32_t)seed);
```

> If `seed < 0` (not forced), still call it with the natural seed if you have one, or skip — the host
> will emit `RunStart` with whatever seed it passes; the client only logs it. Resetting per-run state
> is the important side effect and is harmless to call with any value.

### 1f. Net-thread send — inside `on_update()`, AFTER the PlayerState send block (~line 152, before the
method's closing brace):

```cpp
m_pickups.on_net_tick();
```

### 1g. Inbound dispatch — inside `dispatch(const crc::Frame& fr)`'s `switch`, add the M4 cases before
`default:` (~line 188):

```cpp
case Msg::PickupCollected:
case Msg::PickupStateSync:
case Msg::ObjectiveReached:
case Msg::RunStart:
case Msg::RunEnd:
case Msg::Restart:
    m_pickups.on_message(fr);
    break;
```

### 1h. xmake — ALREADY DONE
`mod/CyberRatsCoop/xmake.lua` already has:
```lua
add_files("dllmain.cpp", "Transport/UdpTransport.cpp", "Game/MazeModule.cpp", "Game/Pickups.cpp")
```
No change required.

### 1i. (Optional) coop.ini — no new keys are strictly required for M4. The module currently uses
file-local tuning constants (`kCell = 100 cm` quantization, `kPickupRadiusSq = 150 cm` collect radius,
`kExitObjectiveId = 1`). If in-run testing shows the maze cell pitch or collect radius needs tuning,
promote them to `[maze]` keys (`cell_size`, `pickup_radius`) and pass them through `bind(...)`.

---

## 2. Build + deploy

```pwsh
pwsh F:\Projects\Mods\CyberRats\scripts\build_mod.ps1 -Mode Game__Shipping__Win64
```

Incremental build is ~6-8 s. On success it prints:

```
[ ok ] Gebaut: ...\CyberRatsCoop.dll
[ ok ] Deployed -> ...\Cyber Rats\Engine\Binaries\Win64\ue4ss\Mods\CyberRatsCoop\dlls\main.dll (+ coop.ini, enabled in mods.txt)
... compiling.Game__Shipping__Win64 cppmods\CyberRatsCoop\Game\Pickups.cpp
... build ok
```

(Use `-Rebuild` to force a clean rebuild of the mod target if you change headers shared with dllmain.)

---

## 3. 2-instance in-game test

Per `docs/sessions.md`: host via Steam, client via the bootstrap exe.

1. **Host** — launch Cyber Rats normally through Steam (role defaults from `coop.ini`; set
   `[role] role = host` or pass `-CRCoopRole=host`). Drive a **real run** (rat-select / start-game
   path so `Random Seed Roll` is actually rolled — `open Maze_LVL` at difficulty 0 spawns **0 cheese**
   and M4 has nothing to bind; see `docs/hooks.md`).
2. **Client** — launch the bootstrap:
   ```
   "Cyber Rats.exe" -CRCoopRole=client -CRCoopConnect=127.0.0.1
   ```
3. Confirm the link first (M2): both logs show `co-op link established`. Then both reach the maze and
   M3 logs the matching `MAZE FINGERPRINT ... hash=0x........` on both — pickups only make sense once
   the maze is proven identical.

### Success criteria — UE4SS.log (`ue4ss/UE4SS.log`)

| Stage | HOST log line | CLIENT log line |
|---|---|---|
| Module armed | `[CRCoop] M4 pickups module ready (role=1)` | `[CRCoop] M4 pickups module ready (role=2)` |
| Run begin | `[CRCoop] M4: run begin (runId=N, seed=S)` | `[CRCoop] M4: RunStart runId=N ...` (received) |
| Cheese spawn seen | `[CRCoop] M4: Spawn All Cheese seen; deferring enumeration` | same |
| Bound to ids | `[CRCoop] M4: enumerated K cheese pickups (role=1)` | `[CRCoop] M4: enumerated K cheese pickups (role=2)` — **same K** |
| Host collects | `[CRCoop] M4: host collected cheese id=0x....` | `[CRCoop] M4: applied collected cheese id=0x....` (via StateSync) |
| Client collects | `[CRCoop] M4: host confirmed client collect id=0x....` | `[CRCoop] M4: client request collect id=0x....` then `applied collected ...` |
| Exit reached | `[CRCoop] M4: exit reached (byPlayer=..) -> ObjectiveReached` | `[CRCoop] M4: ObjectiveReached id=0x0001 ...` |
| Dungeon complete | `[CRCoop] M4: dungeon complete -> RunEnd(win) runId=N` | `[CRCoop] M4: RunEnd runId=N result=1 ...` |

### Success criteria — on-screen / behavioral

- **Identical cheese count `K` on both peers** at enumeration (the headline check that cell-hash ids
  bound the same set on both machines).
- When **either** player walks over a cheese, it disappears on **both** screens within ~1 round-trip,
  and the HUD cheese count increments on both (single increment, not double).
- The **host's** count is authoritative: induce packet loss / collect rapidly — the 1 Hz
  `PickupStateSync` keyframe re-hides any cheese a dropped confirm left visible (no permanently-stuck
  ghost cheese on the client).
- Reaching the maze exit / completing the dungeon ends the run on both (RunEnd logged on both).

---

## 4. Open items to verify in the real run (flagged by `docs/hooks.md`)

These are 0-instance on a difficulty-0 direct-open, so they are confirmable only in a driven run.
Verify and adjust in `Pickups.cpp` if the dumped names differ:

1. **Cheese / exit class paths** — the module resolves classes by full path
   (`/Game/Pickups/Chese_Pickup.Chese_Pickup_C`, `Last_Chese_Pickup_C`,
   `/Game/Pickups/BP_EnterExit.BP_EnterExit_C`) AND enumerates by short class name
   (`FindAllOf(STR("Chese_Pickup_C"))`, `STR("Last_Chese_Pickup_C")`). If the real package path differs
   (e.g. cheese lives under `/Game/Procedural_Maze/...`), update the `StaticFindObject<UClass*>` paths;
   the `FindAllOf` short-name enumeration is path-independent and should keep working regardless.
2. **Collect trigger** — `Pickups.cpp` resolves the cheese collect UFunction by probing candidate names
   (`On Collected`, `Collect`, `Collected`, `Pickup`, `On Pickup`, `Interact`, `On Interact`) on the
   cheese class. The **client's primary path is proximity** (`clientProximityScan`, collision disabled
   on its cheese → never collects locally → requests instead), so collection works even if the collect
   fn never resolves. If the real run shows collection is driven by `BP_LabRat:InpActEvt_IA_Interact_*`
   + `Interact_Interface` instead, add that as the resolved fn (prefix-match the per-instance suffix
   with `name.rfind(STR("InpActEvt_IA_Interact"), 0) == 0` under `ctx->IsA(m_labRatClass)`), and keep
   proximity as the fallback. Disable whichever path does not fire.
3. **Grid cell pitch / pickup radius** — confirm `kCell` (currently 100 cm quantization; only needs to
   be finer than the real inter-cheese spacing so distinct cheeses land in distinct cells) and the
   `kPickupRadiusSq` collect radius from the real maze. Promote to `coop.ini [maze]` if tuning is
   needed (see 1i).
4. **Cheese-count property** — `writeCheeseCountToGI()` writes `GameInstance_LabRats_C:"Cheese Amount"`.
   `docs/hooks.md` lists GI `Cheese Number Array` (per-level) and the generator's `Cheese Amount : Int`.
   Confirm which one the **HUD** actually reads and point the write there. The guarded write is a no-op
   if the property is absent, and both peers converge to the host total via the 1 Hz keyframe
   regardless, so a wrong guess degrades only the local HUD number, not sync.
5. **Double-count guard** — confirm the client's native local increment is suppressed (collision
   disabled means the game's own collect should not fire on the client). If the HUD double-counts on
   the client, the native path is still firing — neutralize it (disable the cheese's own collect logic
   on bind, analogous to dllmain's `Turn Off Rat` on the puppet).

---

## 5. Threading audit (must hold)

- `on_process_event` / `notify_run_begin` / `enumerateAndAssignIds` / `onCheeseCollectHook` /
  `onExitOverlap` / `onDungeonComplete` / `drainInboundToWorld` / `clientProximityScan` /
  `applyCollected` / `writeCheeseCountToGI` — **GAME THREAD only** (all UObject access lives here).
- `on_net_tick` / `on_message` — **LOOP THREAD only**: sockets + POD (de)serialization. They copy
  ids/PODs in/out of the shared queues under `*m_mtx` and **never** touch a `UObject*`.
- `UObject*` / `UClass*` / `UFunction*` / `AActor*` never cross the mutex — only `uint16_t` ids and
  small PODs do. The `m_idToActor` / `m_actorToId` maps are game-thread-only and never locked.
- Critical sections are minimal: copy-in/copy-out, then act with the lock released (e.g.
  `drainInboundToWorld` swaps `m_applyQueue` under lock, then mutates actors unlocked).
