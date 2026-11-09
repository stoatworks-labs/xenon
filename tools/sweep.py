"""Render every parameter at both ends of its range and fail if any made no
difference.

**This is the only thing in the repo that catches a dead control.** A GLSL
uniform whose name does not match the C++ is silently ignored --
`glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented no-op
-- so a slider can be stone dead while everything compiles, links, loads and
renders.

Each control is swept where it can act. Every render is 152 frames at 120
BPM on the harness's look clip over the committed unmaptest preset (four
heads), so the last frame is the one that catches the flash of the beat at
2.5 s: most of the strobe's controls show only in a flash. Fire is a press,
swept by pressing it on the last frame (`--at`) with Trigger Fire Only;
Threshold acts only on music (`--kicks`, Trigger Onset, a long Decay so the
picture holds what the onsets did); Seed only with Pattern Random; Recharge
only when a head fires again before its bank is full (Pattern All, every
head every beat); Width only with Look Outline; Rows and Columns only with
Slices Grid. An option parameter reads back 0..1 whatever its count (the
fleet's trap), so options are set here by element index. A control whose
ends differ by less than `--floor` (mean 8-bit difference per channel) is
reported as barely alive: vectrix's lesson.

Every render is 320x180 (CI's raster).

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose] [--jobs N] [--bare]

`--bare` drops every per-control context (keeping only the base), the
addendum's honesty check: what goes dead there is the list of controls that
only act with another one set, which AGENTS.md records.
"""

import argparse
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

BASE = []

# A setting is "Name=Value" (passed as --set) or a list of raw arguments.
MUSIC = [["--kicks"]]
OTHER_PRESET = str(REPO / "tests" / "presets" / "LPP slicer proof A.xml")

# name -> (low settings, high settings, context settings)
SWEEP = {
    "Pattern": (["Pattern=0"], ["Pattern=5"], []),
    "Trigger": (["Trigger=1"], ["Trigger=6"], []),
    "Fire": ([], [["--at", "151", "Fire=1"]], ["Trigger=6"]),
    "Threshold": (["Threshold=0"], ["Threshold=1"], MUSIC + ["Trigger=5", "Decay=0.9"]),
    "Seed": (["Seed=1"], ["Seed=1000"], ["Pattern=4"]),
    "Recharge": (["Recharge=0.3"], ["Recharge=0.9"], ["Pattern=5"]),
    "Decay": (["Decay=0.2"], ["Decay=0.9"], []),
    "Intensity": (["Intensity=0.25"], ["Intensity=0.75"], []),
    "Colour": (["Colour=0"], ["Colour=1"], []),
    "Colour_Green": (["Colour_Green=0"], ["Colour_Green=1"], []),
    "Colour_Blue": (["Colour_Blue=0"], ["Colour_Blue=1"], []),
    "Look": (["Look=0"], ["Look=2"], []),
    "Width": (["Width=0.2"], ["Width=0.8"], ["Look=1"]),
    "Room Light": (["Room Light=0"], ["Room Light=0.5"], []),
    "Slices": (["Slices=0"], ["Slices=1"], []),
    "Preset File": ([], ["Preset File=" + OTHER_PRESET], []),
    "Order": (["Order=0"], ["Order=1"], []),
    "Rows": (["Rows=2"], ["Rows=3"], ["Slices=1"]),
    "Columns": (["Columns=4"], ["Columns=5"], ["Slices=1"]),
}

# The About block: a text line and browser buttons, with no pixel to sweep.
# Audio is the host's FFT buffer: --kicks writes it, and Threshold is swept
# through it.
SKIP = {"About", "Project page", "Source on GitHub", "Support the work", "User guide", "Audio"}


def read_png(path):
    """Enough of PNG for xntest's own writer: 8-bit RGBA, filter 0 rows."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        out += raw[start + 1:start + 1 + stride]
    return bytes(out)


def difference(a, b):
    if len(a) != len(b):
        return 255.0
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def render(xntest, out, settings):
    args = [str(xntest), "--out", str(out), "--size", "320x180", "--frames", "152"]
    for setting in settings:
        args += list(setting) if isinstance(setting, list) else ["--set", setting]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"xntest failed: {' '.join(args)}\n{result.stderr.strip()}")
    return read_png(out)


def parameters(xntest):
    result = subprocess.run([str(xntest), "--list"], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"xntest --list failed: {result.stderr.strip()}")
    names = []
    for line in result.stdout.splitlines()[1:]:
        parts = re.split(r"\s{2,}", line.strip())
        if len(parts) >= 3 and parts[0].isdigit():
            names.append(parts[1].strip())
    return names


def sweep_one(xntest, scratch, name, bare):
    low, high, context = SWEEP[name]
    context = BASE + ([] if bare else context)
    tag = f"p{abs(hash(name))}"
    before = render(xntest, scratch / f"{tag}-a.png", context + low)
    after = render(xntest, scratch / f"{tag}-b.png", context + high)
    return name, difference(before, after)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--bare", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--floor", type=float, default=0.05,
                        help="mean 8-bit difference below which a control is 'barely alive'")
    args = parser.parse_args()

    build = args.build if args.build.is_absolute() else REPO / args.build
    xntest = build / "xntest"
    if not xntest.exists():
        print(f"{xntest} not found", file=sys.stderr)
        return 1

    declared = parameters(xntest)
    unknown = [n for n in declared if n not in SWEEP and n not in SKIP]
    if unknown:
        # A new parameter with no sweep is a hole, not a pass.
        print(f"no sweep defined for: {', '.join(unknown)}", file=sys.stderr)
        return 1
    unused = [n for n in SWEEP if n not in declared]
    if unused:
        print(f"the sweep names parameters the plugin does not have: {', '.join(unused)}", file=sys.stderr)
        return 1

    dead, weak = [], []
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda n: sweep_one(xntest, scratch, n, args.bare), [n for n in declared if n in SWEEP]))
    for name, delta in results:
        if delta == 0.0:
            dead.append(name)
            print(f"  DEAD {name:16s} both ends identical")
        elif delta < args.floor:
            weak.append(name)
            print(f"  WEAK {name:16s} mean delta {delta:.4f}")
        elif args.verbose:
            print(f"  ok   {name:16s} mean delta {delta:.3f}")

    print(f"{len(results)} parameters swept{' (bare: base context only)' if args.bare else ''}, "
          f"{len(dead)} dead, {len(weak)} barely alive")
    if dead or weak:
        if not args.bare:
            print("\nA parameter that changes nothing is usually a uniform name that does not match\n"
                  "the C++, or a setting nothing reads. Both are silent everywhere else.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
