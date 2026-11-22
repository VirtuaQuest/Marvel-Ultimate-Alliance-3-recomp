# SteamOS / Linux build

This folder contains the native Linux build of MUA3 Recomp. It builds a Linux ELF game runtime and launcher from the
same SDL3 and Dear ImGui sources used by the Windows version; the launcher keeps the same PLAY, GAME FILES, DISPLAY,
CONTROLS and ABOUT pages. The Windows release remains separate.

## Automatic Steam Deck setup

In Steam Deck Desktop Mode, open this folder and run `run.sh` (or `run-linux.sh`).
On SteamOS, the script automatically:

1. Opens Konsole when first-time setup needs a terminal.
2. Creates a Fedora 44 Distrobox named `mua3recomp-deck`.
3. Installs the compiler, Python and development libraries inside that container.
4. Opens the original launcher inside the container so INSTALL can complete all stages.
5. Reuses the prepared container on later launches.

Internet access is required for the initial download. A password prompt may appear.
Keep the setup terminal open until the launcher finishes. Failures are recorded in
`setup-linux.log`; game preparation continues to use `prepare.log`.
Builds started through this helper use two compiler jobs to limit memory use on Deck.

Distrobox and a working container engine must already be available. If Distrobox is
missing, update SteamOS or follow its [official Steam Deck guide](https://github.com/89luca89/distrobox/blob/main/docs/posts/steamdeck_guide.md).
The helper does not install packages on the SteamOS host. Start through the scripts
rather than opening `build/mua3launcher` directly. Run first-time setup in Desktop Mode.
The user confirmed the game launches on Steam Deck with the shader/display workaround below. Broader performance and compatibility testing is ongoing.

## Build and launch on Linux

Use a writable checkout location. The `run-linux.sh` helper checks the required tools and libraries, prints package
instructions for Bazzite/Fedora, Arch and Debian/Ubuntu, builds the launcher and opens its GUI:

```sh
./run-linux.sh
```

On Bazzite, use the Toolbox instructions printed by the helper so development packages stay in the container. The
project folder is shared with the host, and the GUI opens from the Toolbox session.

To prepare the local Python generator environment manually:

```sh
make setup-linux
```

Setup uses a separate `.venv-linux` environment with Python 3.12. If necessary it downloads that Python version
with uv; the system Python and any older `.venv` stay in place. Generator dependencies are installed as binary
wheels, avoiding a source build against a newer system Python.

Place your legally dumped base game, 4.0.1 update, optional Expansion Pass and `prod.keys` in a location you control.
Build and open the launcher:

```sh
make -j"$(nproc)" launcher
build/mua3launcher
```

Choose those files on **GAME FILES**, then press **INSTALL**. The launcher creates `.venv-linux`, extracts the files you
selected, recompiles the game code and builds `build/mua3recomp` plus `build/mua3game.so`. Press **PLAY** when setup
finishes. The game data stays in your NSP/XCI files; keep those files at their selected paths. Use `tools/run.sh` to
open the launcher GUI from a terminal and write its log to `mua3.log`.

The native build requires an x86-64 Steam Deck or Linux PC with AVX2, Vulkan 1.3, and the required runtime libraries.
It is not a Windows executable run through Proton: both the game host and the GUI launcher are compiled for Linux.

## Troubleshooting

- If Make cannot find `SDL3/SDL.h`, `vulkan/vulkan.h`, FFmpeg headers or `shaderc`, install the corresponding `-dev`
  packages for your distribution (or the packages above on Arch).
- If the runtime reports a Vulkan error, update SteamOS and check that the Deck is using its normal Mesa Vulkan driver.
- If game setup fails, read the live launcher log or `prepare.log`; the NSP/XCI files and keys must match the MUA3 4.0.1
  release described in the parent README.

## Missing Vulkan Memory Allocator header

The release now includes Vulkan Memory Allocator 3.3.0 and its MIT license in
`third_party/vma/`. The Makefile and dependency check include that directory,
so `vk_mem_alloc.h` no longer needs to be installed separately.

For an older release, copy the updated `Makefile`, `third_party/vma/`,
`run-linux.sh`, `tools/steamdeck_setup.sh` and `tools/build_linux.sh` into your
existing folder. Restart with `run.sh`, then choose **REBUILD** to reuse your
extracted files and existing compiler output.

## Starting by clicking

Double-click `MUA3 Recomp.desktop` in this folder. If KDE asks, choose
**Trust and Launch** / **Execute**. Keep the desktop file beside `run-linux.sh`
when moving the folder. The desktop shortcut requires Python 3.

Both `run.sh` and `run-linux.sh` also work from a terminal. Depending on your
file manager settings, clicking a `.sh` file may open an editor; choose
**Execute** or use the desktop launcher instead.
Normal desktop launches show only the program window. Startup errors are
saved in `launcher-start.log` and shown in an error dialog when KDialog or
Zenity is available. SteamOS first-time dependency setup may still open a
terminal for installation and password prompts. Updating a runtime-only Makefile no longer forces a GUI rebuild.

## Steam Deck black screen compatibility workaround

If the runtime loads RADV and the game library but reports zero draws and
hundreds of unfinished shader builds, the SteamOS launch helper now skips
eager shader preparation and compiles shaders synchronously on first use.
This avoids dropping required draws while shader jobs are pending. It may
cause pauses while new shaders compile; performance on Deck needs verification.
Skipped-draw diagnostics are enabled for follow-up logs.

The helper also explicitly forwards display-session variables when starting
the runtime on the host from Distrobox. Two different failures appeared in
the supplied logs: unavailable SDL video devices in earlier runs, and shader
startup delays with zero draws in later runs. These changes address both
paths. The user confirmed this update resolves the black screen on their Deck.
No game rebuild is needed for this script update. Close the game, replace
`tools/start_game_linux.sh`, reopen the launcher and select PLAY.

## Steam Game Mode

Finish INSTALL in Desktop Mode first, then add `play-steam.sh` from this folder
to Steam as a Non-Steam Game. Set the shortcut's **Start In** to this folder
and leave forced Proton compatibility disabled; this is a native Linux build.
For an existing Steam shortcut, change its **Target** to the full path of
`play-steam.sh` (keep the path quoted if it contains spaces).

The new entry starts the installed game directly and keeps its process attached
to Steam. It opens no setup launcher, terminal or helper dialog. Startup failures
are written to `game-mode.log`; runtime details remain in `lasrecomp.log`.
`run-linux.sh --game-mode` does the same thing. `run.sh` and `run-linux.sh`
otherwise always open the GUI, even if Steam session variables are inherited.

Continue using `MUA3 Recomp.desktop` in Desktop Mode for setup and settings.
This Game Mode handoff change needs confirmation on the Deck. The earlier
Desktop Mode black screen workaround is still included.

## Gameplay stays black with audio / cutscene output

The SteamOS launch helper now defaults to a conservative rendering profile:
GPU overlap and lean image usage are disabled, per-frame render-target clears
are restored, and movie frames use CPU color conversion. This isolates several
GPU output paths while keeping the existing on-demand shader workaround.
It can reduce performance. This profile is a workaround awaiting Deck confirmation,
not a confirmed root-cause fix. It does not change saves or user settings.

Replace `tools/start_game_linux.sh`, close the game, and start normally. No game
rebuild is needed. The `[linux-start] Deck conservative rendering` line in
`game-mode.log` confirms the profile was selected; `lasrecomp.log` should show
lean usage off, target clears on, and no `GPU overlap on` message. To compare
against the earlier behavior, use Steam launch options:
`MUA3_DECK_RENDER_SAFE=0 %command%`.

## AMD compute performance candidate

The runtime now requests 32-lane compute subgroups on AMD devices that report
support through Vulkan 1.3 subgroup-size control. This targets the translated
compute shaders that previously ran in the Deck driver's 64-lane subgroups.
It leaves the confirmed conservative rendering profile in place. Pipeline
creation falls back to the driver's subgroup size if the request is rejected.
This is a compiled performance candidate; no Deck FPS improvement has been
measured yet. It does not change the generated game library or saved games.

The log records `AMD 32-lane compute subgroups: on` when available. For a
comparison, set Steam Launch Options to `LARECOMP_VK_WAVE32=0 %command%`.
New pipeline variants may compile on the first run; retain the cache folder.

For lower GPU load, choose **Bilinear** upscaling in DISPLAY. The Deck launch
helper now applies the native display and 1x rendering defaults described below.

If performance is still poor, temporarily point the Steam shortcut at
`play-steam-profile.sh`, play through a cutscene and into gameplay, then close
the game and share `lasrecomp.log`. This optional diagnostic entry collects
GPU timing by pass every 120 frames. Profiling adds overhead; return to
`play-steam.sh` afterward for normal play.

## Steam Deck screen resolution

SteamOS startup now sets the game window to **1280×800**, fullscreen, with
**1× internal rendering**. On the integrated GPU, 1× is 1280×720 game rendering
fitted to the Deck screen; it does not request 4K rendering. The launch helper
updates window width, height, fullscreen, exclusive fullscreen, internal
resolution, upscaler (Bilinear) and anti-aliasing (Off) in `larecomp.ini`. Before its first change to an existing file, it
saves `larecomp.ini.before-deck-resolution`. Other settings stay as chosen.

For a docked display or manual resolution control, disable this automatic
setting with Steam Launch Options: `MUA3_DECK_NATIVE=0 %command%`.

## Repairing the GUI start scripts

Use the complete startup repair ZIP when updating an older install. It includes
`tools/launch_linux.sh` and the other required helpers; copy the entire tools
folder from the patch, not just the two root scripts. It does not contain game
files, keys, settings or saves.

`run.sh`, `run-linux.sh` and `MUA3 Recomp.desktop` open the setup GUI.
`play-steam.sh` starts the installed game directly for Game Mode. Startup no
longer redirects based on inherited Steam/Gamescope variables. Existing ready
Distrobox containers are checked and reused even if the readiness marker was
not saved in the current desktop session.

### Actual render-scale cap

The latest supplied log showed a 1280×800 swapchain but `resolution 2x` and
`render scale 2.000x2.000`, equivalent to 3840×2160 scene targets. Window size
alone did not prevent 4K rendering. The SteamOS helper now also sets
`LARECOMP_SCALE=0.6666666667`, bounding scene targets to 1280×720 independently
of an old internal-resolution selection. This cap is disabled together with
the native defaults by `MUA3_DECK_NATIVE=0`. The next runtime log should show
a render scale near `0.667x0.667`, with the 1280×800 swapchain.

## Deck performance defaults and optional compressed-image profile

The supplied 720p profiling log showed SMAA enabled and gameplay around 18–27
FPS, with GPU work around 42–44 ms in heavier moments. The native Deck
settings helper now applies Bilinear upscaling and disables SMAA alongside
the 1280×800 window / 1280×720 scene defaults. This preserves the known
rendering workaround. No frame-rate improvement is claimed before a Deck run.

`play-steam-fast.sh` is an optional performance candidate. It re-enables lean
image usage, which allows AMD DCC compression for compatible color targets.
GPU overlap remains disabled, render-target clears remain enabled, and movie
conversion remains on the CPU. This isolates one of the conservative
rendering switches rather than reverting the entire black-screen workaround.
It may restore an earlier rendering problem; use `play-steam.sh` again if so.
No game rebuild is required to compare these two launch scripts.

Use `play-steam.sh` or the optional `play-steam-fast.sh` for normal play.
`play-steam-profile.sh` is for collecting timing data and adds profiling
overhead. Video stutter still needs further diagnosis; no decoder failure
appeared in the supplied log.

## Runtime performance update (2026-11-21)

This runtime update (`build/mua3recomp`, SHA256 `df60d7af…c2e`) needs no game
regeneration: the game ABI and generated library are unchanged.

- **Compute shared memory sized per shader.** The translator previously
  reserved 48 KB of workgroup shared memory for every compute shader that used
  any. It now reads the allocation from the NVN control section (for example
  512 B, 1 KB, 3 KB) and specializes the GLSL array to it. On the Deck's RDNA2
  GPU this lets more workgroups stay resident. Unknown metadata keeps 48 KB.
  Rollback: `LARECOMP_VK_SHARED_SIZE=0`.
- **Movie colour conversion on two threads.** NV12→RGBA conversion keeps the
  same bilinear output but uses FFmpeg slice workers (about half the CPU time
  per frame in tests). `LARECOMP_MOVIE_CONVERT_THREADS=1..4` (default 2; 1 is
  the old single-thread path). The log prints `[movie-perf]` timings every
  120 frames/packets.
- **One fewer full-frame copy per movie frame.** Queued movie uploads hand
  their buffer to the texture instead of copying it again (8.3 MB per 1080p
  frame).

Deck FPS gains are not measured yet. Please send `lasrecomp.log` from the same
scene before/after, ideally once with `play-steam-profile.sh`. If a picture
problem appears, add `LARECOMP_VK_SHARED_SIZE=0` to the Steam launch options
(`LARECOMP_VK_SHARED_SIZE=0 %command%`) and report it.

## Deck update 2 (2026-11-21): movie decoding and branch-stack slots

Built and tested on the Deck (distrobox `mua3recomp-deck`):

- **Movies:** the Fedora `ffmpeg-free` libraries have no native H.264 decoder,
  so movies fell back to libopenh264 (24–35 ms per 1080p frame). The Deck's
  build container now uses RPM Fusion `ffmpeg-libs` (same sonames), re-bundled
  into `build/lib` with `tools/bundle_runtime_linux.py`. Decode now averages
  about 4 ms per frame. Backup: `build/lib.before-rpmfusion-2026-11-21`.
  This is a local, personal-use change. Patent-encumbered codecs are not part
  of the public release.
- **Translator:** SSY/PBK branch stacks use fixed slots per nesting level
  when the depth is known at translation time (`static_stacks` in
  `runtime/nvn/maxwell/translate.c`), including BRX jump tables bounded by the
  compiler's clamp. The largest compute shader went from 385 KB to 189 KB of
  code and from 256 to 192 VGPRs. Heavy-fight fps is unchanged (about
  14–20 fps); two other compute shaders still hit 256 VGPRs. Off switch:
  `LARECOMP_STATIC_STACKS=0`.

## v8.2 (2026-11-22): button icons that match your controller

The game's own button icons (A/B/X/Y, L/R/ZL/ZR, +/−) are redrawn when they're
loaded to show the controller you're using: **Nintendo, Xbox, PlayStation,
Steam Deck or Steam Controller**. No game art ships; the recomp edits the
game's icons from your own dump as they load.

- Settings > Controls > **Button icons**: Auto (default) / Nintendo / Xbox /
  PlayStation / Steam Deck / Steam Controller. `larecomp.ini`:
  `prompts = auto|nintendo|xbox|playstation|steamdeck|steamcontroller`.
- Auto on a Steam Deck in Game Mode picks the Deck style. Remapped buttons show
  the button they're bound to.
- A change of style shows when the game next loads its icons (after a loading
  screen, or on restart).
- Diagnostics: `LARECOMP_LOG_PROMPTS=1` logs every texture upload's shape;
  `LARECOMP_PROMPTS=xbox|playstation|steamdeck|steamcontroller|nintendo` forces
  a style for a test.
- Code: `runtime/nvn/mua3_prompts.c` (icons from
  `UI/screenlayout/80_button_icon/80_button_icon.ext.kslt`), hooked in
  `runtime/nvn/texpatch.c` and the buffer→texture copy in
  `runtime/nvn/vulkan/exec.c`; controller detection in `runtime/hle/input.c`.
