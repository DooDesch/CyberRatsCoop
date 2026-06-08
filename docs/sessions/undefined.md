# S2 / M4 — Pickups & Objective (host-authoritative) — integration + test runbook

> The orchestrator passed the milestone name literally as `undefined`. Per `docs/sessions.md`, M0–M3
> are done & verified; the next roadmap item is **S2 — M4 pickups & objective (host-authoritative)**,
> which this session implemented. This file is the authoritative, self-contained runbook for that work.
> (`docs/sessions/s2.md`, `m4.md`, `m4-pickups.md`, `s2-m4-pickups.md` are earlier drafts of the same
> milestone; where they disagree on the `dllmain` wiring, THIS file is correct — see the WARNING in §1.4.)

Deliverables (this session):
- `mod/CyberRatsCoop/Game/Pickups.hpp` — `crc::game::PickupsModule` interface + inline M4 wire codecs
  (`PickupCollectedMsg 0x30`, `PickupStateSyncMsg 0x31`, `ObjectiveReachedMsg 0x40`, `RunStartMsg 0x70`,
  `RunEndMsg 0x71`, `RestartMsg 0x72`) + `hash16(cellX, cellY, slot)`. Does NOT redefine anything in
  `Net/Protocol.h`; uses `crc::ByteWriter`/`crc::ByteReader`. Byte layouts match `docs/protocol.md`.
- `mod/CyberRatsCoop/Game/Pickups.cpp` — the `PickupsModule` implementation.
- `mod/CyberRatsCoop/xmake.lua` — `Game/Pickups.cpp` added to `add_files(...)` (already applied).

`dllmain.cpp` and `Net/Protocol.h` are NOT edited by this session — the wiring below is the serial
integration step. Depends on M1 transport, M2 player presence, M3 shared-maze seed-sync (all verified).

---

## 0. Authority model (recap)

- **Host** owns the canonical pickup set + run lifecycle. It decides when a cheese is collected and when
  the run ends, and broadcasts those facts.
- **Both peers** locally generate the identical cheese set (deterministic from the M3 shared seed, via
  the generator's `Spawn All Cheese`). We do **not** suppress spawning; we **bind** each local actor to a
  stable cell-derived id, then keep the two sets in agreement by hiding on command.
- **Collection = request → confirm.** Client suppresses its own collection (`SetActorEnableCollision(false)`
  on every local cheese), detects pickup intent by **proximity**, sends `PickupCollected` (request); host
  validates + finalizes + broadcasts the confirm as a `PickupStateSync` delta. Both then hide the cheese.
- **Objective / run lifecycle = host emit.** Host hooks the generator's `Check for Dungeon Complete` and
  the exit overlap, and emits `ObjectiveReached` / `RunEnd`. `RunStart` is host-originated from the
  existing gen-begin path. `ObjectiveReached` is bidirectional (client proposes; host re-broadcasts).

Stable cross-machine pickup ids derive from the **maze grid cell** (`docs/protocol.md`:
`id = hash16(cellX, cellY, slot)`), never from pointer/FName. Both peers build the identical maze (M3),
so a cheese in a given cell hashes to the same id on both. See `crc::game::hash16` in the header. The Z
cell is folded into the Y term (unsigned, well-defined wrap) and `slot=1` for `Last_Chese_Pickup_C` else
0. Both peers sort pickups by quantized cell+slot before id assignment, so any collision perturbation
(`id += 0x9E37`, full-period odd-step walk) lands identically on both.

---

## 1. EXACT `dllmain.cpp` wiring (additive only — no existing logic changes)

Module public interface (from `Pickups.hpp`):
`bind(...)`, `on_unreal_init()`, `on_process_event(ctx, fn)` (game thread), `notify_run_begin(seed)`
(game thread), `on_net_tick()` (loop thread, sends), `on_message(const crc::Frame&)` (loop thread).
These slot into the EXISTING dllmain methods `on_unreal_init()`, `onProcessEvent(ctx, fn, parms)`,
`onMazeGenBegin()`, `on_update()`, and `dispatch(const crc::Frame&)`.

### 1.1 Include — top of `dllmain.cpp`, after `#include "Net/Protocol.h"` (~line 26)
```cpp
#include "Game/Pickups.hpp"
```

### 1.2 Member — in the `private:` block of `CyberRatsCoopMod`, near `m_localPawn`/`m_puppet` (~line 343)
```cpp
crc::game::PickupsModule m_pickups;
```

### 1.3 Bind + init — at the END of `on_unreal_init()`, AFTER `m_playerId` is set and AFTER the
`RegisterProcessEventPreCallback(...)` block (so transport/role/playerId are known). `m_peerReady` is a
plain `bool` member, so `&m_peerReady` is the required `bool*`; `m_transport` is a
`unique_ptr<UdpTransport>`, and `m_transport.get()` up-casts to `crc::INetTransport*`:
```cpp
// M4 pickups & objective. Shares dllmain's transport/role/playerId/peerReady and the SAME m_mtx.
// getLocalPawn hands the module the controller-possessed local rat that player-sync already tracks
// (m_localPawn) so the client's proximity-collect uses the same pawn. Lambda is invoked ONLY on the
// game thread (from m_pickups.on_process_event), so reading m_localPawn there is safe.
m_pickups.bind(m_transport.get(), m_role, m_playerId, &m_peerReady, &m_mtx,
               [this]() -> U::AActor* { return m_localPawn; });
m_pickups.on_unreal_init();
```

### 1.4 Game-thread forward — inside `onProcessEvent(ctx, fn, parms)`.
> ⚠️ **PLACEMENT IS LOAD-BEARING.** dllmain's `onProcessEvent` early-returns for every non-LabRat /
> non-local context (`if (fn == m_mazeGenBeginFn) {...; return;}` at ~L209, then `if (fn != m_ratTickFn)
> return;` at ~L210, then more). The pickups module needs to see `Spawn All Cheese`,
> `Check for Dungeon Complete`, the exit-overlap fn, and the cheese-collect fn — **none** of which are
> LabRat ticks. So the forward MUST go **before** those early-returns. Putting it at the end of the
> method (as some earlier drafts said) means the module only ever sees the local rat's `ReceiveTick` and
> silently never enumerates cheese or fires the objective — a complete, silent failure.

Place it immediately AFTER dllmain's own fn-caching block (the `if (!m_ratTickFn || !m_mazeGenBeginFn) {
... }` block, ~L207) and BEFORE the `if (fn == m_mazeGenBeginFn)` line (~L209):
```cpp
        m_pickups.on_process_event(ctx, fn);   // M4 (game thread); module gates internally by cached fn ptr
```
The module does its own cached-pointer gating, so the extra forwarded calls cost only a few pointer
compares once everything is resolved.

### 1.5 Run-begin notify — inside the EXISTING `onMazeGenBegin()` (the `Maze_Generator:ReceiveBeginPlay`
pre-hook, game thread), as the LAST line, AFTER the seed is chosen/forced and the host's `MazeSeed` is
sent (~L289). Pass the same `seed` dllmain resolved:
```cpp
        m_pickups.notify_run_begin((uint64_t)(uint32_t)seed);
```
Resets per-run pickup/objective state on both peers (drops the id↔actor maps so the next maze rebinds
cleanly); on the host it also queues a `RunStart`. `onMazeGenBegin()` returns early when `seed < 0`
(seed not forced) — if you want a per-run reset in that case too, move this call above that early return
and pass `0`; only the reset matters for the client, and the host's `RunStart.seed` is informational.

### 1.6 Net-thread send — inside `on_update()`, AFTER the PlayerState send block, before the closing brace
(~L152):
```cpp
        m_pickups.on_net_tick();
```

### 1.7 Inbound dispatch — inside `dispatch(const crc::Frame& fr)`'s `switch` (which already has
`using crc::Msg;` at its top), add the six M4 cases before `default:` (~L188):
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
> `dispatch` runs on the UE4SS loop thread; `on_message` touches only PODs under `m_mtx` — no UObject
> access there. World mutation happens later on the game thread in `drainInboundToWorld()`.

### 1.8 xmake — ALREADY DONE. `mod/CyberRatsCoop/xmake.lua` already lists the source (no glob):
```lua
    add_files("dllmain.cpp", "Transport/UdpTransport.cpp", "Game/MazeModule.cpp", "Game/Pickups.cpp")
```

---

## 2. Build + deploy
```pwsh
pwsh F:\Projects\Mods\CyberRats\scripts\build_mod.ps1
```
Incremental build ~6–8 s; deploys to `<game>/ue4ss/Mods/CyberRatsCoop/dlls/main.dll` (+ `coop.ini`,
enabled in `mods.txt`). Use `-Rebuild` if a newly added source file isn't picked up. (First-ever build
also compiles UE4SS + deps and is long.) If it fails on an unresolved
`PickupsModule::notify_run_begin`/`on_net_tick`/`on_message`, confirm `Game/Pickups.cpp` is in the
`add_files(...)` line above.

---

## 3. 2-instance in-game test

Per `docs/sessions.md`: host via Steam, client via the bootstrap exe.

1. **Host** — launch via Steam (set `[role] role = host` in `coop.ini` or pass `-CRCoopRole=host`).
   Drive a **REAL run** (rat-select / start-game) — `open Maze_LVL` at difficulty 0 spawns **0 cheese**
   (`docs/hooks.md`), so the module is inert (valid smoke test: it logs `enumerated 0 cheese pickups`).
2. **Client** — launch the bootstrap:
   ```
   "Cyber Rats.exe" -CRCoopRole=client -CRCoopConnect=127.0.0.1
   ```
3. Confirm the link first (M2): both logs show `co-op link established`; then both reach the maze and M3
   logs a matching `MAZE FINGERPRINT ... hash=0x........` on both — pickups only make sense once the
   maze is proven identical.

### Success criteria — `ue4ss/UE4SS.log`

| Stage | HOST | CLIENT |
|---|---|---|
| Module armed | `[CRCoop] M4 pickups module ready (role=1)` | `[CRCoop] M4 pickups module ready (role=2)` |
| Run begin | `[CRCoop] M4: run begin (runId=N, seed=S)` | `[CRCoop] M4: RunStart runId=N ...` (received) |
| Cheese spawn seen | `[CRCoop] M4: Spawn All Cheese seen; deferring enumeration` | same |
| Bound to ids | `[CRCoop] M4: enumerated K cheese pickups (role=1)` | `[CRCoop] M4: enumerated K cheese pickups (role=2)` — **same K** |
| Host collects | `[CRCoop] M4: host collected cheese id=0x....` | `[CRCoop] M4: applied collected cheese id=0x....` |
| Client collects | `[CRCoop] M4: host confirmed client collect id=0x....` | `client request collect id=0x....` then `applied collected ...` |
| Exit reached | `[CRCoop] M4: exit reached (byPlayer=..) -> ObjectiveReached` | `[CRCoop] M4: ObjectiveReached id=0x0001 ...` |
| Dungeon complete | `[CRCoop] M4: dungeon complete -> RunEnd(win) runId=N` | `[CRCoop] M4: RunEnd runId=N result=1 ...` |

### Success criteria — on-screen / behavioral
- **Identical cheese count `K` on both peers** at enumeration (headline check that cell-hash ids bound
  the same set on both machines).
- When **either** player walks over a cheese it disappears on **both** screens within ~1 round-trip, and
  the HUD count increments on both (single increment, not double).
- Host count is authoritative: collect rapidly / drop a confirm — the 1 Hz `PickupStateSync` keyframe
  re-hides any cheese a dropped confirm left visible (no permanently-stuck ghost cheese on the client).
- Reaching the exit / completing the dungeon ends the run on both (RunEnd logged on both, exactly once).

---

## 4. Open items — verify in the REAL run (0-instance on a difficulty-0 open)

Fix in `Pickups.cpp` if the dumped names differ. These are the only unverified constants; the flow is
fixed. Each is written to degrade gracefully, but confirm and tighten:

1. **Cheese / exit class PATHS** — `resolveClassesAndFns()` guesses
   `/Game/Pickups/Chese_Pickup.Chese_Pickup_C`, `/Game/Pickups/Last_Chese_Pickup.Last_Chese_Pickup_C`,
   `/Game/Pickups/BP_EnterExit.BP_EnterExit_C`. `docs/hooks.md` confirms the short class NAMES
   (`Chese_Pickup_C`, `Last_Chese_Pickup_C`, `BP_EnterExit_C`) but NOT the package paths. Enumeration is
   path-INDEPENDENT (it uses `FindAllOf(STR("Chese_Pickup_C"))` and captures the real `UClass*` from a
   live instance), so `enumerateAndAssignIds` works even if the paths are wrong. But the
   collect/exit/last-cheese `IsA(...)` discrimination needs the class objects — if `enumerated K` prints
   but no collect/exit lines appear, dump the real `/Game/...` paths
   (`obj->GetClassPrivate()->GetFullName()`) and correct the three `StaticFindObject<UClass*>` paths.

2. **Cheese collect UFunction name** — NOT in `docs/hooks.md` (0 cheese on a difficulty-0 open). The
   module probes a candidate list (`On Collected`, `Collect`, `Collected`, `Pickup`, `On Pickup`,
   `Interact`, `On Interact`) on any fn whose `ctx IsA m_cheeseClass`, bounded (gives up after 4096
   probes). The **client's primary path is proximity** (collision disabled → never self-collects →
   requests), so collection works even if the collect fn never resolves; the host's natural collect also
   works. If `docs/hooks.md` is right that collection runs through `Interact_Interface` /
   `BP_LabRat:InpActEvt_IA_Interact_*`, add that resolution (prefix-match the per-instance suffix:
   `name.rfind(STR("InpActEvt_IA_Interact"), 0) == 0` under `ctx->IsA(m_labRatClass)`), keep proximity
   as the fallback, and disable whichever path does not fire.

3. **Exit trigger** — the module hooks `BP_EnterExit_C:ReceiveActorBeginOverlap`. If the exit uses a
   different trigger, dump `BP_EnterExit_C` functions and update the `m_exitOverlapFn` resolution.

4. **Cell pitch `kCell` / collect radius `kPickupRadiusSq`** — `kCell = 100 cm` quantizes away
   cross-process FP noise (maze cells are far larger, so distinct cheeses land in distinct cells); the
   deterministic sort + odd-step nudge resolve any collision identically on both peers. If host and
   client ever print a different `K`, or a cheese fails to remove on one side, adjust `kCell`. Confirm
   `kPickupRadiusSq` (150 cm) against the real maze. Promote to `coop.ini [maze]` if tuning is needed.

5. **Cheese-count property (HUD)** — `writeCheeseCountToGI()` writes the **generator's** `Cheese Amount`
   (`docs/hooks.md` lists it under generator "Counts (read)"). ⚠️ That field is most likely the SPAWNED
   TOTAL, not a collected counter — if the host's `Check for Dungeon Complete` compares collected vs.
   `Cheese Amount`, overwriting it would corrupt the win check. CONFIRM on a real run which object/field
   the HUD reads; if `Cheese Amount` is the spawn-total, retarget the write to the true collected field
   (or the GI's `Cheese Number Array`) and only read the spawn-total. A wrong guess degrades only the
   local HUD number, not sync (both peers converge via the 1 Hz keyframe).

6. **Double-count guard** — confirm the client's native local collect is suppressed (collision disabled).
   If the HUD double-counts on the client, the native path is still firing — neutralize harder (also
   `SetActorHiddenInGame`, or pin and early-out on the resolved collect fn, analogous to dllmain's
   `Turn Off Rat` on the puppet).

7. **Removal style** — `drainInboundToWorld` currently hides + disables collision (flicker-free, safe).
   `applyCollected(id)` additionally calls `K2_DestroyActor()` for a hard remove; if hide-only leaves a
   visible husk, switch `drainInboundToWorld` to call `applyCollected(id)` per id.

---

## 5. Threading audit (must hold — verified against `dllmain.cpp` + the UE4SS headers)

- **GAME THREAD only** (all UObject access): `on_process_event`, `notify_run_begin`,
  `resolveClassesAndFns`, `enumerateAndAssignIds`, `onCheeseCollectHook`, `onExitOverlap`,
  `onDungeonComplete`, `drainInboundToWorld`, `clientProximityScan`, `applyCollected`,
  `writeCheeseCountToGI`, `idForActor`, `isLocalRat`.
- **LOOP THREAD only** (sockets + POD (de)serialization): `on_net_tick`, `on_message`. They copy
  ids/PODs in/out of the shared queues under `*m_mtx` and **never** touch a `UObject*`.
- `UObject*`/`UClass*`/`UFunction*`/`AActor*` NEVER cross the mutex — only `uint16_t` ids and small PODs
  do. The `m_idToActor`/`m_actorToId` maps and all cached `UClass*`/`UFunction*` are game-thread-only and
  never locked.
- The mutex is never held across a UObject call: `drainInboundToWorld` swaps `m_applyQueue` out under
  lock, releases, then mutates actors; `onCheeseCollectHook`/`writeCheeseCountToGI` are sequential lock
  blocks (no nesting → no deadlock).

## 6. Promote-later TODO
Move `PickupCollectedMsg / PickupStateSyncMsg / ObjectiveReachedMsg / RunStartMsg / RunEndMsg /
RestartMsg` (and `hash16`) from `Pickups.hpp` into `Net/Protocol.h` in a dedicated pass — the byte
layouts already match `docs/protocol.md`, so it is a pure move. The duplicate drafts `s2.md`, `m4.md`,
`m4-pickups.md`, `s2-m4-pickups.md` can be deleted once this file is adopted as canonical.
