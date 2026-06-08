# Session workflow outputs (raw) — read this first

These files are the **raw output of the per-session dynamic workflows** (design → implement → review),
plus the draft modules they wrote to `mod/CyberRatsCoop/Game/`. They are **reference/WIP**, NOT yet
integrated into the build (`xmake.lua` builds only the verified `dllmain.cpp` + `UdpTransport.cpp`).

## What happened (honest note)

Five workflows were launched, one per remaining session (S1 M3-validation, S2 M4 pickups, S3 enemies,
S4 death/revive, S5 Steam+anim). A **bug in `args` propagation** meant each workflow's milestone
fields arrived as `undefined`, so every agent fell back to "the next roadmap item is M4" and **all
five designed M4 (pickups & objective)**. M5/M6/M7 got no design this round. The upside: **four
independent agents converged on the same robust M4 design**, which strongly validates it. Files
`m4.md`, `m4-pickups.md`, `s1.md`, `s2.md`, `s2-m4-pickups.md`, `undefined.md` are these (overlapping)
M4 runbooks; `Game/Pickups.{hpp,cpp}` and `Game/MazeModule.{hpp,cpp}` are the (last-writer-wins) drafts.

## The validated M4 design (consensus of all four)

- **Host-authoritative** cheese + objective; **client proposes, host disposes** (request→confirm).
- **Stable pickup ids = `hash16(cellX, cellY, slot)`** from the cheese's maze-cell (both peers build the
  identical maze in M3, so positions match → ids match without sending a map). Last-cheese = `kind=1`.
- **Critical engine constraint:** the global `RegisterProcessEventPreCallback` pre-hook **cannot cancel**
  a UFunction call. So the client can't "block" its local collection — instead the authoritative count
  lives only on the host and the client reconciles via the `PickupStateSync` keyframe. (This shaped the
  whole authority model — don't assume call-cancellation.)
- Bind cheese a few frames after `Maze_Generator:Spawn All Cheese` (mirror the M3 `m_mazeGenFrames` delay).
- Fallback if the exact interact UFunction is unknown: **proximity collection** on the local rat tick.

## Blocking prerequisite for M4–M6 (all designs flagged it)

The exact Blueprint UFunction names for cheese-collect, the exit overlap, `Kill Rat`, etc. are **not in
`docs/hooks.md`** because the autonomous tests used `open Maze_LVL` (difficulty 0 → 0 cheese/enemies).
They must be resolved in a **real run** (rat-select → start) via UE4SS Live View. That real-run driving
is the shared first step for the remaining gameplay milestones.

## Status of the foundational pieces (done this session, in the build)

- `Net/Protocol.h`: all M4–M6 message structs added + compile-tested (`PROTOCOL OK`).
- `docs/sessions.md`: the per-session plan (goals/hooks/deliverables).
- The runbooks below give concrete `dllmain.cpp` wiring + test steps for M4.
