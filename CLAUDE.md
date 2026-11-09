# xenon

Every slice in the operator's Advanced Output a xenon strobe head, fired by a lighting
desk's chase, for Resolume Arena/Avenue, as an FFGL effect. C++/GLSL, CMake MODULE →
universal `.bundle` (macOS) + Windows `.dll`. MIT. ID `XN01`, display name `SW Xenon`.

Read `AGENTS.md` before touching the slice reader (what Arena writes, and when), the
rig (the bank's law, the exposure, the chase's moments), the light pass (its inside
test is the one the harness holds every pixel to), or the light buffer's format.
The design is `~/Projects/resolume/specs/SPEC-xenon.md`.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build` (writes into Arena's Extra Effects)
- Render offline: `./build/xntest --out /tmp/frame.png` (unmaptest's fixture, the
  defaults at 120 BPM on the harness's stripes, 150 frames in)
- The frame a beat lands on: `--frames 152` (the beat at 2.5 s is caught by the frame after it)
- Another map: `--preset tests/presets/output_map_1.xml`, `--preset grid` (Rows x
  Columns), `--preset active` (whatever this Mac's Arena has active)
- What the reader finds in a preset: `./build/slicedump FILE` or `./build/slicedump --active`
  (built with the harness: `XENON_BUILD_TOOLS`, on by default)
- List parameters: `./build/xntest --list`
- Set anything by name: `./build/xntest --set "Pattern=Bounce" --set "Look=Outline"`
- Press something later: `./build/xntest --at 100 "Fire=1"`; music in the Audio buffer: `--kicks`
- The harness's clips: `--clip look|black|white`
- Film: `./build/xntest --film 600 --size 1280x720 --script cues | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - out.mp4`
- A clip through it: `ffmpeg -i in.mov -vf fps=60 -f rawvideo -pix_fmt rgba - | ./build/xntest --pipe --size WxH | ffmpeg …`

## Verify
- Everything: `tools/verify.sh` (about three minutes on an M4 Max: fresh universal build, every
  picture check at 1280x720 and 320x180, again on Apple's software renderer, the
  offline checks, the negative controls, 13 mutants, the sweep, lipo, plist, ad-hoc
  signature, `oxbow probe` and `selftest`, the pipe, the bench). oxbow is found on
  PATH or `OXBOW=/path/to/oxbow`; never build it inside `~/Projects`.
- **The slices** (no GL): `--reader` (the four presets and the 7.28.1 polygon-and-masks
  save, to the corners Arena wrote), `--pointer` (a scratch Documents tree: the
  pointer, the override, Avenue, and every way to end on the grid).
- **The re-read**: `--reread` (a Save & Close picked up within a poll; nothing read
  while nothing changes; an unchanged save leaves the chase running).
- **The strobe**: `--charge` (E = (1 - e^(-Δt/RC))² over Δt/RC 0.13–4), `--energy`
  (a flash's light over all its frames is E; frames fall by e^(-T/τ)), `--chase`
  (every head on every frame of 24 beats, every pattern), `--audio`.
- **The picture**: `--inside` (every pixel of every head, lit alone, against a
  winding number in double, Fill and Outline), `--alpha`, `--resize`, `--state`.
- **The rest**: `--cues`, `--names`; `--offline` is what CI runs (no GL).
- The checks can fail: `--negative` (16 wrong models), `tools/mutate.sh` (13 mutants).
- No dead controls: `python3 tools/sweep.py` (19 swept, all live; the 20th, Audio,
  is the input Threshold is swept through; `--bare` lists the five that need a context).
- Render cost: `./build/xntest --bench`.

## Notes
- **FFGL never tells an effect about Advanced Output.** The plugin reads the saved
  preset `Preferences/AdvancedOutput.xml` points at (or the Preset File parameter),
  and falls back to a Rows x Columns grid. Arena writes both files only at Save &
  Close, so the plugin stats them every 0.5 s of its own clock and re-reads on mtime;
  a re-read that finds the same regions is not a new map.
- **The file is read on the render thread**, never in a setter or the constructor.
- **The light buffer is R32F.** RGBA16F truncated on this Mac's GPU (AGENTS.md).
- **The inside test is the shader's**: an even-odd crossing count, half-open, over
  the head's edges in an RGBA32F texture, one bbox quad per lit head, additive. The
  harness checks it with a winding number in double: keep them different.
- **Nothing absolute reaches the GPU.** Times are reduced in double on the CPU; the GPU
  gets one light value per head per frame.
- All host parameters are 0..1 and mapped in `Controls.cpp`, except Seed, Rows and
  Columns (real integers); options hold the element index; Fire acts on the rising
  edge; Colour is FF_TYPE_RED/GREEN/BLUE (one picker in Arena); Preset File is
  FF_TYPE_FILE (a text path through `SetTextParameter`).
- GLSL reserved words (`patch sample input output filter common active half
  layout flat packed`, `noise1`–`noise4`) must not be identifiers; `verify.sh`
  greps for them.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin.
- `xenon_core` and `xenon_slices` are OBJECT libraries: the plugin registers itself
  from a file-scope constructor nothing references. An OBJECT library's objects reach
  only what links it directly, so the bundle and xntest each name `xenon_slices`.
- The macOS build must be universal. Check with `lipo`, never the build log.
- `source/StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies
  (no guide button) until the project is registered with stoatworks-backend.
- Local only: no GitHub repository, no tag, no website entry. The workflows in
  `.github/` assume the public `stoatworks-labs/xenon` they are written for; landing
  it in the fleet is the owner's call.

## Not done yet
- Never built on Windows. No OpenFX port, no presets, no user guide, no browser demo.
- See README's Status for what Arena on this Mac has and hasn't shown.

## Diagnostics

`source/Diag.{h,cpp}`: log file only, no crash handler (this runs inside Resolume).
It says which map was read (or why the grid), every re-read, and what was dropped.

    ~/Library/Logs/xenon/xenon.YYYY-MM-DD.log
    %LOCALAPPDATA%\xenon\logs\xenon.YYYY-MM-DD.log

`XENON_LOG_DIR` overrides the folder.
