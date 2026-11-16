# xenon

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. Its behaviour is measured rather
> than asserted: an offline harness drives the plugin's own class in a headless GL context
> and checks each claim out of the picture. It reads the slices of four real Advanced
> Output presets, and a save with a polygon and two masks, to the corners Arena wrote; a
> flash's peak follows the capacitor's (1 − e^(−Δt/RC))² to 3.5e-6; a flash's light summed
> over every frame it lands in is its energy, whatever its length; every head lights on
> the frame its beat falls in, for every pattern; and every pixel of every head, lit
> alone, is inside its slice exactly when an independent winding number says so. 16
> negative controls and 13 one-character mutants prove the checks can fail. The full
> rundown, including what Arena has and hasn't shown, is under [Status](#status).

Every slice in your Advanced Output is a xenon strobe head, and a lighting desk's chase
fires them. An FFGL effect for [Resolume](https://resolume.com) Arena and Avenue.

![Five white outlines over a dark striped clip: a frame round the whole picture, a rectangle tilted a little at the top right, a small triangle at the top left, a tall triangle whose apex touches the top edge and whose base lies on the bottom edge, and a circle at the bottom left](docs/hero.png)

<sub>One frame, rendered by `xntest`, the offline harness, not captured from Resolume.
An Arena 7.28.1 Advanced Output preset with a full-frame slice, a slice rotated a little,
a triangular polygon, and two slices seen through input masks (a triangle and a circle):
Pattern All, Look Outline, 35% room light,
on the frame that catches a beat at 120 BPM.</sub>

## The one idea

**Nothing here is animated. Each slice is a strobe head with a capacitor bank behind it,
and the chase decides which heads fire when.**

- **the chase** steps on the bar, the beat or its subdivisions (from Arena's own clock),
  on onsets in the audio, or when you press Fire, and each step fires one head, or
  several, by its pattern;
- **the bank** behind each head charges through a resistor between its flashes, so a
  head that fired a moment ago has less to give: a flash carries
  (1 − e^(−Δt/RC))² of a full bank's energy, Δt since that head last fired;
- **the tube** dumps the bank as light that dies away with its own time constant;
- **the camera** sees all the light that lands inside each frame.

Nothing in the plugin is told to make any of this happen. It falls out:

- **a fast chase gets dimmer**, because the heads can't recharge in time: what real
  strobes do at high rates;
- **a Bounce chase lights its inner heads alternately weak and strong**, because they
  fire at alternating short and long intervals, and the end heads full;
- **in a Build the first head fires on every step and is always the weakest**;
- **Gate turns the clip into flashes of itself**, slice by slice.

## Where the slices come from

FFGL never tells an effect about Advanced Output, so Xenon reads the preset Arena saved:
`Documents/Resolume Arena/Preferences/AdvancedOutput.xml` names the active preset, and
the preset holds every slice's corners in composition pixels. Rotated slices, polygons
(lines and curves) and input masks are read as the shapes they are, not their bounding
boxes. Disabled slices and screens are skipped; the same region on several screens is one
head.

Arena writes the preset only when you press **Save & Close** in the Advanced Output
window. Xenon looks for a change twice a second and picks the new map up then; while
you're editing, it shows the map you last saved. A save that changes nothing doesn't
restart the chase.

If there's no map (no preset, or one with no slices), or you set **Slices** to Grid,
the heads are a Rows × Columns grid. The **Preset File** control points at any other
preset you choose.

## Controls

| Group | Control | What it does |
| --- | --- | --- |
| Chase | **Pattern** | Forward, Reverse, Bounce, Build (every head up to the step's), Random (every head once a cycle, in an order set by Seed), All (every head every step) |
| | **Trigger** | Bar, Beat, Half Beat, Quarter Beat, Eighth Beat, Onset (the Audio input), Fire Only |
| | **Fire** | A step now, in any mode |
| | **Audio**, **Threshold** | The music Onset listens to, and how loud a hit must stand out |
| | **Seed** | Random's order |
| Strobe | **Recharge** | The bank's time constant RC, 5 ms to 5 s |
| | **Decay** | The tube's time constant, 0.2 ms to 500 ms |
| | **Intensity** | A full bank's flash, 0 to 2 |
| | **Colour** | The tube's colour |
| Look | **Look** | Fill (a lamp in front of the clip), Outline (a tube bent round each slice's edge, inside it), Gate (the clip lit by the strobe) |
| | **Width** | Outline's band, 1 to 100 composition pixels |
| | **Room Light** | How much of the clip shows between flashes |
| Slices | **Slices** | Map (the Advanced Output preset) or Grid |
| | **Preset File** | Another preset instead of the active one |
| | **Order** | As Listed (Arena's panel order), Left to Right, Top to Bottom |
| | **Rows**, **Columns** | The grid, 1 to 32 each |

## Status

Version 0.1.0, 2026-11-09. Public on GitHub since 2026-11-16; no release yet.

**Measured offline**, on an M4 Max and Apple's software renderer, by `xntest` (every picture
check at 1280x720 and 320x180):

- the slices of the four presets on the Mac this was written on, and an Arena 7.28.1
  save with a polygon and two masks, to the corners in the files (1e-12); a circle mask's
  curves to an independent evaluation; Arena's panel order; duplicates collapsed;
- the active preset found through `AdvancedOutput.xml`, the Preset File override, and
  the grid in each case where there's no map; a rewritten preset picked up within half
  a second, and nothing read while nothing changes;
- the bank's law over Δt/RC from 0.13 to 4, to 3.5e-6; a flash's light over all its
  frames equal to its energy; successive frames falling by e^(−T/τ);
- every head's light on every frame of 24 beats at 128 BPM, for every pattern and order;
- every pixel of every head on every map and on the grid, Fill and Outline, inside its
  slice exactly when a winding number in double says so;
- onsets stepping the chase; alpha; a resize keeping the banks; the host's GL state;
- 16 negative controls and 13 mutants (six in the GLSL) caught; all 19 swept controls
  live;
- GPU cost: 0.65 ms a frame at 4K with unmaptest's four heads, 0.90 ms with 256.

**In Arena** (7.28.1, macOS): not yet loaded.

**Not established:**

- Windows: the code (the Documents folder, wide-character file access) compiles clean
  with MSVC; it has not yet run in Resolume on Windows.
- Avenue: its folder is Arena's with the name changed; never seen.
- Presets saved by Arena versions other than 7.27.1 and 7.28.1.
- Screen-level output masks and DMX fixtures are ignored in this version.

## Build

    git submodule update --init
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    cmake --install build

`cmake --install` copies `Xenon.bundle` into `~/Documents/Resolume Arena/Extra Effects`.
The release build is universal (Apple Silicon and Intel); add
`-DCMAKE_OSX_ARCHITECTURES=arm64` for a quicker development build.

## Testing

    tools/verify.sh

builds from scratch, checks the bundle, and runs every check, the negative controls,
the mutants, the sweep for dead controls, and the bench. `./build/xntest --help` lists the
checks one by one, and renders frames and films offline. `./build/slicedump --active`
prints the heads Xenon would find in the preset Arena has active.

## Thanks

To @CariTauGan, who asked for a strobe that fires slice by slice. See
[ATTRIBUTIONS.md](ATTRIBUTIONS.md) for the work this is built on.

## Licence

MIT. See [LICENSE](LICENSE).
