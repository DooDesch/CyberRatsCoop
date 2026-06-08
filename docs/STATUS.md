# Cyber Rats Co-op — live status

Plan: `C:\Users\denni\.claude\plans\in-diesem-ordner-liegt-prancy-clover.md`. This file tracks
what's actually done/verified vs. the milestone plan.

## ✅ M0 — Foundation & introspection (COMPLETE, verified in-game)

- UE4SS (`UE4SS_v3.0.1-954`, experimental) installed into the game; **injects on UE 5.6** after a
  custom `StaticConstructObject_Internal` AOB (`third_party/ue4ss/overrides/`). Verified:
  `PS scan successful` + `Using engine version: 5.6` + mods load.
- `CRToolkit` Lua mod written; the game was driven **autonomously** (Steam launch → `open Maze_LVL`)
  and dumped all key classes live. Results → `docs/hooks.md`.
- **Determinism verdict = SEED-SYNC** (R-3 resolved): `Maze_Generator_C.Stream` (FRandomStream)
  seeded from `GameInstance_LabRats_C."Random Seed Roll"`; all spawning centralized in the generator.
  Layout-stream fallback retained; final empirical check happens in M3.
- Key infra learned (see `memory/ue4ss-cyberrats-quirks`): `NotifyOnNewObject` + `ExecuteWithDelay`
  work; `RegisterLoadMapPostCallback` + `RegisterKeyBind` (synthetic) don't; Steam launch required.

## ✅ M1 — Transport + handshake (COMPLETE, verified in-game 2026-06-08)

**`CyberRatsCoop.dll` builds, loads in-game, and TWO real game instances complete the UDP
Hello/HelloAck handshake.** Verified log:
```
Starting C++ mod 'CyberRatsCoop'
[CRCoop] hosting on UDP 7777
[CRCoop] connecting to 127.0.0.1:7777
[CRCoop] peer Hello ok; co-op link established
[CRCoop] HelloAck; co-op link established
```
- **Build:** `scripts/build_mod.ps1` (xmake 2.9.3, MSVC, builds UE4SS from source + the mod, deploys
  to `…/ue4ss/Mods/CyberRatsCoop/dlls/main.dll`). 7 source/toolchain patches (see `docs/build.md`).
- **2-instance launch method (IMPORTANT, reused for all future testing):** instance 1 (host) via
  Steam (`steam://rungameid/3565080`, role from coop.ini); instance 2 (client) via the **bootstrap**
  `"Cyber Rats.exe"` directly (NOT the shipping exe — that exits early) with Steam env vars
  (`SteamAppId/SteamGameId=3565080`) + `-CRCoopRole=client -CRCoopConnect=127.0.0.1` args (the
  bootstrap forwards them). Two instances coexist fine (U7 resolved). Both share one `UE4SS.log`.

## ✅ M2 — Player presence (COMPLETE, verified in-game)

Each player's rat appears in the other player's game as a network-driven puppet. Verified: both
instances logged `spawned remote puppet rat` + `puppet neutralized (Turn Off Rat)`.
- `dllmain.cpp`: global `RegisterProcessEventPreCallback` (game-thread) hook detects the local
  `BP_LabRat` (the one with a `Controller`) via its `ReceiveTick`, reads transform → mutex snapshot →
  on_update sends `PlayerState` @20 Hz. On inbound `PlayerState`, spawns a `BP_LabRat` puppet
  (`UWorld::SpawnActor`), neutralizes it (`Turn Off Rat`), and drives it via
  `K2_SetActorLocationAndRotation`. Net thread (on_update) vs game thread (hook) separated by a mutex.
- Verified UE4SS C++ Unreal API: `FindFirstOf`, `StaticFindObject<UClass*>`, `Cast`, `IsA`,
  `GetValuePtrByPropertyName(InChain)`, `K2_GetActorLocation/Rotation`, `K2_SetActorLocationAndRotation`,
  `GetFunctionByNameInChain`+`ProcessEvent`, `RegisterProcessEventPreCallback`.

## 🟡 M3 — Shared maze (seed-sync MECHANISM done; full validation pending)

- **Done & running in-game:** pre-hook on `Maze_Generator_C:ReceiveBeginPlay` (game thread) sets
  `GameInstance_LabRats_C."Random Seed Roll"` before generation (`forced maze seed = N` logged), and
  the host shares its seed with the client via a reliable `MazeSeed` message (client stores + applies).
- **Not yet conclusively validated:** that the forced seed visibly changes/syncs the maze. Reasons:
  the autonomous test drives the maze via `open Maze_LVL`, which is a **difficulty-0** state that may
  not consume the seed for randomization; and the room-layout fingerprint came back empty
  (`Final Room List` is cleared post-generation; spawned rooms are biome-specific classes not easily
  enumerated). **Follow-up:** validate in a real run (rat-select → start, where the seed is actually
  rolled/consumed); add a fingerprint that enumerates the real spawned room actors, or compare maze
  screenshots between peers.

## ✅ Live 2-instance test (2026-06-09) — core co-op validated end-to-end

Host (Steam launch, role=host) + client (bootstrap exe, `-CRCoopRole=client -CRCoopConnect=127.0.0.1`),
loopback UDP, both forced seed 1337. **Verified in one shared session, no crash:**
- M1 handshake — both logged `co-op link established`.
- M2 — **both instances spawned a puppet rat of the other** (`spawned remote puppet rat` ×2 + `Turn Off Rat`).
- M3 — both forced/built maze seed 1337 (`forced maze seed = 1337`).
- M4 — both built the cheese registry (`cheese registry: 10 pickups`, hash IDs).
- Enemies spawned in the maze (DAVE, critters, spiders, team rats).
- **No crash, no `[CRCoop]` errors across the whole run — the use-after-free fix (#1) holds.**

Then **adversarially reviewed** the M4/M5/M6 code with a multi-agent workflow → 11 real bugs found
(incl. the m_puppet use-after-free crash, cheese-id desync, host-auth death gap, 2 races) → **all
fixed** (commit "Fix 11 bugs from adversarial review"). Test-driven follow-ups (commit "Test-driven…"):
robust death detection via **`Is Dead` polling** (a trap death bypassed the `Kill Rat` hook),
**production seed-sync** (host always shares its seed; client mirrors — no manual force_seed needed),
and M5 enemy-spawn/puppet logging.

**Still to demo live (implemented + protocol-tested + reviewed, but not yet shown firing in-game):**
the cheese collect→replay *event* across instances (manual nav kills the rat in traps; the Lua
auto-collect harness has a find-cheese bug), the M5 enemy-puppet visual mirroring, and an actual
host-authoritative enemy kill of the client's rat. See `docs/test-plan.md`.

## ✅ M4 — Pickups + objective (cheese sync) — COMPLETE, runtime-validated 2026-06-08

Single-instance in-game validation PASSED: entering a real run logged
`[CRCoop] cheese registry: 10 pickups` on maze gen with **no crash** (cheese class resolves,
`FindAllOf` works, deterministic IDs assigned). Still to do with a 2nd instance: confirm a collected
cheese disappears + counts on the peer (see `docs/test-plan.md`).
- **Real cheese class = `BP_Cheese_Pickup_C`** (the M0 `Chese_Pickup_C` was an unused decoy);
  no-param `Interact()` collect fn. See `docs/hooks.md`.
- Deterministic `pickupId` = index in position-sorted `FindAllOf(BP_Cheese_Pickup_C)` (identical on
  both peers via seed-sync — no id-map broadcast). ProcessEvent fast-path hooks `Interact` → on local
  collect broadcast `PickupCollected{id}`; on receive, faithfully replay by calling that cheese's
  `Interact()` (count + `Destroy Cheese` + dungeon-complete → `BP_EnterExit.Open?`). Dedup + re-entrancy
  guard. Thread-safe outbound queue (game-thread hooks enqueue; loop thread is the sole socket driver).

## 🟡 M5 — Host-authoritative enemies — SCAFFOLD built green, NEEDS 2-instance validation

Enemies are AI-driven (`Random Roam`/NavMesh `MoveTo`/chase) → non-deterministic, so host-authoritative.
Enemy classes captured live: `BP_DAVE_C`, `BP_Canister_Rat_C`, `BP_SmokeBomb_Rat_C`, `BP_SpiderRat_C`,
`BP_Rat_Critter_C`, `BP_TeamRat_C`.
- **Host** (role=host): ~10 Hz `FindAllOf` enemies → monotonic id, broadcast `EnemySpawn`(reliable) +
  batched `EnemyState`(unreliable) + `EnemyDespawn`.
- **Client** (role=client): hide its own AI enemies (`SetActorHiddenInGame`+collision/tick off) and
  spawn host-driven puppets (collision/tick off, transform snapped from `EnemyState`); archetype
  `UClass` cached from a live instance; unhandled spawns re-queued until cached.
- ⚠️ **Open risks to test:** client rat can't be damaged while all its enemies are neutralized → death
  must become host-authoritative (ties into M6); puppet AIController may still `MoveTo` (we override
  each state tick); `EnemyHit` (client→host damage) not wired yet.

## 🟡 M6 — Death — detection+sync done; downed/revive deferred

- **Done (builds green):** cache `BP_LabRat_C:"Kill Rat"`; ProcessEvent fast-path → broadcast
  `DeathMsg{playerId,pos}`; peer logs awareness; reset per maze.
- **Deferred (needs a blocking hook + testing):** the global ProcessEvent pre-callback is
  **observe-only** — it cannot turn `Kill Rat` into a "downed" state. Full downed/revive needs a
  Lua-side blocking `RegisterHook` on `Kill Rat` and/or host-authoritative damage (host owns enemy
  collision via M5), plus proximity revive playing `LabRat_Revive_A`.

## ⏭ Remaining

- **2-instance co-test of M4/M5/M6** (needs the foreground / the user — deferred while the user games
  on the same machine). Procedure: `docs/test-plan.md`.
- **M6 downed/revive** (after the host-auth-damage decision from the M5 test).
- **M7** Steam P2P transport (`ISteamNetworkingSockets` behind `INetTransport`) + Steam invites +
  anim/montage sync + netcode smoothing + polish.
The build pipeline (xmake incremental ~6–8 s) and 2-instance launch method are established.

> **Dead-rat gotcha** (affects testing + run-lifecycle): if the selected rat is dead, START / loadout
> `Press Start` silently do nothing until you click **NEW RAT** (tooltip "Remove dead rat / Get a new
> rat") or **REVIVE**. Loadout START can be driven programmatically via `Rat_Selected_Screen_C:"Press Start"`.

### (historical) M1 build/code notes

- **DONE & test-verified (clang, MSVC ABI):**
  - `Net/Protocol.h` — full message set, framing, (de)serialization, FNV-1a.
  - `Transport/UdpTransport.{h,cpp}` — Winsock UDP + minimal reliable layer. **Loopback host↔client
    round-trip PASSES** (reliable Hello, unreliable PlayerState, host→client reliable MazeSeed).
  - `Util/Config.h` — coop.ini reader.
- **WRITTEN, needs UE4SS build to compile:** `dllmain.cpp` (`CppUserModBase`: config → transport →
  Hello/HelloAck handshake → heartbeat; per-game hooks are TODO for M2+).
- **Build toolchain ASSEMBLED & corrected:** **MSVC 2022 BuildTools present** (xmake finds it; the
  shipped UE4SS is MSVC-built → build the mod with MSVC, NOT clang). Plus xmake ✓, Rust nightly ✓
  (patternsleuth), Windows SDK ✓, UE4SS source + submodules ✓.
- **Build-structure fix (important):** building the mod via an *external* `includes(RE-UE4SS)` made
  xmake use the public `raw_pdb` package → `ninja: error: unknown target 'install'` (raw_pdb has no
  cmake install target). UE4SS ships its OWN working `raw_pdb` in `deps/third-repo`, registered only
  when building from the UE4SS root (`ue4ssRoot` config). **Fix applied:** the mod is now a **cppmod**
  — junctioned into `third_party/RE-UE4SS/cppmods/CyberRatsCoop`, listed in `cppmods/xmake.lua`, and
  built from the UE4SS root (`scripts/build_mod.ps1`). Mod `xmake.lua` is the simple
  `target + add_rules("ue4ss.mod")` form.
  ⏳ Build currently **blocked on transient GitHub rate-limiting**: after many clones/downloads this
  session, `fmt 11.2.0` download+clone fail, and `raw_pdb`'s *source* clone fails → xmake falls back
  to the broken public `raw_pdb` (`ninja: unknown target 'install'`); `imgui` also hits a parallel-
  build lock. **Continuation (clean retry, likely succeeds once rate-limit clears):**
  1. Wait for GitHub rate-limit to clear (or `gh auth` / set `GH_TOKEN`); kill stray `xmake`/`ninja`;
     `rm -r` any `installdir.failed` under `%LOCALAPPDATA%\.xmake\cache\packages`.
  2. From `third_party/RE-UE4SS`: `xmake f -p windows -a x64 -m Game__Shipping__Win64 -y` then
     **build all default targets first** (`xmake -y`, not just the one target) so the root's
     `add_repositories("third-party deps/third-repo")` resolves UE4SS's working `raw_pdb`/`fmt`.
  3. If imgui lock recurs, install packages serially: `xmake require -y` before `xmake build`.
  4. Then `pwsh scripts/build_mod.ps1` builds+deploys `CyberRatsCoop` → `…/ue4ss/Mods/CyberRatsCoop/dlls/main.dll`.
  Structure is correct (cppmod junction + registered); only the package fetches need a clean run.

## ⏭ Next (M1 finish → M2)

1. Finish the UE4SS build → produce `CyberRatsCoop.dll`; deploy via `build_mod.ps1`.
2. Two-instance LAN test → confirm `[CRCoop] … co-op link established` in both logs (handshake over real UDP).
3. M2 — wire the Unreal API in `dllmain` and hook per `docs/hooks.md`:
   - `Maze_Generator_C:ReceiveBeginPlay` → inject `Random Seed Roll` (seed-sync) + empirically confirm determinism.
   - `BP_LabRat` spawn/possess → register local pawn; spawn neutralized puppet (`Turn Off Rat`); 20 Hz transform/anim sync.

## Risks resolved / open

- ✅ R-1 (launch/inject): use Steam launch; proxy injects into shipping exe.
- ✅ R-2 (UE4SS 5.6 AOB): custom StaticConstructObject AOB.
- ✅ R-3 (determinism): seed-sync (FRandomStream + central gen).
- ⚠️ U7 (2 instances on one machine): untested; Steam runs one instance, direct shipping-exe launch exits without Steam context → may need the bootstrap or a cloned install for the 2nd instance.
- ⏳ UE4SS-from-source build success with clang (no MSVC driver) — in progress.
