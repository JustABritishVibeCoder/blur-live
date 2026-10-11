# Analysis of f0e/blur (reference project)

Repository: https://github.com/f0e/blur — inspected October 2026 (source files under `src/vapoursynth/blur/*.py`,
`src/vapoursynth/blur.py`, `src/common/config_blur.*`). Blur Live contains **no code from this project**; it is an
independent real-time reimplementation of the technique described below.

## What Blur is

An offline video tool (C++ GUI/CLI on top of **VapourSynth** + **FFmpeg**) that renders motion blur into recorded
gameplay. It is not a frame-blending filter on its own: its characteristic look comes from interpolating to a very high
frame rate first.

## Pipeline (verified in source)

1. **Decode** – `bs.VideoSource` (BestSource) or `lsmas.LWLibavSource`, optional GPU decoding.
2. **Deduplicate** (optional, on by default) – duplicated frames are replaced by interpolated ones (SVP, RIFE or an
   older method) so stutter in the recording doesn't become uneven blur.
3. **Pre-interpolation** (optional) – RIFE (ncnn/Vulkan) to an intermediate rate (default 360 fps).
4. **Interpolation** (on by default) – to `interpolated_fps` (**default 1200**, 600 on macOS). Methods:
   - **SVP** (default on Windows/Linux): `svp1.Super` → `svp1.Analyse` (block motion vectors) →
     `svp2.SmoothFps` (motion-compensated frame synthesis). Defaults: block size 8, overlap 2, algorithm 13,
     preset "weak", scene detection disabled.
   - **RIFE** (default on macOS): `core.rife.RIFE`, model `rife-v4.26`.
   - An MVTools path (`mv.Super`/`mv.Analyse`/`mv.FlowFPS`) exists in the code base.
5. **Frame blending** – `frame_gap = int(fps / blur_output_fps)`, `blur_frames = int(frame_gap * blur_amount)`, forced
   odd; neighbouring frames (a window centred on each output frame) are averaged with weights from
   `weighting.parse()`. `blur_gamma != 1` switches to `average_bright`, which raises values to `gamma` before averaging
   and back afterwards (approximate linear-light blending). Output is resampled to `blur_output_fps`
   (default **60**) by picking frames.
6. **Filters / encode** – optional brightness/saturation/contrast (`adjust.Tweak`), then FFmpeg (default h264, CRF 16).

Defaults: `blur_amount = 1` (a 360° shutter: each 60 fps output frame integrates a full 1/60 s), weighting `equal`.

## Weighting functions (index 0 = oldest frame of the window)

| Name | Definition |
|---|---|
| equal | uniform |
| gaussian | `exp(-(x-mean)^2 / 2sd^2)` with `x = linspace(bound)`; defaults mean 2, sd 1, bound [0, 2] → peaks at the newest frame |
| gaussian_sym | same with mean 0 over `[-M, M]`, `M = max(|bound|)` |
| pyramid | `h - |i-h| + 1`, `h = (n-1)/2` |
| ascending / descending | `i+1` / `n-i` |
| vegas | `1,2,…,2,1` for even counts, uniform otherwise |
| custom | user list stretched across the window |

All are normalised to sum 1 (negative values shifted first). The header credits smoothie-rs and HFR-Resampler.

## Dependencies

VapourSynth, FFmpeg, plugins: SVPflow (svpflow1/2), BestSource, MVTools, Akarin (`akarin.Expr`), RIFE-ncnn-Vulkan,
Adjust; Dear ImGui and stb for the GUI. On macOS SVP needs the (paid) SVP Manager.

## Licence

**GPL-3.0.** Copying or adapting its code would make the derived program GPL-3.0 as well. Blur Live therefore reuses
only ideas and published mathematical definitions (weighting formulas, the "interpolate then average" model, the
`amount / output fps` shutter semantics) and implements everything itself. If you ever copy Blur code into this
project, the project must be distributed under GPL-3.0.

## What can be reused for real time, and what cannot

| Blur component | Real-time viability | Blur Live |
|---|---|---|
| SVP / MVTools block motion vectors | Too slow / CPU-bound for 1080p at 280 fps through VapourSynth; SVP is proprietary | Own GPU hierarchical block matching (4-level pyramid, 8×8 overlapped blocks, vector median, half-pel refinement) in compute shaders |
| Interpolate to 1200 fps, then average | Generating hundreds of full frames per second is wasteful | Mathematically equivalent shortcut: integrate the shutter per pixel with N warped samples directly (no intermediate frames stored) |
| Centred window (uses future frames) | Impossible live without adding a whole shutter of latency | Causal window `[now − S, now]` |
| Weighting functions | Trivially portable | Same formulas, evaluated as a 64-entry LUT on the GPU |
| Gamma ("bright") blending | Portable | `pow` before/after accumulation |
| Deduplication | Not needed: WGC only delivers frames that changed | Timestamps drive all timing; long gaps reset history |
| RIFE | Possible on the GPU but several ms per frame at 1080p; not in the first release | Engine interface leaves room for a flow-field provider |
| FFmpeg encode | Not applicable (live display) | Swap chain / DirectComposition presentation |

## Quality trade-off, stated plainly

Simple frame blending of captured frames is **not** equivalent to Blur's output: at equal capture and display rates
it produces discrete ghost copies (or nothing at all with a 360° shutter at the capture rate). Blur Live's
motion-compensated mode approximates Blur's interpolated result, but with a block-based motion model that is weaker
than SVP/RIFE around occlusions and thin structures; where the estimate is unreliable the pixel is kept sharp rather
than smeared.
