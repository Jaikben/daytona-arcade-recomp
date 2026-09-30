# Vita performance diagnostics

## Native GXM builds (GPU20 / 01.18)

The sections below this one describe the original software-rendered diagnostic
patch, not the current `main_gpu.cpp` path. Current builds write
`ux0:data/daytona93/vita-diag.log`; copy it before relaunching.

- `geo_ms` measures complete geometrizer execution, including lighting. It is
  not a deadline that abandons later lighting work. Lighting is stored in each
  polygon before that polygon is published.
- `run_ms` measures main-thread board work. `sound_ms` and
  `audio_worker_queue_ms` are worker durations overlapping rendering; do not
  add them to main time. `sound_wait_ms` is the residual main-thread join.
- `poly_ms` includes `sort_ms`. `tiles_ms` and `upload_ms` measure the other
  native GXM preparation stages. These are CPU submission times, not GPU
  execution timestamps. `gpu_wait_ms` measures the pool-reuse fence.
- `menu`, `frames`, `presents`, and `window_ms` expose paused or repeated-frame
  windows. `log_prev_ms` measures the previous synchronous log write, which is
  included in the next FPS window but not in the rendering stage counters.

The banked-corner sampler audit (attract frames 1000-1120) found coordinates
outside the nominal texture dimensions on 48,582 polygons. The CPU masks those
coordinates even when the texture wrap flag is clear; that flag changes
bilinear neighbors at the seam, not the overall repetition. GPU19 selected
CLAMP in that case. Of 85,113 visible centroid samples, 4,037 became near-black
under CLAMP but bright under the CPU's repeating coordinates. GPU20 retains
MIRROR where requested and uses REPEAT otherwise. Exact seam interpolation and
mip/microtexture behavior remain separate GPU approximations.

GPU20's integer-key ordering preserves the CPU reference's window/depth/tie
order; the painter path still traverses that order in reverse. A 6,000-frame
host race compared 6,564,621 indices with no differences. Host timings do not
prove Vita speed or pixel output.

The rendering audit also found solid checker polygons filled as opaque quads
in GPU19. In the same host race, 27,614 such polygons were fully black, including
9,880 with bounding boxes over 5,000 native-screen pixels. Alternating-pixel
coverage must be retained, not replaced with an opaque shadow. Other GPU
approximations (including textured checker patterns, mip/microtexture sampling
and quantized lighting) remain separate fidelity limits; a black-road photo
alone does not distinguish them.

## Original software-rendered diagnostic patch

This patch is based on branch `psvita-native-frontend` at `d39e47a`, with
the separate 192 MiB startup correction already applied. It has not been
pushed to GitHub and is not a verified full-speed fix.

## Apply and build

Run from the repository root, after applying the startup patch. Save any
local edits first. The check does not overwrite conflicting changes.

```sh
git apply --check "$HOME/Downloads/daytona-vita-performance.patch" &&
git apply "$HOME/Downloads/daytona-vita-performance.patch" &&
python3 scripts/build_vita.py
```

The companion ZIP contains only changed/new source and test files. Its full
main.cpp and CMakeLists.txt include the startup correction as well. Prefer the
checked patch when retaining your own edits. Do not copy files into a different
revision without reviewing the differences.

The same existing host-generated game code is used; it need not be regenerated.
The build script still configures Release. Check a manually configured build:

```sh
grep -E 'CMAKE_BUILD_TYPE:|CMAKE_CXX_FLAGS_RELEASE:' build/vita/CMakeCache.txt
```

Install the rebuilt `build/vita/daytona_vita.vpk` with VitaShell. Leave the ROM
and saves under `ux0:data/daytona93/` in place. Keep the matching ELF locally.

## Capture

Leave the launcher open long enough for a `mode=MENU` sample, start the game,
and reproduce the slow scene until several `mode=GAME` samples have been
written. Copy `ux0:data/daytona93/vita.log` before relaunching: it is overwritten
on every launch. The new lines start with `perf:` and are written at most once
every two seconds, not per instruction. No file upload happens automatically.

## Read the measurements

`sim_fps` counts completed Model 2 frames per real second. `present_fps` counts
SDL presentations, which can include repeated images. Neither is a benchmark
result until observed on the device. CPU, GPU and bus clock readings are in MHz;
negative values indicate an SDK query error. The patch does not change clocks.

`total_ms`, `core_ms`, `geo_ms`, `video_ms` and `sound_ms` are averages per
completed simulation frame. `core_ms` is total minus the other three stages:
i960 execution, synchronous TGP work, scheduler and remaining frame overhead.
`geo_ms` wraps the geometry/vblank-start work; `video_ms` wraps software polygon
rasterization and screen composition; `sound_ms` wraps the 68000 and chip
simulation. Native-rate gameplay targets about 17.384 ms per board frame;
total work, presentation and scheduling all have to fit the real-time budget.

`audio_ms` measures conversion/queueing on the main thread, not the callback's
independent CPU cost. `upload_ms` is per screen upload; `draw_ms` and
`present_ms` are per SDL presentation; `save_ms` is per autosave check. They
have different denominators and must not simply be summed in MENU or when
frames are repeated. `present_ms` includes SDL command flushing, any driver
waits and vsync; it is not a GPU timestamp. These are elapsed host durations,
not CPU utilization percentages.

The loop presents each completed frame rather than calculating up to four
complete software rasters before displaying one. This can improve visible
updates and input-polling frequency when overloaded; it does not accelerate
the actual guest instructions or guarantee a higher simulation frame rate.
It deliberately discards overdue host-time debt under load. Fractional time
is retained to avoid accidentally halving the normal frame rate.

Text drawing now batches the same pixels in groups of up to 256 rectangles.
This reduces menu API calls; it is not a GPU conversion of the game renderer.
No geometry, sound, floating-point results or game instructions are skipped.

## Host checks

```sh
mkdir -p build
g++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc \
  tests/test_vita_performance.cpp -o build/test_vita_performance
build/test_vita_performance
g++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Itests/vita_sdl_shim tests/test_vita_text.cpp -o build/test_vita_text
build/test_vita_text
```

The SDL shim is a unit-test drawing recorder only; do not include it in any
game target. Unit tests use synthetic timer values, not synthetic game code.
The real ROM-generated game, ARM compilation/linking, device performance,
audio, suspend/resume and race parity remain to be tested for this patch.
