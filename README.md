# Blur Live

Real-time visual enhancement for Windows games, applied **externally**. Blur Live captures a game window with
Windows Graphics Capture and processes every frame on the GPU:
* **Blur-style motion blur:** a motion-compensated shutter.
* **A suite of 25 independent filters:** sharpening, colour, tone, grading, LUTs, effects and accessibility.

The result is shown in a click-through overlay exactly on top of the game, or in a window or on another monitor.
Blur Live never injects into, hooks, or reads the memory of the game.

Inspired by [f0e/blur](https://github.com/f0e/blur) (offline). See [docs/ORIGINAL_BLUR_ANALYSIS.md](docs/ORIGINAL_BLUR_ANALYSIS.md)
for how that project works and what was (and wasn't) carried over.

## How it works

```
 game window ──WGC──▶ pipeline thread (D3D11, one immediate context)
                       ├─ motion-blur engine
                       │    copy frame into 16-slot history ring
                       │    (blur on) luma pyramid → block matching → shutter integral over [now−S, now]
                       │    (blur off) frame passes straight through: motion analysis is skipped
                       ├─ filter chain (skipped entirely when no filter is enabled)
                       │    [denoise pass] → [luma pyramid 1/2…1/16] → [bloom chain] → ONE uber pass
                       └─ present: DirectComposition overlay / flip-model window (+ 30 fps preview copy)
 UI thread (Dear ImGui, own D3D11 device) ◀── snapshots / settings / shared preview texture ──▶ pipeline thread
```

### Motion blur

* **Shutter semantics match Blur:** `shutter = blur amount / reference fps` (Blur's `blur_amount` / `blur_output_fps`).
  The default is 1.0 / 60 fps = 16.7 ms, independent of the game's frame rate.
* **Weighting:** equal, gaussian, gaussian (symmetric), pyramid, ascending, descending, vegas, custom.
* **Robustness:**
  * Per-block motion confidence: areas where motion can't be estimated stay sharp.
  * Scene cuts are detected on the GPU.
  * Timing is based on timestamps, so variable frame rates work.
  * Pauses are detected and reset the history.
  * When the game stops sending frames, the last frame is re-shown sharp.
  * Frames that share a compositor timestamp replace the newest history entry instead of resetting it.

### Filter suite

All 25 filters start **off**. Each one has:
* its own toggle, strength/main slider, defaults and reset button;
* a description;
* settings that are saved in presets.

Turning a filter off removes its effect completely and keeps its values.

| Category | Filters |
|---|---|
| Detail & clarity | **Sharpen** (contrast-adaptive, luma-only, intensity / fine detail / edges / radius / noise threshold / halo suppression), Detail enhancement, Clarity, Local contrast, Denoise (bilateral) |
| Colour | Exposure (linear light, highlight roll-off), Brightness, Contrast, Gamma, **Saturation** (off unless you enable it), Vibrance, Temperature & tint, RGB channels, Selective colour (8 hue bands × hue/sat/lightness) |
| Tone | Shadows & highlights (halo-free local mask), Levels (black/white points), Lift / gamma / gain (with tints) |
| Grading & LUT | Colour grading (split toning + ambient tint), 3D LUT (`.cube`, trilinear) |
| Effects | Motion blur (engine), Bloom, Vignette, Film grain, Chromatic aberration |
| Accessibility | Colour-vision assistance (Machado 2009 daltonization, protan/deutan/tritan, correct or simulate), Colour focus (boost one hue range, optionally mute the rest) |

**Master switch** (Ctrl+Alt+F): bypasses every effect, including motion blur, while capture keeps running.
**Compare** (Ctrl+Alt+C): shows the untouched capture left of a movable split line and the processed image on the
right, in the output and in the live preview.

### Colour accuracy

* **Linear-light work:** exposure, white balance, RGB gains, bloom and colour-vision use exact piecewise sRGB↔linear
  conversions.
* **White balance:** Bradford chromatic adaptation along the black-body locus, with luminance preserved.
* **Perceptual colour work:** saturation, vibrance, selective colour and colour focus work in **OkLCh** (perceptual
  chroma and hue), with hue-preserving gamut compression instead of clipping.
* **Tone curves:** contrast, brightness and levels are designed not to clip at neutral.
* **Neutral settings are exact:** with every filter enabled at its neutral values the output differs from the input
  by **0/255** (tested).
* **Dither:** a ±1 LSB temporal dither is added only when a colour/tone filter actually changes the image, to prevent
  banding in 8-bit output.

### Presets

Presets store every filter plus the motion-blur look:
* create, save, save as, rename, duplicate, delete, import and export (`.blpreset`, plain text);
* stored in `%APPDATA%\BlurLive\presets`;
* built-in presets are read-only templates: tweak one and use **Save as**.

Built-ins:

| Preset | What it does |
|---|---|
| Original | Everything off. |
| VALORANT Competitive | Sharpen, halo-free shadow lift, light clarity, light vibrance. No blur, bloom, grain, CA or vignette. About 0.19 ms GPU at 1080p. It's a visual preference, **not** an aim advantage. It never activates automatically. |
| Cinematic Motion | Blur's classic 1/60 s blur, a gentle grade and a vignette. |
| Vivid & Crisp | Vibrance (saturation stays off), contrast and detail. |
| Bright & Clear | Exposure, shadow lift and local contrast for dark scenes. |
| Colour-blind assist (deutan) | Colour-vision correction for deutan vision. |

## Measured results (RTX 5070, i5-12400F, Windows 11, 1080p 280 Hz)

All figures come from the test suite or the app's own instrumentation, not estimates. "GPU time" is shader execution
time (Direct3D timestamp queries). It is **not** end-to-end latency, which is reported separately.

**GPU time per 1080p frame:**

| Configuration | Filters | Motion blur engine | Total |
|---|---|---|---|
| Everything off (pass-through) | 0 ms (chain skipped) | 0.035 ms | **0.035 ms** |
| VALORANT Competitive preset | 0.186 ms | 0.035 ms | **0.22 ms** |
| Common: sharpen + vibrance + contrast + shadows | 0.166 ms | 0.035 ms | **0.20 ms** |
| All 25 filters (incl. denoise, bloom, 33³ LUT) | 0.355 ms | 0.036 ms | **0.39 ms** |
| All 25 filters + motion blur (High, game at 240 fps) | 0.354 ms | 0.367 ms | **0.72 ms** |
| Motion blur only, High (48 samples), game at 60 / 240 fps | n/a | 1.12 / 1.15 ms | |
| Motion blur only, Medium / Ultra at 240 fps | n/a | 0.77 / 1.91 ms | |

At 280 fps a frame lasts 3.57 ms, so even the full stack plus motion blur uses about 20% of a frame of GPU time.

**Live pipeline (real WGC capture of a window rendering at 280 fps, 960×540, filters on):**

| Measurement | Result |
|---|---|
| Capture / output rate | 280 fps in / 280 fps out |
| Frame arrival → Present() returned (CPU, measured) | 0.13–0.25 ms |
| Added display latency (estimate from DirectComposition/DXGI frame statistics) | 3.57–3.61 ms ≈ one refresh at 280 Hz |
| Frame pacing (present-to-present) | median 3.55 ms, sd 0.20–0.27 ms, 99th percentile 4.1–4.3 ms |
| Dropped frames | 0–1 per run |

Not yet measured: a real game (e.g. VALORANT) running at the same time. GPU contention depends on the game. The
Performance page shows all of these numbers live while you play: blur, filter and total GPU time, GPU utilisation
(system and per-process), dropped frames, latency and frame pacing.

## Requirements

* Windows 11 (Windows 10 1903+ works with reduced features: yellow capture border, no `MinUpdateInterval`).
* A DirectX 11 GPU (feature level 11.0+).
* To build: Visual Studio 2022 or **VS 2022 Build Tools** with the *Desktop development with C++* workload
  (MSVC v143, Windows 11 SDK, CMake + Ninja are included). No other dependencies: Dear ImGui 1.91.9 (MIT) is vendored in
  `third_party/imgui`.

## Build

From PowerShell in the project folder:

```bash
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

Other options: `-Config Debug`, `-Clean`, `-Test` (build and run the tests), `-Run` (build and launch).
Outputs: `build\Release\BlurLive.exe` (standalone, no runtime DLLs needed) and `build\Release\BlurLiveTests.exe`.
HLSL shaders are compiled at build time with `fxc` (warnings are errors) and embedded in the executable.

## Run

```bash
.\build\Release\BlurLive.exe
```

1. Start your game in **borderless windowed** (or windowed) mode.
2. **Sources** → select the game → **Start capture** (or double-click it).
3. **Filters** → pick a preset or switch on individual filters. Every change applies live.
4. Click back into the game. The overlay covers it exactly, and keyboard and mouse still go to the game.

Global hotkeys:

| Hotkey | Action |
|---|---|
| Ctrl+Alt+B | Motion blur on/off |
| Ctrl+Alt+F | All effects on/off (master) |
| Ctrl+Alt+C | Compare view on/off |
| Ctrl+Alt+S | Start/stop capture |

Command line:

| Option | Effect |
|---|---|
| `--preset "<name>"` | Apply a preset at launch |
| `--page <dashboard\|sources\|filters\|blur\|output\|performance\|settings\|help>` | Open that page |
| `--capture "<window title substring>"` | Start capturing that window |
| `--output <overlay\|window\|monitor>` | Output mode for this launch only |

Settings: `%APPDATA%\BlurLive\settings.ini`. Presets: `%APPDATA%\BlurLive\presets\`. Log:
`%APPDATA%\BlurLive\blurlive.log`.

## Tests

```bash
.\build\Release\BlurLiveTests.exe
```

**37 tests, 2,528 checks**, all passing on the machine above.

* **Motion blur (unchanged behaviour, with one fix):**
  * motion-estimation accuracy, continuous energy-conserving streaks;
  * scene cuts, pauses, duplicate compositor timestamps, resize/crop;
  * 1080p benchmarks.
* **Filters:**
  * registry defaults: everything off, saturation off, and no built-in preset enables saturation;
  * pass-through costs nothing when every filter is off;
  * every filter is exact (0/255) at neutral values, individually and all together;
  * toggling one filter doesn't change any other and restores the image when turned off;
  * sharpening makes edges crisper (+36% gradient) with at most about 5/255 overshoot and no noise amplification;
  * no dark halo next to bright objects;
  * saturation keeps greys grey and preserves hue (< 6°), and white balance is correct;
  * contrast and brightness don't clip;
  * the compare view shows the exact original;
  * presets round-trip every parameter of every filter exactly (save, rename, duplicate, import, export, delete,
    built-in protection);
  * LUT parsing, an identity LUT is exact, and a missing LUT is handled;
  * the full stack and every built-in preset render sanely;
  * 1080p benchmarks.
* **Capture and pipeline:**
  * real WGC capture with stop/restart, closed-window detection and window enumeration;
  * overlay visibility (checked by capturing the monitor) and click-through;
  * full pipeline in overlay and window modes, including:
    * filters switched on **live without restarting capture**;
    * the master switch bypassing the chain;
    * the game closing mid-capture.

The integration tests briefly open small test windows. Pass a name filter to run a subset, e.g.
`BlurLiveTests.exe filters`.

Developer utilities:

| Command | Purpose |
|---|---|
| `BlurLiveTests.exe --demo-window 30` | An animated stand-in "game" |
| `--snap "<title>" out.png` | WGC screenshot of a window |
| `--snap-screen "<title>" out.png` | What is on screen over that window, overlay included |

## Latency: what is and isn't possible

An external program **cannot modify the pixels a game hands to the display** without hooking the game, which Blur
Live deliberately doesn't do.

The path a frame takes:
1. The game's frame is composed by Windows.
2. It is delivered to Blur Live.
3. It is processed: about 0.04–0.7 ms of GPU time depending on what's enabled.
4. It is presented again in the overlay.

Expect roughly **one extra display refresh** (about 3.6 ms at 280 Hz) on top of processing. The filters add almost
nothing beyond that. Literal zero latency is not possible with external capture.

Optimisations in place:
* GPU-only processing, with no CPU readbacks in the frame path.
* Statistics read back asynchronously.
* The filter chain is skipped when idle.
* Motion analysis is skipped while blur is off.
* All per-pixel filters run in one pass.
* Intermediate targets are allocated only for filters that need them.
* Driver frame latency is 1.
* Flip-model presentation, with tearing allowed in window/monitor modes.
* The UI runs on its own device and thread, capped at 30–120 fps.
* The preview copy runs only while the preview is visible.

| Mode | How | Limits |
|---|---|---|
| **Game overlay** (default) | Borderless, top-most, click-through, never-activated window via DirectComposition, aligned to the game's client area. Opaque: it *replaces* what you see. | Needs borderless/windowed games. Auto-hides when the game isn't focused. |
| **Window** | Normal flip-model window (tearing allowed) | For testing / recording |
| **Monitor** | Borderless fullscreen window on a chosen monitor | Ideal for a second display |

## Limitations

* **Exclusive fullscreen:** capture may be black or stop, and nothing can draw above it. Use borderless windowed.
* **Anti-cheat / protected content:** some games or DRM windows block capture (black frames, which the app detects).
  Blur Live does not try to get around this.
* **HDR:**
  * Capture and processing are 8-bit SDR (`B8G8R8A8`). HDR games are tone-mapped by Windows and the output is SDR.
  * An HDR path would need FP16 capture and scRGB output and was deliberately not started, to avoid breaking SDR.
* **8-bit source:** filters work on 8-bit captured frames. Strong grading can reveal banding; the output dither and
  film grain hide most of it.
* **Local effects:**
  * Clarity, local contrast and detail use a 1/2–1/16 resolution luma pyramid with smooth bicubic upsampling.
  * Very strong settings can still show soft halos around high-contrast edges (shadows/highlights is designed
    halo-free).
* **Motion blur:**
  * The blur window is causal (past frames only), so streaks trail behind objects.
  * The block-based motion model is weaker than SVP/RIFE around occlusions.
* **Elevated games** (run as administrator) need Blur Live to run as administrator as well.
* **Not tested here:** an actual VALORANT session (no game was running on the build machine). Vanguard is expected
  to allow Windows Graphics Capture, as with OBS window capture, but verify on your system.

## Troubleshooting

| Symptom | Fix |
|---|---|
| "An Application Control policy has blocked this file" | **Smart App Control** blocks unsigned executables it has no reputation for, and its verdict can change between builds. Turn it off (Windows Security → App & browser control) or sign the executable. |
| Black output / "frames are completely black" banner | Switch the game to borderless windowed. The game or DRM may block capture. Make sure the game isn't minimized. |
| Overlay doesn't appear | It hides while the game isn't the active window (Output → auto-hide). Click into the game. |
| Image looks unchanged | Check the master switch (Ctrl+Alt+F) and that the filter shows a change. Cards say "at neutral" when the values don't alter the image. |
| LUT not applied | The card shows the load error. Only 3D `.cube` LUTs with a 0..1 domain are supported. |
| Live preview empty | It updates only on the Filters page while capturing. It needs the UI and the pipeline on the same GPU (handled automatically). |
| Stutter / dropped frames | Lower motion-blur quality, disable denoise/bloom, or cap the game's frame rate to leave GPU headroom. Check the Performance page. |
| Capture fails with E_ACCESSDENIED | The game runs elevated: run Blur Live as administrator. |
| Hotkeys don't work | Another app already uses Ctrl+Alt+B/F/C/S (see the log). |

## Project layout

```
src/core/       settings (INI persistence), weighting, segment planner, logging, utilities
src/capture/    window enumeration, Windows Graphics Capture session
src/engine/     motion-blur engine (D3D11 compute) + shaders
src/filters/    filter registry, colour maths, presets, .cube LUT loader, FilterChain + shaders/
src/present/    output window (DirectComposition overlay / window / monitor), latency estimation
src/pipeline/   processing thread: device, capture → engine → filters → output, stats, recovery
src/platform/   GPU utilisation via PDH performance counters
src/ui/         Dear ImGui app (app.cpp), Filters page (app_filters.cpp), theme and widgets
tests/          test suite + developer utilities
docs/           analysis of the original Blur project
```

### Adding a filter

1. Add an id to `FilterId` in `src/filters/filter_registry.h`.
2. Add a `FilterDef` to the registry, in the same order as the ids. It declares:
   * the key and name;
   * the description;
   * the category;
   * its parameters (range, default and *neutral* value, display format);
   * its GPU cost.
3. Pack its parameters into `FxCB` in `FilterChain::packConstants()`, and the matching field in `fx_uber_cs.hlsl`.
4. Implement it in the uber shader behind its flag bit, or as its own pass for neighbourhood/scale effects.

Settings persistence, presets, reset, the UI card, search and neutral-identity tests come from the registry
automatically.

## Download

A prebuilt standalone `BlurLive.exe` (Windows 10/11 x64, no install needed) is in [`dist/`](dist/BlurLive.exe). It is
unsigned, so Windows SmartScreen / Smart App Control may warn about it; building from source is equally easy (see
Build).

## Licence

Blur Live is released under the [MIT licence](LICENSE). Its code is original: f0e/blur (GPL-3.0) was used only as a
technical/visual reference and no code was copied. Dear ImGui is MIT (see `third_party/imgui/LICENSE.txt`).
