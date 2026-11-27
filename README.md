# Changes in this fork (Tetracell/next-ray-engine)

This fork branches from [Ozicom23/next-ray-engine](https://github.com/Ozicom23/next-ray-engine) at `d67044cf6`. Below are
all the changes made since then, oldest first. Everything below the line further down is the original README, unedited.

### Build

- **Visual Studio 2026 compiler (v145)** (`a8ccb6b26`): braces one `case` in `SetParam` that declared a local variable
  (error C2360), so the solution builds with the v145 toolset.

### Upstream fixes and Lua exports picked ahead of the full merge (branch `upstream-script-exports`)

Cherry-picked from [themrdemonized/xray-monolith](https://github.com/themrdemonized/xray-monolith). GAMMA scripts call these:

- `f4a054688`: Lua get/set for the HUD fire bone and fire position (silencer).
- `b7a8621f1`: `level.remove_hud_motion_cam_effectors`.
- `f0447b360`: `npc:make_enemy_visible(enemy)`, which puts an enemy into an NPC's "seen" memory.
- `02dccb744`: `level.set_rain_wetness`.
- `978c98519`: fixes a crash, a double free of the `$level$` path when levels load in parallel.

### Full merge of upstream xray-monolith (branch `monolith-merge`)

- **`1d9a0cba8`:** merges `all-in-one-vs2022-wpo-mt` at `b58698d0a` (2026-11-22), about 650 upstream commits from 2026-01
  to 2026-11. Highlights:
  - scripted attachments, 3D inventory icons and inverse kinematics;
  - spawn overlays and level packs;
  - the shader bus;
  - Lua callback priorities and many new exports;
  - AI fixes and many stability fixes;
  - RenderDoc integration (`rdoc_capture`).
- **44 files conflicted, all resolved by hand.** The main decisions:
  - **File system (`LocatorAPI`):** upstream's locking is the base, with neXt-Ray's parallel file index, prefetching and
    level preloading put back on top. A plain merge would have nested two locks and frozen the game.
  - **Lua callbacks (`axr_main.script`):** neXt-Ray's callback cache is renamed `callback_list` so it doesn't clash with
    upstream's new `callback_order`.
  - **Weapon-attached UI:** drawn where upstream draws it without an upscaler, and after DLSS/FSR (neXt-Ray's place) with one.
  - **Shader cache:** uses upstream's new layout, so **the first start after updating recompiles every shader**.
- **`669376de8`:** a DX12 build fix. The DX12 exe sends GPU markers through its own wrapper, because upstream's RenderDoc
  marker path is DX11 only.
- **The exes and `db/mods/00_modded_exes_gamedata.db0` must be updated together:** the merged scripts call the new engine
  functions.

### Upscaler (DLSS/FSR) fixes

- **Night vision now covers detector and wrist-device screens** (`ba8b7eb89`):
  - **The problem:** with an upscaler on, the UI drawn on items in your hands (detector screens, the WearableDevices watch
    and Promin) was drawn after night vision, the gas mask and the fake scope. It showed on top of them, untinted.
  - **The fix:** while one of those overlays is on, that UI is drawn before them. It is still drawn after the upscaler,
    so it stays sharp.

### Picked from [brodrigz/xray-monolith](https://github.com/brodrigz/xray-monolith) (branch `fork-upscaling-picks`)

- **WearableDevices screen glass** (`bca467e59`, after `e92194607`): with an upscaler on, the glass on the Promin, Vektor
  and Metro watch screens is drawn over the screen instead of under it.
- **Motion vectors for moving rigid objects** (`bca467e59`, from `353e4bf48`): props, doors and dropped items get their
  previous-frame position. Before, they reported motion as if they came from the world's origin, which caused smearing
  and ghosting under DLSS/FSR and SSS TAA.
- **Sampler states** (`c22db1f86`, after `777c5220d`): switching the upscaler's texture LOD bias at the start and end of the
  scene, every frame, no longer rebuilds every sampler state.
- **Memory** (`b1e6e7988`, from `6a93fc679`): a block that `realloc` shrank in place and then grew again could hand back
  stale bytes where zeros were promised. That tail is now cleared.

### Texture streaming is off by default

Texture streaming is one of neXt-Ray's biggest features (see [Texture streaming](#texture-streaming) further down): world
textures load small first, and each one loads at full size in the background the first time it's drawn. At full texture
quality that uses about 2.5 times less video memory. **In this fork it's turned off by default for now, because it's
causing crashes:**

- **DX12 (`AnomalyDX12AVX.exe`):** a crash while loading a save, reproducible with GAMMA. The crash is heap corruption
  (`0xc0000374` in `ntdll.dll`): something writes into memory it doesn't own. Windows ends the game on the spot, so the
  engine's crash handler never runs, and there's no crash report, only an entry in the Windows event log. The bad write
  hasn't been found yet. The streaming code and the DX12 texture loader were read without finding it, so the next step is
  a crash dump.
- **DX11 (`AnomalyDX11AVX.exe`):** streaming is also the main suspect, not proven, for crashes inside NVIDIA's driver
  (`nvwgf2umx.dll`), on the driver's own worker threads, a few hundred frames after a save loads. Streaming creates
  full-size textures on a background thread while the game renders, which the stock engine never does.

Streaming saves video memory. It doesn't make the game faster. With streaming off, the game needs as much video memory
as GAMMA's own engine does: the comparison further down measured 9.89 GB at full texture quality in one location. On a
card with less than that, lower the texture quality, or turn streaming on and accept the risk.

**To turn it on anyway:** open the console (`~`), enter `r4_tex_stream 1`, then load a save. The setting is read when
textures load, so it only takes effect at the next level load. It's saved in `appdata\user.ltx`. To turn it off again,
enter `r4_tex_stream 0` and load a save. If you get a crash with it on, a report is very welcome. A crash dump helps
most: Windows can be set to keep one with
[WER LocalDumps](https://learn.microsoft.com/windows/win32/wer/collecting-user-mode-dumps).

The default changed in `dx10SH_Texture.cpp` (`ps_r4_tex_stream = 0`). Your own `user.ltx` overrides it, so if you used
neXt-Ray before and never changed the setting, it may still say `r4_tex_stream 1` there.

### Known issues

- **Texture streaming crashes:** see [above](#texture-streaming-is-off-by-default); off by default.
- **Red dot / reflex sights show no reticle without a shader override.** Upstream's "Fix SSS Bloom" (`a07386981`) removed
  the lens rendering errors, but not this.
- **Night vision can flicker** between a soft, dark look and a bright, sharp one with DLSS/FSR on. Workaround:
  `r4_upscaler_nvg_res 0`.

---

*Everything below this line is the original neXt-Ray README, unedited.*

---

<p align="center"><img src="docs/splash.png" width="500" alt="neXt-Ray x64 / DX12"></p>

<p align="center"><a href="https://ko-fi.com/ozicom23"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="Support me on Ko-fi"></a></p>

# neXt-Ray Engine

neXt-Ray is a new engine for S.T.A.L.K.E.R. Anomaly and [G.A.M.M.A.](https://github.com/Grokitach/Stalker_GAMMA)
It replaces the game's exes and leaves your modlist as it is. You get more fps, fewer stutters, faster loading and
lower memory use.

It is built on [Modded Exes](https://github.com/themrdemonized/xray-monolith) by themrdemonized, the engine GAMMA
ships with, so everything Modded Exes has is still here. The fork point is Modded Exes MT-TEST 2026.5.15, the engine
of GAMMA 0.9.5 (branch `all-in-one-vs2022-wpo-mt`, commit
[`3d4dcca5`](https://github.com/themrdemonized/xray-monolith/commit/3d4dcca52f842c797099da3e6206e53dab7e7d79)).

## Compatibility

- **Mods** (scripts, configs, textures, shaders): anything that works with GAMMA should work with neXt-Ray, since it
  is the same game with a different engine.
- **Mods that ship their own exes** (for example xrRazom co-op): these won't work. They need their own engine, and
  installing them would replace neXt-Ray.

## Install

neXt-Ray needs GAMMA 0.9.5 as the GAMMA launcher installs it. On the Steam Deck, follow the
[Steam Deck guide](docs/steam-deck/README.md) instead (GAMMA and neXt-Ray from scratch).

1. Download the latest zip from [Releases](../../releases).
2. Move two things out of your `Anomaly` folder into a backup folder outside it: `bin\AnomalyDX11AVX.exe` and the
   `gamedata` folder. GAMMA keeps its engine's scripts and shaders loose in `gamedata`, and loose files override
   neXt-Ray's.
3. Extract the zip into your `Anomaly` folder. It adds `bin\AnomalyDX11AVX.exe`, `bin\AnomalyDX12AVX.exe`, the DLSS
   and XeSS DLLs and the engine's gamedata (`db\mods\00_modded_exes_gamedata.db0`).
4. Add the DX12 exe to Mod Organizer: the executables list (the gears icon) > Add > Title `Anomaly (DX12-AVX)`,
   Binary `Anomaly\bin\AnomalyDX12AVX.exe`.
5. Start `Anomaly (DX12-AVX)` from Mod Organizer. DX11 is `Anomaly (DX11-AVX)`, as before.

Your saves, settings and modlist stay as they are. Only AVX exes are included.

**After a GAMMA update:** it can put GAMMA's own engine and its `gamedata` folder back and remove the DX12 entry from
Mod Organizer. Delete the new `gamedata` folder and do steps 3 and 4 again.

**Uninstall:** delete `bin\AnomalyDX12AVX.exe` and `db\mods\00_modded_exes_gamedata.db0`, then put the backed-up
exe and `gamedata` folder back.

neXt-Ray is not part of GAMMA: please don't report its problems to GAMMA's support, open an issue here instead.

## Features

### DirectX 12
<!-- screenshot -->
A new DX12 renderer with HDR. It is the main renderer: new features are made for it first. DX11 is still included
for older PCs and gets some of the updates, but not all of them (no frame generation or XeSS, for example).

### Upscaling
<!-- screenshot -->
NVIDIA DLSS 4, AMD FSR 3.1 and Intel XeSS.

**DLSS on Linux (Proton):** add `PROTON_ENABLE_NVAPI=1` to the game's Steam launch options
(`PROTON_ENABLE_NVAPI=1 %command%`). Without it DLSS option isn't offered in game.

### Frame generation
<!-- screenshot -->
FSR and XeSS frame generation.

### Texture streaming
<!-- screenshot -->
Textures load when they are needed instead of all at once. At full texture quality the game uses about 2.5x less
video memory; how much depends on the location (9.89 GB against 3.63 GB in the comparison below).

### General optimization
<p align="center"><img src="docs/img/engine_comparison.png" width="900" alt="The same place on GAMMA's engine and on neXt-Ray, with the performance overlay of each"></p>

The same place with the same settings (1728x1080, no upscaler, no frame generation) on GAMMA's engine and on
neXt-Ray's DX12 renderer:

| | GAMMA engine | neXt-Ray | |
|---|---|---|---|
| FPS | 93 | 114 | +23% |
| 1% low FPS | 50 | 63 | +26% |
| CPU time per frame | 9.74 ms | 6.74 ms | -31% |
| Draw calls | 4904 | 2431 | -50% |
| Triangles | 3.51 M | 2.31 M | -34% |
| VRAM | 9.89 GB | 3.63 GB | -63% |

For the same picture neXt-Ray needs half the draw calls (hidden objects are skipped), a third less CPU time per
frame and about a third of the video memory.

GPU occlusion culling, tree batching, cached shadows, multithreaded culling, background Lua garbage collection, a
faster LuaJIT and A-Life work spread over several frames. The heaviest scripts and shaders of GAMMA's mods run
cheaper versions, without touching the mods' files. The image stays the same.

### Faster game startup, level transitions (fade to black) and loading
<p align="center"><a href="docs/img/transition_comparison.mp4"><img src="docs/img/transition_comparison.gif" width="720" alt="The same level transition on GAMMA's engine (28.7 s) and on neXt-Ray (12.8 s)"></a></p>

Up to ~2.5x faster loading on level transitions, with no loading screen. Above: the same transition on GAMMA's
engine (28.7 s) and on neXt-Ray (12.8 s), from the fade to black to the new level on screen.

Starting the game is about 2x faster too: from launch to a loaded save takes ~150 s on GAMMA's engine and ~76 s on
neXt-Ray on the same machine (launch to the main menu 107 s against 56 s, loading the save 42 s against 20 s).

### Performance overlay
<p align="center"><img src="docs/img/performance_overlay.png" width="720" alt="The detailed performance overlay on a Steam Deck: FPS, frame times, CPU, GPU and memory on the left with the GPU passes below, the Lua time of each mod and script loading times on the right"></p>

A built-in overlay with FPS, frame times, CPU and GPU time and video memory (`perf_overlay compact`, `graph` or
`detailed` in the console). `perf_overlay_mods` adds the Lua time of each mod per frame and how long each took to load.

### Steam Deck preset
<p align="center"><a href="https://www.youtube.com/watch?v=iWOeuRe3ifY"><img src="docs/img/steam_deck_video.jpg" width="720" alt="Video: STALKER GAMMA with neXt-Ray on a Steam Deck (opens on YouTube)"></a></p>

<p align="center"><sub>Actual performance and quality are higher: Steam's recording takes a hit on both.</sub></p>

One switch for the Steam Deck (Options > Video > Advanced) that lowers the settings whose loss is hard to see on its
800p screen: smaller shadow maps, ambient occlusion and reflections at half resolution, fewer shadow and indirect
light samples, 8x anisotropic filtering. Turning it off puts your previous values back.

### Gamepad (in progress)
Every button can be rebound in Options > Modded Exes > Controls > Gamepad.

<p align="center"><img src="docs/img/gamepad_wheel.png" width="720" alt="The radial wheel in game: weapon and device slots around the ring, mod actions on the buttons listed beside it"></p>

<p align="center"><img src="docs/img/gamepad_controls.png" width="900" alt="The default gamepad layout: what each button does pressed, held, tapped twice and with LT held, and the controls in menus"></p>

## More
- [Features](docs/FEATURES.md): the full list, with console commands and defaults

## Remote console

For scripted tests: start the game with `-dbg -remote_console`. It runs the commands written to
`appdata/remote_cmd.txt` (one per line) and writes its state to `appdata/remote_status.txt`. Extra commands:
`rc_wait <s>`, `rc_wait_level <s>`, `rc_clear`, `rc_silent_level_changers 1`. `tools/remote_console.sh` wraps it
(launch, load a save, send commands, time a level transition).

## License

The X-Ray engine source is GSC Game World's, under its [license](License.txt): non-commercial use only. Third-party
code keeps its own license (`src/3rd party`).
