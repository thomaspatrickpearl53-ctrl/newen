# Android: plan

Design written 2026-11-21 from newen at commit 2ba971b. The owner chose: **Android only**, **hlsdk-portable** for the
game code, **rewrite** the old OpenGL calls (no gl4es), test phone a **Samsung Galaxy A26** (64-bit ARM, Mali GPU,
OpenGL ES 3.2). The owner said "go" on 2026-11-21.

## 1. In plain terms

The PC version runs Valve's own compiled game files (`hl.dll`/`hl.so`, `client`, `vgui`, `GameUI`). They are 32-bit
x86 machine code and can't run on a phone's ARM processor. Valve published the source of the game code (the HL SDK),
and `hlsdk-portable` (github.com/FWGS/hlsdk-portable) is that source fixed to compile for 64-bit and ARM. The phone
version runs it, built for the phone. What only exists as Valve binaries can't come along: the VGUI1 menus
(hlsdk-portable has non-VGUI scoreboard and MOTD fallbacks) and the GameUI main menu (the phone gets a small Lua menu).

The PC version doesn't change: it keeps running Valve's real files, 32-bit, as before. Every step below keeps Windows
and Linux working, and each is tested on Linux before anything is tried on the phone.

## 2. Steps

| Step | What | Tested by |
|---|---|---|
| 1 | **64-bit build.** newen builds and runs as a 64-bit program (Linux x86-64 first) and loads hlsdk-portable's 64-bit `hl`/`client` | a 64-bit Linux run of c1a0 |
| 2 | **OpenGL ES.** Every `glBegin`/`glEnd`, matrix-stack and fixed-function call is replaced by vertex arrays and small shaders, so the same code runs on desktop GL 3.x core and GLES 3 | desktop runs, then `-gles` (SDL asks Mesa for an ES 3 context) |
| 3 | **Touch and the phone menu.** On-screen move stick, look drag, fire/jump/duck/use/reload/weapon buttons, laid out and drawn in Lua; gamepads through SDL's controller API; a Lua main menu (new game, load, settings, quit) for builds without GameUI | desktop with mouse-simulated touches (`-touch`) |
| 4 | **Android package.** Gradle project in `android/`, SDL2's Java activity, the NDK building newen, Lua, Jolt and hlsdk-portable for `arm64-v8a`; game data read from a folder the owner copies to the phone | an APK that installs |
| 5 | **On the phone.** Lifecycle (pause in the background, GL context loss), screen size and HUD scale, performance on the A26 | the owner, with screenshots |

## 3. Step 1 in detail: 64-bit

Things that assume 32-bit today:

- **`CMakeLists.txt`** refuses 64-bit. It becomes a check that a 64-bit build isn't pointed at Valve's 32-bit files.
- **`string_t`** is an `int` offset from `pStringBase`, and the engine allocates strings with `Mem_Alloc`, so on 64-bit
  the offset can exceed 32 bits. The engine's strings move into one arena next to `pStringBase` (reserved address
  space, grown as needed, reset per map). hlsdk-portable's `MAKE_STRING` already falls back to `ALLOC_STRING` when
  the DLL's own constants are too far away; the engine's `SV_MakeString` does the same.
- **Struct layouts and size asserts** in `game_api.h`, `client_api.h` and the VGUI/GameUI bridges were written for
  32-bit. The structs shared with the game are checked against hlsdk-portable's headers on 64-bit; the VGUI1/GameUI
  bridges (Valve-binary only) are left out of a 64-bit build.
- **Saved games**: the engine walks the game's save tables (`TYPEDESCRIPTION`) and stores entities and pointers;
  field sizes that are pointers must use the real pointer size.
- **Library names**: hlsdk-portable names its libraries with the architecture (`hl_amd64.so`, `client_amd64.so`,
  `libserver.so` on Android). The engine tries the suffixed name before the one in `liblist.gam`.
- **Pointer/int casts** anywhere else, found with gcc's `-Wpointer-to-int-cast -Wint-to-pointer-cast` and a review.

hlsdk-portable is not copied into newen. `tools/build_hlsdk.sh` fetches a pinned commit and builds it; the built
libraries go next to the engine (`bin/hlsdk/valve/...`) and are searched before the Half-Life folder's own, without
ever writing into the Half-Life folder.

## 4. Decisions in this plan

- Game data on the phone: the owner copies Half-Life's `valve` folder to the phone's shared storage
  (`Android/media/<app id>/valve` or a folder picked at first start); saves and settings go to the app's own storage.
- Licences: hlsdk-portable is under Valve's Half-Life SDK licence. newen loads it as a game library, which the
  game-DLL exception (`LICENSE-EXCEPTION`) covers; its code never goes into newen's own sources.
- Release builds for the phone are `arm64-v8a` only.

## 5. Built

- **Step 1 (2026-11-21):** built and tested on Linux without Half-Life's data (see "64-bit" in `HANDOFF.md`,
  section 5). Left for the owner: `ARCH=64 ./build.sh`, `tools/build_hlsdk.sh`, `bin64/newen +map c1a0` on the
  CachyOS machine, walking, a level change, save and load, and a screenshot.
