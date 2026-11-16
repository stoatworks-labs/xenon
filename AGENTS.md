# AGENTS.md: Xenon

Read this before touching the slice reader (what Arena writes, and when), the rig (the
bank's law, the exposure, the chase's moments), the light pass (its inside test is the
one the harness holds every pixel to), or the light buffer's format.

## The idea

Every slice in the operator's Advanced Output is a xenon strobe head, and a lighting
desk's chase fires them. Each head is a capacitor bank charged through a resistor and
dumped through its tube when its step fires. Nothing is animated: the chase decides who
fires when, the capacitor how much light there is, the tube how long it lasts.

Three models in sequence, each in its own file:

- **The slices** (`SliceMap.{h,cpp}`): FFGL never tells an effect about Advanced Output,
  so the plugin reads the operator's saved preset, following
  `Preferences/AdvancedOutput.xml`'s `presetFile`, or the Preset File parameter, or a
  Rows x Columns grid. Re-read on mtime, because Arena writes it only at Save & Close.
- **The rig** (`Rig.{h,cpp}`): the chase's steps on the host's bar phase (the crossing
  interpolated inside the frame), an onset or a press; each head's bank
  `v <- 1 - (1 - v) e^(-T/RC)`, `E = q^2`; each flash's light `(E/tau) e^(-(t - t_f)/tau)`
  integrated over the frame's own interval in closed form.
- **The picture** (`Shaders.{h,cpp}`, `Xenon.cpp`): one quad per lit head (its bounding
  box, padded a pixel) drawn additively into an R32F light buffer; each fragment decides
  whether its pixel centre is inside the head's region by an even-odd crossing count over
  the head's edges (and its mask's) in an edge texture; then the clip and the light are
  composited in linear light (Fill, Outline, Gate).

## Decisions taken without asking (the brief says decide and write it down)

- **`XN01`, `SW Xenon`.** The id was free across `~/Projects/resolume` and `~/dev`; the
  name is eight characters, well inside FFGL's unterminated sixteen.
- **`E = q^2`, not IDEAS.md's `q`.** IDEAS wrote `E0 (1 - e^(-dt/RC))`, which is the
  voltage (or the charge) fraction. A capacitor holds `C V^2 / 2`, so the energy is the
  square. The harness checks the square and its negative control is the unsquared law.
- **A head that has never fired is full**, and a new map (different regions) gives every
  head a full bank and puts the chase back at step 0. A re-read that finds the same
  regions is NOT a new map: Arena rewrites both files on every Save & Close, changed or
  not, and resetting the chase on each would be a visible glitch for nothing.
- **An onset or a press fires at the start of the frame that heard it**, so that frame
  shows it; a beat fires where it fell inside its frame (zoetrope's rule).
- **The camera is a 360-degree shutter**: a frame sees all the light in its own interval.
  There is no Shutter control: a strobe chase wants every flash seen, and the exposure
  then conserves light exactly (a flash summed over its frames is E, whatever tau).
- **The light buffer is R32F**, not RGBA16F. The light is one number per pixel; R32F is
  half the bytes of RGBA16F and holds it to float precision. RGBA16F was the first choice
  and was measured wrong: see the traps.
- **Hard edges.** A pixel is inside a region when its centre is. Arena crops a slice
  exactly, and a hard edge is what the harness can count. No antialiasing.
- **Alpha.** Fill and Outline make a lit pixel opaque as far as its light goes
  (`max(src.a, min(1, max(Colour x X)))`): a lamp in front of the clip puts light where
  there was none. Gate keeps the clip's alpha: lighting a surface adds no matter. Output
  premultiplied, as the input is taken to be.
- **Outline is inside the region**, a band of Width from its edge. Width is in
  composition pixels (the units Arena shows a slice in), turned into output pixels by
  the output's height over the composition's. Where the output's aspect is not the
  composition's (a layer squeezed in Arena) the band is still a distance in output
  pixels, so it is Width composition pixels vertically and not exactly horizontally. In
  Arena the effect renders at the composition's size, where it is exact. On a grid there
  is no composition and Width is output pixels.
- **Masks.** Invert 1 (Arena's default) shows the slice inside the mask, 0 outside. Read
  from the Advanced Output editor (it shades the inside as shown), NOT from a picture of
  Arena's output: see "Verified, and assumed".
- **Ignored in 0.1.0:** screen-level Output Masks and DMX fixtures/lumiverses (they are
  not slices of the composition; DMX fixtures sample the composition too and are a
  candidate for later), and disabled slices, screens and masks.
- **Identical regions collapse** to one head (every point within 0.01 composition pixel,
  same mask). unmaptest has a full-frame slice on each of Screens 1 to 4. LPP slicer
  proof B's two right-hand slices are 0.001 px apart and stay two heads.
- **Overlapping heads add** their light, as two lamps on one wall do.
- **Head order:** As Listed is Arena's panel order (screens in file order, each screen's
  slices REVERSED from the file, which lists them bottom-up); Left to Right and Top to
  Bottom sort area centroids.
- **Limits:** 256 heads and 8192 edges from a preset (more are dropped and logged). The
  grid is Rows x Columns, 1 to 32 each, up to 1024 cells, not capped at 256: a cell is
  four edges and a quad, and the SPEC's 256 is about presets.
- **Poll every 0.5 s of the plugin's own clock** (two `stat` calls), at once when the
  clock runs backwards or a setting changes. The file is read on the render thread and
  never in a setter or the constructor (jackpot's rule: Arena constructs every plugin at
  its startup scan).
- **Random** is Fisher-Yates per cycle of N steps on PCG's hash of (Seed, cycle): every
  head once a cycle, the order reproducible.
- **No host beat:** the plugin's own clock at the host's BPM (120 if none) drives the bar
  phase, so a host that never moves it still has beats.
- **A frame fires at most 64 steps.** More is a seek or a stall, not music.
- **Colour is FF_TYPE_RED/GREEN/BLUE**, which Arena shows as one colour picker (tinsel's
  "Colour", "Colour_Green", "Colour_Blue"); taken as sRGB codes and decoded.

## The traps actually hit

- **RGBA16F truncates on this Mac's GPU.** With the light buffer RGBA16F, every measured
  light came back LOW, by up to 9e-4 relatively, and a long flash summed over 600 frames
  came out 3.7e-4 short: half a half-float ULP on average. That is round-toward-zero, not
  round-to-nearest; GL leaves the rounding of a conversion to half to the implementation.
  The checks had been written with a 2^-11 tolerance (one round-to-nearest) and failed. A
  tolerance of one whole ULP would have passed, but R32F removed the cause, halved the
  buffer, and took the worst error to 3.5e-6.
- **An OBJECT library linked to an OBJECT library** gives its objects only to that
  library, never to that library's consumers: `xenon_core` linking `xenon_slices` left
  the bundle with undefined `MapSource::Poll`. The bundle and the harness name
  `xenon_slices` themselves.
- **`std::vector< int > perm( size_t( n ) );` is a function declaration** (the vexing
  parse); clang said so as a warning and then failed on the subscripts.
- **Arena writes the preset only at Save & Close**, and moves the pointer's mtime on
  every Save & Close even when nothing changed (measured, SPEC-xenon.md). So the plugin
  re-reads on mtime, and `MapSource::Poll` compares the new regions with the old before
  calling it a change.
- **The XML lists a screen's slices bottom-up**: Arena's panel shows the reverse.
- **A `<Polygon>`'s `<InputRect>` is only its bounding box**; the shape is the
  `<InputContour>`. A circle mask is `CCCC` with twelve points (an anchor and two
  controls per segment, the next anchor wrapping).
- **A slice's InputRect is already rotated**: unmaptest's Slice 3 at -3.107 rad has its
  first corner, its own top left, at the composition's bottom right. Nothing assumes an
  axis-aligned rectangle.
- **The even-odd test's crossing side is not a mutation target.** Counting crossings to
  the left of a point instead of the right gives the same parity for any closed polygon
  (the line crosses it an even number of times), so `p.x < ...` -> `p.x > ...` is an
  equivalent mutant everywhere off an edge. The mutants used flip the straddle test and
  the crossing's height instead.
- **The harness must never read this Mac's Documents.** Every check points the plugin at
  a Documents folder that is not there (`kNowhere`) unless it builds a scratch one, so a
  run does not depend on what Arena has active.
- **`InsidePolygon` (CPU) pairs edges in the shader's order** (point i to i + 1). It is
  not what the harness checks pixels with (that is a winding number), but it is the
  plugin-side statement of the same test, and the same form keeps the two comparable.
- **oxbow is not built in `~/Projects`**, and `~/Projects` is read-only reference. verify
  used a copy built in the session's scratch directory (`OXBOW=...`).
- **Driving Arena with computer use** (step 5): Arena's menus ignore the accessibility
  press, so it took full-screen control and real clicks; a half-scale screenshot's
  coordinates have to be doubled; LogiPluginService and Chrome steal focus between
  batches.

## Verified, and assumed

Verified offline, on this Mac (M4 Max, and Apple's software renderer at 320x180):

- the reader on all four presets and the 7.28.1 save, to the corners Arena wrote (1e-12);
  the circle mask's 64 points to an independent evaluation of its four cubics;
- the pointer, the override, the Avenue fallback and the three ways to end on the grid;
- the re-read within one poll interval, nothing read while nothing changes, and an
  unchanged Save & Close leaving the chase running;
- the bank's law over dt/RC from 0.13 to 4 (3.5e-6), a flash's light over all its frames
  (to the stated tail of the light below kDark), the frame-to-frame fall e^(-T/tau);
- every head's light on every frame of 24 beats at 128 BPM, for every pattern, both head
  orders, and Random's cycle and Seed;
- every pixel of every head, lit alone, on every map and the grid, Fill and Outline;
- alpha in all three looks over opaque, half and clear clip; a resize keeping the
  banks; the host's GL state.

Assumed, not verified:

- **Which side of an Input Mask Arena shows.** The editor shades the inside for Invert 1;
  the plugin follows that. Step 5's result is under "In Arena".
- **Windows.** The Documents folder through `SHGetKnownFolderPath`, `_wfopen` and
  `_wstat64`: MSVC compiled them clean, with no warnings, on ci.yml's first run
  (2026-11-16). Running them is under "In Arena".
- **Avenue** writes the same files under `Resolume Avenue`. Not seen; the path is
  Arena's with the product name changed.
- **A preset Arena wrote in another version.** The fixtures are 7.27.1 and 7.28.1 saves.

## Would this hold on another rasteriser, at another raster?

- **The inside test** is float32 in output pixels. The harness leaves out pixel centres
  within 1e-3 px of a boundary: four float ULPs at 2048 to 4096 px, so it holds to 4K.
  The software renderer at 320x180 passes it to the pixel. Pixels exactly ON an edge
  (output_map_1's trailer meets y = 112.5 at 1280x720) are left out and counted; the
  half-open rule decides them, the same way, on every rasteriser.
- **The light values** are R32F; the remaining error is GLSL's `pow` in the sRGB curve.
  The tolerance allows 64 float ULPs for the round trip; Apple's GPU and software
  renderer use 3.5e-6 of 7.9e-6. A GPU whose `pow` is worse than that would fail
  `--charge` without being wrong: GLSL 4.10 only promises `pow` "inherited from
  exp2(y log2 x)". If that happens, measure it and widen with a reason, do not guess.
- **R32F as a blended render target** is core in desktop GL 4.1 (float blending is only
  missing in GLES), on macOS, Mesa and Windows drivers alike.
- **The chase's moments** come from the host's float bar phase. Its rounding moves an
  interpolated beat by up to 2^-24 x 240/BPM s; `--chase` carries that term, and it
  dominates the tolerance (worst measured 0.14 of it).
- **At another raster:** every picture check runs at 1280x720 and 320x180, and the resize
  check goes from each to about half. Nothing in the plugin depends on the raster but the
  geometry, rebuilt on any change.

## The negative controls and mutants

`xntest --negative`, 16 wrong models, each required to fail its check:

| check | wrong model |
| --- | --- |
| reader | every duplicate kept (unmaptest gives 7) |
| reader | a slice's corners replaced by their bounding box |
| reader | heads in the file's order, not the panel's |
| reread | only the pointer watched (a preset rewritten alone is missed) |
| charge | `E = q`, the unsquared law |
| charge | the bank charging through 2 RC |
| charge | a press's flash at the end of its frame |
| chase | a beat's step at the start of its frame |
| chase | step k lights what step k - 1 should |
| inside | the head's whole quad lit (its bounding box) |
| energy | the light at the frame's end times its length, not the integral |
| audio | the onset detector unprimed (deaf for the first kicks) |
| audio | an onset's flash at the end of its frame |
| alpha | the clip's alpha passed through under Fill and Outline |
| resize | a new raster empties every bank |
| cues | every control ramps |

`tools/mutate.sh`, 13 one-character mutants, all caught (2026-11-09):

| where | mutant | caught by |
| --- | --- | --- |
| GLSL light | the straddle test `!=` -> `==` | --inside |
| GLSL light | the crossing's height from `p.y + a.y` | --inside |
| GLSL light | Outline's band `Band + Band`, not `Band * Band` | --inside |
| GLSL light | the mask mode drawn as the light, `extra.y` -> `extra.x` | --charge |
| GLSL composite | Gate's `Room * lamp` | --alpha |
| GLSL composite | Fill's lamp subtracted | --alpha |
| Rig.cpp | `E = q + q` | --charge |
| Rig.cpp | the beat's place from `boundary + p0` | --chase |
| Rig.cpp | a frame's catch from `t0 + f.time` | --energy |
| SliceMap.cpp | y multiplied by the composition height | --reader |
| SliceMap.cpp | collapse only when x differs | --reader |
| Xenon.cpp | the map not flipped to GL's y up | --inside |
| Xenon.cpp | Outline's Width scaled the wrong way | --inside |

## The sweep's honest list

`tools/sweep.py` finds all 19 swept controls live (Audio is the input Threshold is swept
through). With every context dropped (`--bare`), five go dead, each because it acts only
with another set: **Threshold** (needs Trigger Onset and music), **Seed** (Pattern
Random), **Width** (Look Outline), **Rows** and **Columns** (Slices Grid). Recharge stays
alive bare: on unmaptest's four heads a slow Recharge leaves a bank short after 2 s.

## Shape of the code

- `source/SliceMap.{h,cpp}`: the preset reader (a small XML reader of its own: entities,
  comments, CDATA), the grid, head order, the Documents folder, `MapSource`. No GL.
- `source/Rig.{h,cpp}`: the chase, the banks, the exposure. No GL.
- `source/Controls.{h,cpp}`: the parameter table and every conversion to physical units.
- `source/Shaders.{h,cpp}`: the two passes; `Assemble()` is the one place a fragment
  shader's order is written (`tools/glslc.sh` mirrors it).
- `source/Xenon.{h,cpp}`: the plugin: clock, audio, map, rig, geometry, the passes.
- `source/Audio`, `PassBuffer`, `Diag`, `GLState`: the fleet's, adapted.
- `tools/xntest/main.cpp` (the host, clips, PNG, pipe) and `checks.inc` (the checks, the
  independent models, the negative controls, the bench).
- `tools/slicedump`: prints what the reader finds in a preset (`--active` for this Mac's).

## In Arena (2026-11-09, Arena 7.28.1, this Mac)

Recorded in SPEC-xenon.md and README's Status.

## Not done

- No OpenFX port, no presets.
- No reply to the viewer who asked for it.

## Open design questions

- DMX fixtures as heads (they sample the composition, as slices do).
- A Shutter control, for a camera that misses flashes on purpose (zoetrope has one).
- Whether a Polygon slice's InputRect bounding box ever disagrees with its contour in a
  way Arena itself uses (it does not here).
