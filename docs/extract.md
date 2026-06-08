# Asset extraction & Mappings.usmap

UE 5.6 uses **unversioned properties**, so readable Blueprint property names require a
`Mappings.usmap`. The cleanest way (no editor, no separate dumper) is the **UE4SS built-in**
`Dumper_Usmap` console command.

## 1. Generate Mappings.usmap (game must be running with UE4SS)

1. Launch Cyber Rats with UE4SS installed (`scripts\setup.ps1` done).
2. Open the **UE4SS GUI console** (the separate window, not the log).
3. Type:  `Dumper_Usmap`
4. It writes `Mappings.usmap` into `…\Cyber Rats\Engine\Binaries\Win64\`.
5. Pull it into the project:  `pwsh scripts\pull_usmap.ps1`

> Generate from the **same exe** you will mod (shipping vs dev can differ slightly). Re-generate
> if you switch targets.

## 2. Extract paks with FModel (GUI)

- Install FModel: https://fmodel.app
- Directory selector → add `…\Cyber Rats\Cyber_Rats\Content\Paks`
- UE version: **GAME_UE5_6**  ·  AES key: **leave blank** (IoStore `SignatureHash=0`)
- Load `Mappings.usmap` (Settings → General → Mapping file) — the one from step 1.
- Browse to and export (Save Properties / Save as JSON / decompile) the target assets listed in
  `docs/hooks.md`, primarily:
  - `Procedural_Maze/Maze_Generator`, `Maze_LVL`, `Master_Maze_Room`, `Biomes`, `Wall_Styles`, `BP_EnterExit`
  - `Characters/Lab_Rat/BP_LabRat`, `ABP_LabRat`, `BP_SelectLabRat`, `BP_Dead_LabRat`
  - `SaveGame/GameMode_LabRats`, `GameInstance_LabRats`, `Player_Controller_CyberRats`
  - `Interactions/Chese_Pickup`, `Last_Chese_Pickup`, `Interact_Interface`
  - `Characters/Rat_Cyborgs/BP_*Cyborg_Spawner*`, `Characters/DAVE/BP_DAVE`, `Krueger_*`, `Characters/Team_Rat/BP_TeamRat`
- Save JSON exports under `extracted\` (git-ignored — do not redistribute game content).

### CLI alternative (CUE4Parse)
A small CUE4Parse/.NET tool can batch-export the same set headlessly with `GAME_UE5_6` + the
usmap. Optional; FModel is enough to fill `docs/hooks.md`.

## 3. What to harvest (feeds docs/hooks.md)

- **Maze_Generator:** an `int Seed` and/or `FRandomStream` property; grid dims
  (Width/Height/Rows/Cols/Cell); the generate function name; the spawned-actor arrays.
- **BP_LabRat:** movement/anim props, montage refs (Bite/`LabRat_Revive_A`/Exit), `ELabratSprintStates`, health/downed.
- **Chese_Pickup / Interact_Interface:** the interface method name (`Interact`/`OnInteract`/`Collect`) and where the cheese count lives.

Cross-check every name against UE4SS **Live View** (search class → expand instance) before hooking.
