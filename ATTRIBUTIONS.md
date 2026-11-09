# Attributions

Xenon is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is a provisional hand copy: the master lists live in the `stoatworks-backend` repo and
are pushed out by `scripts/sync-attributions.py` once a project is registered there. Xenon
is not registered yet, so this file was written by hand on 2026-11-09 and will be replaced
by the generated one.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The effect's shape, the harness and the tooling — Stoatworks zoetrope

<https://github.com/stoatworks-labs/zoetrope>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin class's shape and its table of controls, the host-clock vote, source/GLState.h, the harness's host, PNG writer, cue sheets, --pipe/--film (SIGPIPE ignored, a closed stdout exits 1), --negative with its Perturb, --state, --names, --cues and --offline, and tools/verify.sh, tools/mutate.sh, tools/glslc.sh, tools/sweep.py, the CI and release workflows and scripts/release-lib.sh are zoetrope's (schlieren's, fiche's and tinsel's before it), adapted. Firing a beat at the moment inside the frame where it fell (source/Rig.cpp's crossings) is zoetrope's, which is schlieren's and fiche's lesson.

### The audio analyser, the pass buffers and the log — Stoatworks millpond

<https://github.com/stoatworks-labs/millpond>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Audio.{h,cpp} is millpond's onset analyser (rosette's, adapted from macroblock's), with its primed first frame, by way of zoetrope; source/PassBuffer.{h,cpp} (FFGLFBO with the SDK's colour-texture leak fixed) and source/Diag.{h,cpp} are millpond's. All adapted.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### Resolume Arena's own Advanced Output presets

The four presets on the Mac this was written on (two LPP slicer proofs, an output map and unmaptest) and an Arena 7.28.1 save with a Polygon and two Input Masks, committed under tests/presets: the format source/SliceMap.cpp reads was learned from them, and xntest --reader holds the reader to the corners Arena wrote in them. Arena's behaviour around them (when it rewrites the file) was measured by watching the files while driving Arena by hand.

### The winding number

tools/xntest/checks.inc's windingNumber: a region test in double with nothing in common with the shader's float even-odd crossing count, which xntest --inside holds every pixel of the light pass to.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The request — @CariTauGan

A YouTube viewer's comment on the Vol. 6 reel (2026-10-19/20) asked for "zoetrope but instead of strobe on clip, it strobe on slices": a strobe that fires slice by slice across an Advanced Output, for operators who find Resolume's own Strobe hard to manage across many slices.

### Xenon strobes and lighting-desk chases

A xenon strobe's capacitor bank, charged through a resistor and dumped through the tube (why a strobe dims as its rate goes up), and the step chases every lighting desk runs. Built from how the things work; nothing is copied from anyone's software, fixture profile or design.

## Standards and published specifications

What the implementation is measured against.

- **IEC 61966-2-1 (sRGB)** — toLinear() and toCode() in source/Shaders.cpp: the clip is decoded to linear light so a lamp's light adds to it, then encoded again.
- **The RC charging curve and a capacitor's energy** — V( t ) = V_s ( 1 - e^( -t / RC ) ) and E = C V^2 / 2, in source/Rig.cpp: a flash after an interval carries ( 1 - e^( -t / RC ) )^2 of a full bank's energy.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
