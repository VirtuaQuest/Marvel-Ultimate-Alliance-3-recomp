# MUA3 Recomp — Linux / Steam Deck Preview

A community Linux port of [VirtuaQuest’s Marvel Ultimate Alliance 3 Recomp](https://github.com/VirtuaQuest/Marvel-Ultimate-Alliance-3-recomp), featuring the original SDL3/ImGui launcher interface.

## Included

- Native Linux launcher and game runtime.
- Dedicated `play-steam.sh` entry for Steam Game Mode, bypassing setup windows (awaiting Deck confirmation).
- Game file setup, display settings, controls, and launch options.
- Linux build scripts and Python dependency setup.
- Automatic SteamOS Distrobox setup when starting through `run.sh` or `run-linux.sh`.
- Bazzite/Fedora Toolbox build support.
- Audio timing changes intended to reduce robotic sound and playback delay.
- Source code and bundled runtime libraries.
- AMD 32-lane compute subgroup performance candidate with driver fallback. Build completed; Deck speedup remains unmeasured.
- Optional `play-steam-profile.sh` for GPU pass timing diagnostics.
- Runtime performance update: per-shader compute shared memory, two-thread movie colour conversion, one fewer movie frame copy (see STEAMOS.md). Deck speedup unmeasured.
- Bundled Vulkan Memory Allocator header and license.
- Steam Deck black screen workaround: on-demand synchronous shader compilation and display-session forwarding.
- Desktop launcher that opens only the program window during normal launches.
- Button icons that match your controller: Nintendo, Xbox, PlayStation, Steam Deck or Steam Controller (Settings > Controls > Button icons; see STEAMOS.md).

## Getting started

1. Extract the release into a writable folder.
2. Double-click **MUA3 Recomp.desktop** in Desktop Mode and choose **Trust and Launch** or **Execute** if asked. You can also run `./run.sh` from a terminal.
3. Open **GAME FILES** and select your own base game dump, **4.0.1 update**, optional Expansion Pass, and `prod.keys`.
4. Click **INSTALL** to prepare and compile the game.
5. Click **PLAY** after setup finishes.

On SteamOS, the start script creates a Fedora Distrobox and installs development dependencies automatically. First launch requires internet access and may show a password prompt. Distrobox and its container engine must already be installed. On other distributions, development dependencies are required for initial game compilation. See `START-HERE.txt` and `STEAMOS.md` for setup instructions.

## Requirements and status

- An x86-64 Linux PC with AVX2 support and Vulkan 1.3 graphics.
- Included executables were built on Fedora/Bazzite.
- The user confirmed the game launches on Steam Deck after the black screen workaround. This does not establish compatibility with every SteamOS version or full-game stability.
- First-use shader compilation may cause pauses.
- Audio changes are included for testing; remaining sync or distortion issues may occur.

**No game files, extracted assets, generated game code, encryption keys, or saves are included. You must supply your own game dump and keys.**

## Credits

Original project by **VirtuaQuest** and upstream contributors. This fork provides the Linux port and related launcher, setup, and runtime changes.
