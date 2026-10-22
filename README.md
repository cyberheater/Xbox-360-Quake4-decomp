# Xbox 360 Quake 4 decomp

A static recompilation of the **Xbox 360 version of Quake 4** so it runs natively on Windows PCs.

> ## You must supply your own copy of the game
>
> **This repository contains no game files.** No ISO, no `default.xex`, no `.pk4` data, no
> recompiled game code and no other assets from Quake 4 are included, and none will be accepted.
> To use this project you need **your own legally obtained Xbox 360 copy of Quake 4** (the disc /
> ISO you own). Extract it yourself (for example with
> [extract-xiso](https://github.com/XboxDev/extract-xiso)) and point the game at the extracted folder.
>
> This project is not affiliated with or endorsed by id Software, Raven Software, Activision,
> ZeniMax, Microsoft or Xbox. "Quake" and "Quake 4" are trademarks of their respective owners.
> It exists for education and preservation research. Please do not use it to pirate games.
>

## What's new in v0.2.0

A new native-renderer build, **`q4hle.exe`**, built alongside `q4.exe` (`-DQ4_HLE=ON`):
- **Native Direct3D 11 renderer** that draws the game's own D3D calls directly instead of emulating the
  Xbox 360 GPU, with internal resolutions up to 5120x2880
- **120 Hz game tick** (`q4_tickrate`, automatic from the display's refresh rate) with the game's timing
  code adjusted so physics, AI and animation stay correct
- The game's renderer back end runs on its **own thread**, a frame behind the front end (`Q4_RB_THREAD=0`
  turns it off); in heavy fights about 117-119 fps instead of 94
- **GRAPHICS page in the game's own Options menu**: resolution, display mode, V-Sync, anti-aliasing (FXAA),
  texture filtering, FPS counter and **side-by-side stereoscopic 3D** (half / full SBS, three depth
  levels), with an APPLY entry; changes are saved to `q4.toml`
- **QUIT GAME** on the title-screen menu
- Details and all switches: `rexglue-port/docs/hle.md` and `rexglue-port/docs/tickrate.md`
- Fixed: the view weapon could flicker unlit while the render thread was drawing it

## Why?

Quake 4 was a launch game for the Xbox 360 and suffered as a result.  Ran at an unlocked frame rate 
that wildly wandered between 10 and 60 fps.  I wanted a version that ran smooth.
Why not use the PC version.  The Xbox 360 version has native support for joypads with a touch of 
auto aim which really improves the joypad experience.

## Status

I have only tested the first couple of levels, running on the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (an Xbox 360 recompilation framework built on
Xenia's kernel/GPU/audio layers).

Working:
- Boots straight to the title screen with no command-line flags; menus, intro movies and the first level run
- **Reads the game straight from your ISO** (no extraction needed), or from an extracted folder. If it
  cannot find your game it asks you to locate your Quake 4 ISO (or `default.xex`) and remembers the choice in
  `q4.toml`; you can change it later in the F10 menu (Advanced tab)
- Locked 60 fps with even frame pacing, synced to your display (see "Frame pacing" below)
- Joypad input (Start skips an intro movie, one press per movie), keyboard fallback
- Defaults: 1920x1080 window, fullscreen, FXAA anti-aliasing and V-Sync on (the game renders at its native
  720p internally; the internal resolution can be raised to 1440p/2160p/2880p)
- In-game settings menu on **F10**: fullscreen, V-Sync, window size, internal resolution (1x-4x),
  anti-aliasing (FXAA), texture filtering, mouse-cursor hiding, FPS counter. Saved to `q4.toml`
- **F9** toggles an FPS counter (top-right corner: frames per second, frame time, longest recent frame)
- The mouse cursor hides itself while playing and returns when you move the mouse
- **F4** opens the SDK's generic engine-settings overlay

Not done / known issues:
- Early build: expect crashes and rough edges further into the game
- The F10 menu is a separate overlay; the GRAPHICS page (q4hle) is part of Quake 4's own Options menu
- The 3D mode is built from the depth buffer (one rendered view), so edges of near objects can stretch a
  little and see-through effects take the depth of what is behind them
- Audio is whatever the SDK's XMA/audio path provides; it has had little testing
- Paths are currently hard-coded for `C:\Quake4` (see "Paths")

## Repository layout

| Folder | What it is |
|---|---|
| `rexglue-port/` | **The current approach.** A ReXGlue project for Quake 4: config (function boundaries, hooks), app code (settings menu, startup tweaks, 120 Hz tick, in-game menus), the native renderer (`src/hle/`, built as `q4hle.exe` with `-DQ4_HLE=ON`), docs, build scripts, and `rexglue-sdk-patches.diff` (fixes made to the SDK). |
| `legacy-runtime/` | My earlier hand-written runtime (XenonRecomp output + a custom Direct3D 11 / Xenos GPU emulation). Superseded by the ReXGlue port but kept for reference. Sources only. |
| `legacy-runtime-config/` | Function/switch-table analysis data and helper scripts used with XenonRecomp. |

## Quick setup (one click)

1. Install **Visual Studio 2022** (the free Community edition or Build Tools) with *Desktop development
   with C++*, *C++ Clang tools for Windows* and *C++ CMake tools for Windows*, and **git**. (If they are
   missing, the setup offers to install them for you with `winget`.)
2. Download this repository (Code > Download ZIP, or `git clone`) and double-click **`Setup.bat`**.
3. Pick your Quake 4 ISO (or `default.xex` from an extracted copy) when asked.

The setup downloads the ReXGlue SDK, applies this project's patches, builds it, converts the game code
from **your** copy on your PC, and builds `q4hle.exe` and `q4.exe` into a `Quake4` folder next to
`Setup.bat`, with a "Quake 4" desktop shortcut (q4hle). The first run takes roughly 30-60 minutes; run it
again after updating the repository to rebuild (`Setup.bat -Clean` rebuilds the game from scratch). A log
is written to `setup.log`.

## Building (ReXGlue port)

Requirements: Windows 10/11, Visual Studio 2022 with the C++ Clang tools (clang 19+ works), CMake 3.25+, Ninja, git.

1. **Get the SDK at the matching version and apply the patches**
   ```
   git clone https://github.com/rexglue/rexglue-sdk.git
   cd rexglue-sdk
   git checkout v0.10.0
   git submodule update --init --recursive
   git apply ../Xbox-360-Quake4-decomp/rexglue-port/rexglue-sdk-patches.diff
   ```
   On Windows, git may check out some `libmspack` symlinks as plain text files; if the build fails in
   `thirdparty/libmspack/cabextract/mspack`, replace those files with copies of their targets.
2. **Build and install the SDK** (`cmake --preset win-amd64`, then build the `install` target in
   Release). `rexglue.exe` ends up in the install `bin/` folder.
3. **Create the project from your own game files.** Copy your extracted `default.xex` into
   `rexglue-port/assets/` (this path is git-ignored), then run:
   ```
   rexglue codegen        # generates the recompiled C++ into generated/default (git-ignored)
   ```
   The generated code is a translation of the game's own code, so it is **not** distributed here and
   must be produced on your machine from your copy.
4. **Build** the project with the `win-amd64-release` preset and copy the SDK's `rexruntime.dll`,
   `rexgpu-xenos.dll` and `TracyClient.dll` next to `q4.exe`.
5. **Run** `q4.exe`. It looks for the game in the location saved in `q4.toml`, then `C:\Quake4\game`,
   then a `game` folder next to the exe. If none is valid it shows a file dialog so you can pick your own
   Quake 4 ISO (or `default.xex` from an extracted copy). You can also pass `--game_data_root <iso or folder>`.

`rexglue-port/tools/` has the build scripts I use (`build.bat`, `q4build.bat`) and the recorded pad replay - the
scripts contain my local paths and will need editing.

## Paths

The scripts and defaults assume the game data in `C:\Quake4\game` and the SDK in `C:\Quake4\rex\sdk`.
Change them (in `q4_app.h`, the `.bat` files, and `tools/`) for your machine.

## Frame pacing

Quake 4's Direct3D library throttles the game on GPU fences, and the emulated GPU held every swap
until the next vblank. On some frames the game then needed a fence that was only written one vblank
later, giving a 33 ms frame followed by a 3 ms catch-up frame about every 21 frames - visible judder.
An earlier bug also left the game's 16 ms tick timer dead (`NtSetTimerEx` with the "resume" flag), which
capped it at exactly 10 fps. Later, the screen showed an occasional frame twice because the game's frames
landed right on the display's own paint tick.

Fixes (all in `rexglue-sdk-patches.diff`):
- `NtSetTimerEx` now arms the timer when the resume flag is set
- the GPU command processor no longer waits on the "previous swap done" flag; `VdSwap` instead paces the
  game to one swap per emulated vblank
- the emulated vblank follows the real display's vblank (when V-Sync is on and the refresh rate is a whole
  multiple of 60 Hz) instead of a software timer, and the game's 16 ms periodic tick is driven from it
  (exactly 1/60 s when V-Sync is off)
- every frame is held to a fixed point (8 ms) after the vblank before it is handed to the presenter, so it
  never lands on the display's paint tick (`Q4_SWAP_PHASE_MS` changes this, 0 disables it)
- the command processor wakes when the vblank interrupt finishes instead of polling

Measured with a recorded pad replay at 4K / 3x internal resolution, over about 50 s of the opening cutscene:
frame-time spread 0.3 ms (about 1 ms before), no slow frames, and no repeated display frames (about one every
1.5 s before). With V-Sync off the game still runs at 60 fps: its game loop is a fixed 60 Hz tick, so extra
rendering would only repeat frames.

`rexglue-port/tools/input_replay_start_to_ingame.txt` is a recorded pad sequence (record/replay is built into
the patched SDK via `Q4_INPUT_RECORD` / `Q4_INPUT_REPLAY`; the replay assumes no savegame exists).

## Performance

On a Ryzen-class PC with an RTX 5070 the opening cutscene used about 0.24 of one CPU core and about 3% of the
GPU at the default settings (about 5% at 1440p internal, 10% at 2160p). That is one scene on one machine, not a
benchmark of the whole game, and I have not tested a Steam Deck or any Linux build.

## Other notes

- A handful of function boundaries had to be added by hand (`q4_config.toml`): the recompiler merged tiny
  virtual-call stubs into their neighbours, which crashed when the game called them through vtables.
- Plugin (GPU) settings can't be set from the command line; use environment variables or the in-game menu.
- Quake 4 opens a LAN (system link) UDP socket at startup even in single player. It is bound to 127.0.0.1 only
  (`net_bind_loopback_only`, on by default) so Windows Firewall does not ask for permission; LAN play does not work.

## Credits and licences

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (BSD-3-Clause) and, through it, the
  [Xenia](https://github.com/xenia-project/xenia) project
- [XenonRecomp / XenosRecomp](https://github.com/hedge-dev) by hedge-dev (used by the legacy runtime)
- Reference projects studied: UnleashedRecomp, LostOdysseyRecomp, GoldenEye-Recomp, jeff

## AI assistance

This project was developed with the help of an AI coding assistant (Anthropic's Claude). All behaviour
described here was checked by running the game and measuring it; where something is unverified the
text says so. No game data is invented or embedded - everything about Quake 4's behaviour comes from running
the user's own copy.

## Licence

The code in this repository is licensed under the **GNU General Public License, version 2 or (at your
option) any later version** (GPL-2.0-or-later), the same terms as the author's other emulation work. See
[`LICENSE`](LICENSE). This covers the project's own code only: it does **not** cover any Quake 4 game
data or code, none of which is distributed here. `rexglue-port/rexglue-sdk-patches.diff` modifies ReXGlue
and remains under its BSD-3-Clause licence, and the legacy runtime builds on XenonRecomp (MIT).
