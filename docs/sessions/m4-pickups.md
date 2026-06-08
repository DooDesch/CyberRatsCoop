# M4 — Pickups & Objective (host-authoritative) — integration + test runbook

Deliverables (written this session):
- `mod/CyberRatsCoop/Game/Pickups.hpp` — module interface + inline M4 wire codecs
  (`PickupCollectedMsg 0x30`, `PickupStateSyncMsg 0x31`, `ObjectiveReachedMsg 0x40`,
  `RunStartMsg 0x70`, `RunEndMsg 0x71`, `RestartMsg 0x72`) + `hash16(cellX,cellY,slot)`.
  Does NOT redefine anything in `Net/Protocol.h`; uses `crc::ByteWriter`/`crc::ByteReader`.
- `mod/CyberRatsCoop/Game/Pickups.cpp` — `crc::game::PickupsModule` implementation.

The module is self-contained (`struct PickupsModule`, namespace `crc::game`). It takes a SHARED
mutex from dllmain (the same `m_mtx` dllmain already uses) plus a `getLocalPawn` callback, so it
slots into dllmain's existing snapshot-under-lock pattern with no new locks. NO edits were made to
`dllmain.cpp` or `Net/Protocol.h`. The wiring below is the serial integration step.

THREADING (unchanged from the rest of the mod):
- `on_process_event(ctx, fn)` and `notify_run_begin(seed)` run on the GAME THREAD (inside the global
  ProcessEvent pre-hook). These are the ONLY methods that touch UObjects.
- `on_net_tick()` and `on_message(fr)` run on the UE4SS LOOP THREAD. Sockets + PODs only.
- Bridge = the shared `std::mutex*` + POD queues. No `UObject*` ever crosses the mutex (only u16 ids).

---

## (a) dllmain.cpp wiring (EXACT)

Five touch-points. All additions; nothing is removed.

### 1. Include (top of dllmain.cpp, with the other project includes)
```cpp
#include "Game/Pickups.hpp"
```

### 2. Member (in the `private:` block of `CyberRatsCoopMod`, near the other game-thread members)
```cpp
crc::game::PickupsModule m_pickups;
```

### 3. Bind + init — in `on_unreal_init()`, AFTER `m_playerId` is set and AFTER the
`RegisterProcessEventPreCallback(...)` block (so transport/role/playerId are known). The module
shares dllmain's mutex and reads dllmain's local pawn through a tiny lambda:
```cpp
    m_pickups.bind(m_transport.get(), m_role, m_playerId, &m_peerReady, &m_mtx,
                   [this]() -> U::AActor* { return m_localPawn; });
    m_pickups.on_unreal_init();
```
> `m_localPawn` is the existing dllmain member set each frame in `onProcessEvent` (the
> controller-possessed local rat). The lambda is only ever invoked on the game thread (from inside
> `m_pickups.on_process_event`), so reading `m_localPawn` there is safe.

### 4. Forward the ProcessEvent stream (GAME THREAD) — in `onProcessEvent(ctx, fn, parms)`.
The module caches its OWN UFunctions from the same `(ctx, fn)` stream, so forward EVERY call.
Add ONE line. Place it right after dllmain caches its own fns (e.g. just before the
`if (fn == m_mazeGenBeginFn)` line), so the module sees the call even when dllmain early-returns:
```cpp
        m_pickups.on_process_event(ctx, fn);
```
Then, INSIDE dllmain's existing `onMazeGenBegin()` (which already runs on the
`Maze_Generator:ReceiveBeginPlay` pre-hook), AFTER the seed is chosen/forced, add:
```cpp
        m_pickups.notify_run_begin((uint64_t)(uint32_t)seed);   // M4: reset per-run state; host queues RunStart
```
> Use the same `seed` value dllmain forced/derived (net seed if present, else `m_forceSeed`). This
> gives the host a RunStart with the correct seed and resets both peers' pickup state on every run.

### 5a. Net send (LOOP THREAD) — in `on_update()`, AFTER the player-state send block:
```cpp
        m_pickups.on_net_tick();
```

### 5b. Inbound dispatch (LOOP THREAD) — in `dispatch(fr)`, add the six M4 types to the switch.
Put these cases before `default:`:
```cpp
        case crc::Msg::PickupCollected:
        case crc::Msg::PickupStateSync:
        case crc::Msg::ObjectiveReached:
        case crc::Msg::RunStart:
        case crc::Msg::RunEnd:
        case crc::Msg::Restart:
            m_pickups.on_message(fr);
            break;
```

### xmake.lua — add the new source file
`mod/CyberRatsCoop/xmake.lua` lists sources explicitly (no glob), so add `Game/Pickups.cpp`:
```lua
    add_files("dllmain.cpp", "Transport/UdpTransport.cpp", "Game/Pickups.cpp")
```
No other xmake change is needed (`add_includedirs(".")` already lets the cpp `#include "Game/Pickups.hpp"`
and `"Net/Protocol.h"` / `"Transport/INetTransport.h"`).

---

## (b) Build + deploy

From the project root (PowerShell):
```powershell
pwsh -File scripts/build_mod.ps1
```
This configures + builds `CyberRatsCoop` from the UE4SS root and deploys
`ue4ss/Mods/CyberRatsCoop/dlls/main.dll` (+ `coop.ini`, enabled in `mods.txt`). First build compiles
UE4SS + deps (long). Incremental rebuild after this change is ~6 s.

Expected on success:
```
[CR] Gebaut: ...\CyberRatsCoop.dll
[CR] Deployed -> ...\ue4ss\Mods\CyberRatsCoop\dlls\main.dll (+ coop.ini, enabled in mods.txt)
```

If the build fails on an unresolved symbol for `PickupsModule::notify_run_begin` (or `applyCollected`),
confirm `Game/Pickups.cpp` is in the `add_files(...)` line above and re-run.

---

## (c) 2-instance in-game test

Terminology: peer A = HOST (started via Steam), peer B = CLIENT (bootstrap exe with CLI args).

### Launch
1. HOST — launch Cyber Rats normally via Steam. The mod reads `role=host` from `coop.ini` (or pass
   `-CRCoopRole=host`).
2. CLIENT — launch the bootstrap directly:
   ```
   "Cyber Rats.exe" -CRCoopRole=client -CRCoopConnect=127.0.0.1
   ```
   (Same machine for a loopback test; use the host LAN IP for two machines.)

### Drive a REAL run (NOT `open Maze_LVL`)
Difficulty-0 direct-open spawns 0 cheese (docs/hooks.md). Do a normal rat-select → start so the
generator's `Spawn All Cheese` actually runs and `Random Seed Roll` is consumed. Both peers must be
on the SAME run (M3 seed-sync already forces the identical maze).

### Confirm link + run start (both UE4SS.log windows)
Already-working M2/M3 lines:
```
[CRCoop] HelloAck; co-op link established                 (client)
[CRCoop] peer Hello ok; co-op link established            (host)
[CRCoop] forced maze seed = <N>                           (both, M3)
```
New M4 lines at run start:
```
[CRCoop] M4 pickups module ready (role=...)               (both, at init)
[CRCoop] M4: run begin (runId=..., seed=...)              (both, on ReceiveBeginPlay)
[CRCoop] M4: Spawn All Cheese seen; deferring enumeration (both, when cheese spawn fires)
[CRCoop] M4: enumerated <K> cheese pickups (role=1)       (HOST, ~30 rat-tick frames later)
[CRCoop] M4: enumerated <K> cheese pickups (role=2)       (CLIENT — same K)
```
SUCCESS CRITERION 1: both peers print the SAME `<K>` cheese count. (Identical maze → identical set.)

### Confirm host-authoritative collection (no double-count, removed on both)
Walk EITHER rat over a cheese.

If the HOST rat collects it (natural game path → the pinned collect fn fires):
```
[CRCoop] M4: host collected cheese id=0x.... kind=0       (HOST)
```
…and the client receives the state sync and removes its mirror:
```
[CRCoop] M4: applied collected cheese id=0x....           (CLIENT)
```

If the CLIENT rat collects it (its cheese have collision disabled → no local self-collect; the
proximity scan sends a request):
```
[CRCoop] M4: client request collect id=0x.... kind=0      (CLIENT)
[CRCoop] M4: host confirmed client collect id=0x....      (HOST)
[CRCoop] M4: applied collected cheese id=0x....           (CLIENT, on the host's confirm)
```
ON-SCREEN: the cheese disappears in BOTH games, and the in-game cheese counter increments exactly
ONCE. SUCCESS CRITERION 2: the same `id=0x....` appears on both peers; the cheese is gone in both;
count goes up by exactly 1 (no double-count even if both rats touch it near-simultaneously — the
host's first write wins and the second request is dropped silently).

### Confirm the 1 Hz keyframe / late resync
Have the client miss a packet (or rejoin). Within ~1 s the host's periodic keyframe re-aligns it:
no log spam required; the visible test is that any cheese the host has collected stays gone on the
client after the next keyframe. (Host emits PickupStateSync on ch2 unreliable every 1000 ms.)

### Confirm objective + run lifecycle
Walk a rat into the maze exit (`BP_EnterExit_C`), then complete the run:
```
[CRCoop] M4: exit reached (byPlayer=...) -> ObjectiveReached   (the peer that reached it)
[CRCoop] M4: ObjectiveReached id=0x0001 byPlayer=...           (the other peer)
[CRCoop] M4: dungeon complete -> RunEnd(win) runId=...         (HOST, on Check for Dungeon Complete)
[CRCoop] M4: RunEnd runId=... result=1 reason=0                (CLIENT)
```
SUCCESS CRITERION 3: ObjectiveReached crosses both peers, and RunEnd(result=1) is emitted by the
host and received by the client exactly once.

---

## (d) Open items to confirm/calibrate live (these were 0-instance on the offline open)

The module is written to degrade gracefully, but verify these in a real run and tighten if needed:

1. **Pickup/exit `/Game/...` paths.** `resolveClassesAndFns()` tries
   `/Game/Pickups/Chese_Pickup.Chese_Pickup_C`, `/Game/Pickups/Last_Chese_Pickup.Last_Chese_Pickup_C`,
   `/Game/Pickups/BP_EnterExit.BP_EnterExit_C`. Enumeration itself is path-INDEPENDENT (it uses
   `FindAllOf(STR("Chese_Pickup_C"))` etc. by class name), so `enumerateAndAssignIds` works even if
   the StaticFindObject paths are wrong — but the collect/exit hooks need the `IsA(class)` test, which
   needs the correct class. If "enumerated K" prints but no collect/exit lines ever appear, dump the
   real paths (`obj->GetClassPrivate()->GetFullName()` while iterating) and correct the three
   `StaticFindObject` paths in `resolveClassesAndFns`.

2. **Cheese collect UFunction name.** `resolveClassesAndFns` probes a candidate list
   (`On Collected`, `Collect`, `Collected`, `Pickup`, `On Pickup`, `Interact`, `On Interact`) on any
   UFunction whose `ctx IsA m_cheeseClass`. If none matches (no "host collected"/"client request" lines
   when a rat clearly grabs cheese), log every `fn->GetName()` where `ctx->IsA(m_cheeseClass)` during a
   manual pickup, find the real collect fn, and add its name to `kCheeseCollectCandidates`.

3. **Cell pitch `kCell`.** Ids are `hash16(round(x/kCell), round(y/kCell), round(z/kCell-folded), slot)`.
   `kCell = 100.0` (1 m) quantizes away cross-process FP noise; the maze cells are far larger, so two
   distinct cheeses never share a quantized cell in practice, and the deterministic sort+nudge resolves
   any collision identically on both peers. If host and client ever print different `<K>` or a cheese
   fails to remove on one side, raise/lower `kCell` so each cheese maps to a unique, stable cell on both.

4. **Suppression layer.** Primary layer = `SetActorEnableCollision(false)` on every client cheese at
   enumeration (so it can't self-collect; intent comes from the proximity scan). If client cheese still
   self-collect (an interface call not gated by collision), the collect-fn hook (`onCheeseCollectHook`)
   already guards with `if (!isHost()) return;` after sending nothing — verify no client-side count bump
   happens; if it does, neutralize harder (also `SetActorHiddenInGame` is available, or pin the exact
   collect fn and early-out there).

5. **Removal style.** `drainInboundToWorld` currently hides + disables collision (flicker-free, safe).
   `applyCollected(id)` additionally calls `K2_DestroyActor()` for a hard remove; if hide-only leaves a
   visible husk in-game, switch `drainInboundToWorld` to call `applyCollected(id)` per id instead of the
   inline hide/disable.
