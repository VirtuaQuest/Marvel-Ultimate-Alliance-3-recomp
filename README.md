# MUA3 Recomp

<a href="https://recomplauncher.com"><picture><source media="(prefers-color-scheme: dark)" srcset="https://recomplauncher.com/assets/press/launcher/badge-light.svg"><img alt="Get it on RecompLauncher" src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" height="48"></picture></a>

A work-in-progress static recompilation of *MARVEL ULTIMATE ALLIANCE 3: The Black Order* (Nintendo Switch, 4.0.1:
the base game with its last update) into a native Windows x86-64 program, built on
[LAS Recomp](https://github.com/peiikai/LASRecomp)'s recompiler and Switch runtime (by way of DOAX3 Recomp, which
shares the game's Koei Tecmo engine).

**Status: early, work in progress.** The game boots and runs at its own 30 fps (60 and 120 as options) at 1920x1080
through the title, the update history, save slot and difficulty selection, the opening movie and into the first level
(the Kree ship), with the update's content and the Expansion Pass recognised. Frames are paced on the display's own
refresh (no judder in play or in the movies), and the first start prepares the shaders of the title, the menus and the
first level from your game files ("preparing shaders" in the window title). What has been changed, the debugging
switches and the open work: [docs/CHANGES.md](docs/CHANGES.md).

No game files are included or ever committed, and the releases hold no game code: the game code is recompiled from
your own copy on your own PC. You need your own legally obtained copy of the game, its 4.0.1 update and the keys from
your own console.

## Linux / Steam Deck (this fork)

This fork adds a native **Linux and Steam Deck** version of
[VirtuaQuest's MUA3 Recomp](https://github.com/VirtuaQuest/Marvel-Ultimate-Alliance-3-recomp). Latest: **v8.2**,
tested on a Steam Deck LCD (SteamOS, RADV).

**What's new in this fork:**

- **Button icons that match your controller.** The game's own A/B/X/Y, L/R/ZL/ZR and +/− icons are redrawn when they
  load to show the controller you're holding: Nintendo, Xbox, PlayStation, Steam Deck or Steam Controller. They keep
  the game's look; only the label changes. Choose it in Settings > Controls > **Button icons** (Auto by default; on a
  Deck in Game Mode, Auto picks the Steam Deck style). Code: `runtime/nvn/mua3_prompts.c`.
- **Faster on the Steam Deck.** Heavy fights went from about 15–20 fps to 45–60 fps: the shader translator turns the
  game's branchy GPU programs into real loops and if-blocks, compute shaders reserve only the memory they need, and the
  CPU and GPU work side by side.
- **Smooth movies.** Hardware video decoding (VAAPI), movies read into memory up front, GPU colour conversion, and no
  more 2-second pauses.
- **Fixes:** no audio dropouts, no crash on "return to start screen", an on-screen "PRELOADING SHADERS" bar, and a
  **Fill screen (stretch)** aspect option for the Deck's 16:10 screen.
- **Steam Deck setup:** `run.sh` sets up a Fedora distrobox with everything needed to build, and `play-steam.sh` is the
  entry to add to Steam for Game Mode.

**Install (Linux / Steam Deck):**

1. Download `MUA3-Recomp-Linux-SteamDeck-v<version>-public.tar.gz` from
   [this fork's Releases](https://github.com/Jazzneo/Marvel-Ultimate-Alliance-3-recomp/releases) and extract it
   somewhere you can write to (on a Deck, for example `~/Applications`).
2. In Desktop Mode, run `MUA3 Recomp.desktop` (or `./run.sh`). The first start needs internet: it creates the build
   container and downloads its tools.
3. On **GAME FILES**, choose your base game, the 4.0.1 update, the Expansion Pass (optional) and your `prod.keys`, then
   **INSTALL**.
4. For Game Mode, add `play-steam.sh` to Steam as a non-Steam game.

Logs (`lasrecomp.log`, `game-mode.log`) go to the game folder. Setup details and the Deck's options are in
[STEAMOS.md](STEAMOS.md). The Windows instructions below are unchanged from the original project.

## Play with RecompLauncher

MUA3 Recomp works with [RecompLauncher](https://recomplauncher.com): install it from the launcher, then give Easy Setup
your game (NSP or XCI), the 4.0.1 update NSP, the Expansion Pass NSP if you have it, and your `prod.keys`. The setup
step recompiles the game for your PC (20 to 40 minutes, depending on the CPU; nothing else to install), then Play
starts it. Video, sound, controller and keyboard settings are on the game's page. Keep your NSP / XCI files where they
are: the game reads its data from them. Started from Big Picture mode the game opens full screen (your own window
setting is kept for other starts), and its menus name your controller's buttons from the first frame.

**Recomp Achievements:** the recomp reports the story's progress to RecompLauncher (the chapter each story movie
belongs to, and the last one), and the profile shows the chapter you're on. Which achievements those unlock is
RecompLauncher's list for the game: [docs/achievements.json](docs/achievements.json) is the one proposed (starting the
story, reaching each of chapters 2 to 11, finishing it). Outside RecompLauncher this does nothing.

## Play with the release zip

1. Download `MUA3-Recomp-v<version>-Windows-x64.zip` from
   [Releases](https://github.com/VirtuaQuest/Marvel-Ultimate-Alliance-3-recomp/releases) and unzip it somewhere you can
   write to (Documents or Desktop, not Program Files).
2. Run `mua3launcher.exe`. On **GAME FILES**, choose your base game (NSP or XCI), the 4.0.1 update (NSP), the Expansion
   Pass (optional) and your `prod.keys`, then **INSTALL**. A wrong file is named in one line (another game, another
   version, keys that can't open it).
3. **PLAY**.

Needs Windows 10 or 11 (64-bit), a CPU with AVX2 (Intel 4th gen / AMD Ryzen or newer), a graphics driver with Vulkan
1.3, 8 GB of RAM (16 GB or more makes the setup faster: it compiles more files at once) and about 2 GB free for the
setup (about 0.5 GB stays). The setup took 20 minutes on an 8-core Intel i7-11700F.

## The launcher

`mua3launcher.exe` (`launcher/launcher.cpp`) is drawn in the game's colours, all made in code (no game art): a cosmic
night with drifting nebula glows and stars, and six gem colours, one for each page. It has five pages, each usable with
the mouse, the keyboard or a controller:

- **PLAY**: checks the game files are set up, your NSPs are still where they were, and the PC version is built; PLAY
  saves the settings and starts the game.
- **GAME FILES**: picks your files and INSTALLs (extract, recompile, build) with a live log; REBUILD recompiles from
  the files already set up.
- **DISPLAY**: window mode and size, render resolution, upscaler, anti-aliasing, aspect ratio, frame rate 30 / 60 / 120,
  the FPS counter, volume and movie sound.
- **CONTROLS**: remap every Switch button and stick direction on the keyboard, and every button on a controller: click
  the binding, then press the key or button (Esc cancels; a key already in use swaps places). Rumble, its strength and
  background input are there too. The game's own menu (F1 > Controls) edits the same bindings.
- **ABOUT**: credits and the data folder.

Everything is saved in `larecomp.ini` next to the game, and the launcher only changes its own keys in it.

Default controls - keyboard: WASD left stick, numpad 8 / 5 / 4 / 6 right stick, K / J / L / I the Switch's A / B / X / Y,
Q / E L / R, 1 / 3 ZL / ZR, Enter Plus, Backspace Minus, arrows the d-pad, F / numpad 0 the stick presses. Controllers
(Xbox, PlayStation, Switch Pro and more, through SDL 3) map by position, as on the Switch. F1 or Esc (or Home) opens the
recomp's menu (Controls, Video, Audio, Quit); F11 or Alt+Enter switches fullscreen.

## How it works

The game's ARM64 code (`exefs`) is translated to C by `tools/gen.py` and compiled into `build/mua3game.dll`. The
runtime (`runtime/`) provides what the Switch would: a small Horizon kernel (threads, memory, sync), the OS services
(file system, HID, audio, time, ...), and NVN (the Switch's graphics API) on Vulkan, with a Maxwell shader translator.

| exefs module | What it is | Here |
|---|---|---|
| `main` | the game (Koei Tecmo engine, NintendoSDK 6.5.0) | recompiled: `generated/game` |
| `sdk` | NintendoSDK | recompiled: `generated/sdk`; its system calls and services are host code |
| `subsdk0` | Nintendo multimedia (movie playback) | not loaded: `movie::` is host code on FFmpeg (`runtime/hle/movie.c`) |
| `subsdk1` | NVIDIA glslc (runtime GLSL compiler) | recompiled: `generated/glslc` |

The game data (18.4 GB with the update) is not copied out: `tools/nsp_extract.py` writes `extracted/romfs.src`, and
the runtime reads the data in place from your NSPs (`runtime/romfs_nca.c`).

## Build from source (Windows)

1. MSYS2 with UCRT64 clang, make, SDL3, FFmpeg, Vulkan, shaderc (see LAS Recomp's `tools/setup_windows.ps1`), and
   Python 3.12 with a venv: `py -V:3.12 -m venv .venv` and
   `.venv/Scripts/python -m pip install unicorn==2.1.4 capstone==5.0.9 lz4==4.4.5 pycryptodome==3.23.0 pillow==13.0.0`
   (Pillow: the window and executable icon, made from the game's own icon in your dump at build time).
2. Extract your game (in Git Bash, `export MSYS_NO_PATHCONV=1` first):
   `.venv/Scripts/python -I tools/nsp_extract.py "<base>.nsp" "<your prod.keys>" extracted --update "<update>.nsp"
   --dlc "<expansion pass>.nsp"`. Keep the NSPs where they are: the game data is read from them.
3. In Git Bash with `export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"`:
   `make gen` (recompile the game code into `generated/`, a few minutes), then `make -j16` (about 10 to 20 minutes
   the first time) and `make dlls` (the runtime DLLs next to the programs, so they start from Explorer).
4. Run: `build/mua3launcher.exe` (or `tools/run.sh`; log: `mua3.log`). Steps 2 and 3 can also be run from the
   launcher's GAME FILES page. The settings are in `larecomp.ini` next to `extracted/`; the in-game menu (F1 or Esc)
   changes most of them too.

## Making a release

From the MSYS2 UCRT64 shell (with `nasm` and `zip` installed as well): `make deps-windows` (an LGPL-only FFmpeg),
`make gen-helper` (the extractor and recompiler frozen with PyInstaller), `tools/fetch_zig.sh` (the pinned compiler),
then `make dist`: `build/dist/MUA3-Recomp-v<VERSION>-Windows-x64.zip`. It checks that no game code, game files, keys,
GPL codec libraries or paths of the build machine are inside. Attach it to a GitHub release whose tag is `v<VERSION>`;
`recomplauncher.json` inside tells RecompLauncher how to set the game up (`mua3launcher --prepare`) and start it
(`mua3launcher --play`).

## Credits

- **LAS Recomp** by Peiikai and contributors (MIT License, `LICENSE.LASRecomp`): the recompiler (`tools/gen.py`,
  `lifter.py`, `nso.py`, `simd_c.py`, ...) and the runtime this project started from.
- Third-party components keep their own licenses (`third_party/`): Dear ImGui (MIT), SMAA (MIT), the ported audio
  renderer and shader decoder references (MIT, `third_party/ryujinx/LICENSE.txt`), fonts (SIL OFL 1.1 / DejaVu; the
  launcher's display face is Baloo 2), and [Recomp Achievements](https://github.com/elripalda/RecompAchievements) by
  Ripalda (`third_party/rl_achievements`: its own license, with rcheevos and cJSON under MIT). The release's bundled
  components, their licenses and their sources: [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md).
- Libraries: SDL 3, FFmpeg (LGPL, dynamically linked), Vulkan Memory Allocator, shaderc; the generator uses Capstone,
  Unicorn and python-lz4.
- [Switchbrew](https://switchbrew.org) documentation (NCA/PFS0/RomFS formats for `tools/nsp_extract.py`).

*MARVEL ULTIMATE ALLIANCE 3* is a trademark of Marvel and Nintendo; the game was developed by Koei Tecmo. This project
is not affiliated with Marvel, Koei Tecmo or Nintendo. The release's icon is original (`tools/make_original_icon.py`).
