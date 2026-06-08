# Building the C++ co-op mod (`CyberRatsCoop`)

The co-op mod is a **UE4SS C++ mod**. The networking (UDP/Steam sockets) needs native code — UE4SS
Lua has no socket/file access — so the netcode lives here.

## Toolchain (verified present on this machine)

| Tool | Why | Status |
|---|---|---|
| **clang** (LLVM 18, `x86_64-pc-windows-msvc`) | compiles C++ to MSVC ABI (matches UE4SS) | ✓ `C:\Program Files\LLVM` |
| Windows SDK + MSVC STL | clang links against these | ✓ (clang finds them automatically) |
| **xmake** | UE4SS build system | ✓ `scoop install xmake` |
| **Rust nightly** + cargo | UE4SS's `patternsleuth` dep is Rust | ✓ `rustup … --default-toolchain nightly` |
| **git** | clone UE4SS + submodules | ✓ |

> Note: there is **no `cl.exe`/MSVC compiler driver** installed, but the MSVC *headers/libs* are
> present, so clang produces UE4SS-compatible binaries. If a future UE4SS build hard-requires MSVC,
> install "VS C++ Build Tools" (MSVC + Windows SDK components).

## Source layout

- `third_party/RE-UE4SS/` — full UE4SS source (cloned, submodules via global `url.https.insteadOf`).
  The `ue4ss.mod` xmake rule there links mods against UE4SS.
- `mod/CyberRatsCoop/xmake.lua` — our mod target; `includes("../../third_party/RE-UE4SS")`.

## Build

```powershell
pwsh scripts\build_mod.ps1          # configure + build, copies dll into the game
```
or manually (from `mod/CyberRatsCoop`):
```bash
# ensure PATH has ~/.cargo/bin, scoop/shims, LLVM/bin
xmake f -m "Game__Shipping__Win64" --toolchain=clang -y    # match the game's UE4SS Shipping CRT
xmake build CyberRatsCoop
```
The first build also compiles UE4SS + deps (long; patternsleuth invokes cargo). Output dll →
`build/.../CyberRatsCoop.dll`. Deploy to:
`…\Cyber Rats\Engine\Binaries\Win64\ue4ss\Mods\CyberRatsCoop\dlls\main.dll`
plus `coop.ini` next to it. `scripts\deploy.ps1 -Only coop` handles this (it also auto-detects the built dll).

## Required local patches to the UE4SS source (for the very new MSVC 14.43 toolset)

The cloned `third_party/RE-UE4SS` (git-ignored) has three local patches; re-apply if you re-clone:

1. **xmake version:** use **xmake 2.9.3** (pinned in `third_party/xmake293/`) — the EXACT version
   UE4SS's CI uses (`.github/workflows/*.yml`). 3.0.x breaks `raw_pdb` (`cmake.install` → `ninja
   install`, no install target); 2.9.9 breaks static-lib archiving (`glad.lib`, `linker.lua` nil).
   2.9.3 handles both. `build_mod.ps1` prepends the pinned xmake automatically.
2. **`xmake.lua` (root):** added `add_defines("WINVER=0x0A00", "_WIN32_WINNT=0x0A00", "NTDDI_VERSION=0x0A000000")`
   after `add_rules("ue4ss.core")` — MSVC 14.43 leaves these undefined, tripping UEPseudo's
   "Windows Vista and earlier are no longer supported" guard.
3. **UEPseudo hook headers:** wrap the two `#include <polyhook2/...>` lines with
   `#pragma push_macro("ensure")` / `#undef ensure` / `#pragma pop_macro("ensure")` in
   `deps/first/Unreal/include/Unreal/Hooks/Internal/DetourInstance.hpp` and `DetourSubclasses.hpp` —
   asmjit (via polyhook) has an `ensure()` method that collides with UEPseudo's `ensure` macro.
4. **`UE4SS/src/Mod/LuaMod.cpp:237`:** `[&] -> bool {` → `[&]() -> bool {` — MSVC 14.43 doesn't
   support the C++23 P1102 "lambda without ()" with a trailing return type.

These two are in the **xmake package cache** (`%LOCALAPPDATA%\.xmake\packages\...`), re-apply if the
cache is cleared:
5. **glaze v2.9.5 `.../include/glaze/json/json_t.hpp`:** add `using generic = json_t;` inside
   `namespace glz` (before the namespace close) — UE4SS code uses `glz::generic`, the newer glaze name
   for the generic JSON type (4 files: Dumpers.cpp, LiveView.cpp, JSONDumper.cpp, TMapOverrideGen.cpp).

## CRT matching (important)

UE4SS and the mod **must use the same C runtime + config**. The game ships UE4SS as
`Game__Shipping__Win64` (see `UE4SS.log` line "Build Configuration"). Build the mod with the same
mode, or it will fail to load / crash.

## What's compile-verified already

`Net/Protocol.h`, `Transport/UdpTransport.{h,cpp}`, `Util/Config.h` compile with clang and the UDP
transport passes a loopback host↔client round-trip test (reliable + unreliable delivery). Only
`dllmain.cpp` needs the UE4SS headers (provided by the build) to compile.

## Two-instance local test (after a successful build)

```powershell
pwsh scripts\run_two_instances.ps1     # launches host + client (UDP 127.0.0.1)
```
Expect in each `UE4SS.log`: `[CRCoop] hosting…` / `[CRCoop] connecting…` then
`[CRCoop] … co-op link established`. (Single-instance mutex risk U7 — clone the install if the 2nd
instance refuses.) Steam launch only runs one instance, so 2-instance testing launches the exe
directly — but recall the shipping exe needs Steam context; for local tests use the bootstrap or a
cloned install per instance.
