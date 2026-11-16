"""The demo's shaders and presets must be the plugin's, character for character.

    python3 demo/tools/check_shaders.py           compare; exit 1 on any drift
    python3 demo/tools/check_shaders.py --write   regenerate demo/shaders.js

Called from `tools/verify.sh`. Exit code 1 means a copy has drifted.

------------------------------------------------------------------- why

`demo/shaders.js` holds the GLSL pieces of `source/Shaders.cpp` (the two vertex
stages, the kCommon sRGB library, the light and composite fragments) plus
kVersion. That is two copies of the same text, and two copies drift --
quietly, because a strobe drawn by a *plausible* shader looks exactly like one
drawn by the right one. The whole claim of the page is that it runs the
plugin's own shaders, so the claim needs something enforcing it. `xntest`
drives the real plugin class and has never heard of this page, and
`tools/glslc.sh` compiles the C++ copies and never looks at the JS one.

`demo/presets/` holds copies of the Advanced Output presets in
`tests/presets/` that the page offers as Arena's saved map. They are the
fixtures the harness reads to the corners Arena wrote, so a copy that drifted
would put the page's heads somewhere the harness never checked. They are
compared byte for byte.

------------------------------------------------------------------- what it does

Pulls each piece out of `source/Shaders.cpp` and the matching backtick literal
out of `demo/shaders.js`, and compares them exactly -- no whitespace
normalisation, no comment stripping: a comment updated on one side only is
exactly the drift worth catching. `kVersion`, a plain string, is compared too.

A piece may be several adjacent raw strings (MSVC refuses one literal over
about 16 KB, C2026); this joins them as the compiler does, with nothing
between. None of xenon's pieces needs that today.

It also fails when Shaders.cpp grows a piece this script does not list (a new
pass the page would silently lack), and it checks the JOIN as well as the
pieces: `Assemble()` in Shaders.cpp is kVersion + the light fragment, or
kVersion + kCommon + the composite fragment; InitGL in Xenon.cpp builds each
vertex stage as kVersion + its piece and each fragment stage with Assemble().
demo/render.js must assemble them the same way. A change to either side's
assembly fails here and needs a reader.

The one transformation is a decode, not a normalisation: a backtick cannot
appear raw inside a template literal, so --write would escape one as \\`. This
undoes that escape and REJECTS any other backslash or any `${` on the JS side;
there are none in the C++ bodies, so either could only be somebody hiding a
difference. None of xenon's pieces has a backtick today.

`--write` produces shaders.js from the C++ by exactly that escape, so the copy
is spliced by this script and never typed.

------------------------------------------------------------------- what it cannot

Nothing here checks the PORTED half. demo/port.js is a hand translation of
Controls.cpp (every mapping, name, group and default), Rig.cpp (the chase, the
banks, the exposure), SliceMap.cpp (the XML reader, the slices, polygons,
curves, masks, the collapse and the head order) and the CPU half of
Xenon.cpp's ProcessOpenGL (the frame, the geometry and the quads). Only a
reader checks it. When you change one of those, change the port too.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.dont_write_bytecode = True

# JS constant, C++ symbol (all in source/Shaders.cpp), in the order they are
# declared there.
SHADERS = [
    ("QUAD_VERTEX", "kQuadVertex"),
    ("LIGHT_VERTEX", "kLightVertex"),
    ("COMMON", "kCommon"),
    ("LIGHT_FRAGMENT", "kLightFragment"),
    ("COMPOSITE_FRAGMENT", "kCompositeFragment"),
]

# How the pieces are joined, on each side. Each entry is (file, text that must
# appear in it verbatim). The C++ lines are the plugin's assembly; the JS lines
# are render.js's copy of it. A change to either is a change to what runs.
ASSEMBLY = [
    ("source/Shaders.cpp",
     "std::string Assemble( Pass pass )\n{\n"
     "\tstd::string out = kVersion;\n"
     "\tswitch( pass )\n"
     "\t{\n"
     "\tcase Pass::Light: out += kLightFragment; break;\n"
     "\tcase Pass::Composite:\n"
     "\t\tout += kCommon;\n"
     "\t\tout += kCompositeFragment;\n"
     "\t\tbreak;\n"
     "\t}\n"
     "\treturn out;\n}"),
    ("source/Xenon.cpp", "const std::string quadVertex  = std::string( shaders::kVersion ) + shaders::kQuadVertex;"),
    ("source/Xenon.cpp", "const std::string lightVertex = std::string( shaders::kVersion ) + shaders::kLightVertex;"),
    ("source/Xenon.cpp", '{ &lightShader, &lightVertex, shaders::Pass::Light, "light" },'),
    ("source/Xenon.cpp", '{ &compositeShader, &quadVertex, shaders::Pass::Composite, "composite" },'),
    ("source/Xenon.cpp", "const std::string fragment = shaders::Assemble( stage.pass );"),
    ("source/Xenon.cpp", "if( stage.shader->Compile( stage.vertex->c_str(), fragment.c_str() ) )"),
    ("demo/render.js", "const quadVertex = S.VERSION + S.QUAD_VERTEX;"),
    ("demo/render.js", "const lightVertex = S.VERSION + S.LIGHT_VERTEX;"),
    ("demo/render.js", "light: S.VERSION + S.LIGHT_FRAGMENT,"),
    ("demo/render.js", "composite: S.VERSION + S.COMMON + S.COMPOSITE_FRAGMENT,"),
    ("demo/render.js", "const light = new Program(gl, lightVertex, assemble.light, 'light', { attribs: LIGHT_ATTRIBS });"),
    ("demo/render.js", "const composite = new Program(gl, quadVertex, assemble.composite, 'composite');"),
]

# The presets the page offers, which must be the harness's fixtures byte for
# byte. tests/presets/AdvancedOutput-7.27.1.xml is a pointer file, not a
# preset, and the page has no use for it (it does not follow a pointer).
PRESETS = [
    "unmaptest-7.28.1-polygon-masks.xml",
    "unmaptest.xml",
    "output_map_1.xml",
    "LPP slicer proof A.xml",
    "LPP slicer proof B.xml",
]

HEADER = """// GENERATED from source/Shaders.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape allowed is \\` for a backtick inside a comment.
"""


def read(*parts, binary=False):
    with open(os.path.join(REPO, *parts), "rb" if binary else "r") as handle:
        return handle.read()


def cpp_version(source):
    match = re.search(r'const char\* const kVersion = "(.*?)";', source)
    return None if match is None else match.group(1)


def from_cpp(source, symbol):
    """The piece's text: its raw strings, joined as the compiler joins them.

    Returns (text, count of raw strings), or (None, 0) if the symbol is not
    there or what follows its `=` is not a run of adjacent raw strings ending
    in `;`.
    """
    match = re.search(r'const char\* const ' + symbol + r' = ((?:R"\(.*?\)"\s*)+);', source, re.S)
    if match is None:
        return None, 0
    parts = re.findall(r'R"\((.*?)\)"', match.group(1), re.S)
    rebuilt = re.sub(r'R"\(.*?\)"', "", match.group(1), flags=re.S)
    if rebuilt.strip() != "":
        return None, 0
    return "".join(parts), len(parts)


def cpp_pieces(source):
    """Every raw-string piece Shaders.cpp defines, by symbol."""
    return re.findall(r'const char\* const (k\w+) = R"\(', source)


def from_js(source, name):
    match = re.search(r'^export const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        line = body[: stray.start()].count("\n") + 1
        return None, f"backslash that is not an escaped backtick, at line {line}"
    if "${" in body:
        return None, "template substitution"
    return body.replace("\\`", "`"), None


def write(cpp):
    out = [HEADER]
    version = cpp_version(cpp)
    if version is None:
        print("FAIL  kVersion not found in source/Shaders.cpp")
        return 1
    # kVersion is "#version 410 core\n" in the C++, the \n an escape there.
    out.append(f"export const VERSION = '{version}';\n")
    for name, symbol in SHADERS:
        body, count = from_cpp(cpp, symbol)
        if body is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp (or not a run of adjacent raw strings)")
            return 1
        if "\\" in body or "${" in body:
            print(f"FAIL  {symbol} holds a backslash or ${{; the escape scheme cannot carry it")
            return 1
        joined = f", {count} adjacent raw strings joined" if count > 1 else ""
        out.append(f"\n// {symbol}, source/Shaders.cpp{joined}\nexport const {name} = `{body.replace('`', chr(92) + '`')}`;\n")
    with open(os.path.join(REPO, "demo", "shaders.js"), "w") as handle:
        handle.write("".join(out))
    print("wrote demo/shaders.js")
    return 0


def first_difference(a, b):
    left, right = a.splitlines(), b.splitlines()
    for i in range(max(len(left), len(right))):
        x = left[i] if i < len(left) else "<missing>"
        y = right[i] if i < len(right) else "<missing>"
        if x != y:
            return i + 1, x, y
    return None


def check(cpp, js):
    problems = 0
    version_cpp = cpp_version(cpp)
    version_js = re.search(r"^export const VERSION = '(.*?)';$", js, re.M)
    if version_cpp is None or version_js is None or version_cpp != version_js.group(1):
        print("FAIL  VERSION does not match kVersion")
        problems += 1
    else:
        print(f"ok    {'VERSION':<20} matches kVersion")

    listed = {symbol for _, symbol in SHADERS}
    for symbol in cpp_pieces(cpp):
        if symbol not in listed:
            print(f"FAIL  source/Shaders.cpp defines {symbol}, which this check does not list -- "
                  "add it to SHADERS (and to the page, or say on the page why not)")
            problems += 1

    for name, symbol in SHADERS:
        cpp_text, count = from_cpp(cpp, symbol)
        js_text, complaint = from_js(js, name)
        if cpp_text is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp (or not a run of adjacent raw strings)")
            problems += 1
        elif complaint is not None:
            print(f"FAIL  {name} in demo/shaders.js has a {complaint}")
            problems += 1
        elif js_text is None:
            print(f"FAIL  {name} not found in demo/shaders.js")
            problems += 1
        elif cpp_text == js_text:
            joined = f", {count} raw strings joined" if count > 1 else ""
            print(f"ok    {name:<20} matches {symbol} ({len(cpp_text)} chars{joined})")
        else:
            problems += 1
            print(f"FAIL  {name} has drifted from {symbol}")
            where = first_difference(cpp_text, js_text)
            if where:
                print(f"        first difference at line {where[0]}")
                print(f"          C++: {where[1]}")
                print(f"          js : {where[2]}")

    for path, text in ASSEMBLY:
        try:
            body = read(*path.split("/"))
        except FileNotFoundError:
            print(f"FAIL  {path} is missing")
            problems += 1
            continue
        if text in body:
            print(f"ok    assembly: {path} still says {text.splitlines()[0][:60]}")
        else:
            print(f"FAIL  assembly: {path} no longer says: {text.splitlines()[0]}")
            problems += 1

    for name in PRESETS:
        try:
            ours = read("demo", "presets", name, binary=True)
        except FileNotFoundError:
            print(f"FAIL  demo/presets/{name} is missing")
            problems += 1
            continue
        try:
            theirs = read("tests", "presets", name, binary=True)
        except FileNotFoundError:
            print(f"FAIL  tests/presets/{name} is gone: the page offers a fixture the harness no longer reads")
            problems += 1
            continue
        if ours == theirs:
            print(f"ok    preset {name} ({len(ours)} bytes) is the fixture's")
        else:
            print(f"FAIL  demo/presets/{name} differs from tests/presets/{name} -- copy the fixture across")
            problems += 1
    extra = sorted(set(os.listdir(os.path.join(REPO, "demo", "presets"))) - set(PRESETS))
    for name in extra:
        print(f"FAIL  demo/presets/{name} is not a listed fixture: the page would serve a preset nothing checks")
        problems += 1
    return problems


def main(argv):
    cpp = read("source", "Shaders.cpp")
    if "--write" in argv:
        return write(cpp)
    try:
        js = read("demo", "shaders.js")
    except FileNotFoundError:
        print("FAIL  demo/shaders.js is missing -- run demo/tools/check_shaders.py --write")
        return 1
    problems = check(cpp, js)
    print()
    if problems:
        print(f"{problems} problem(s) -- rerun demo/tools/check_shaders.py --write for a drifted shader copy "
              "(never edit shaders.js by hand), copy a drifted preset from tests/presets/; an assembly or "
              "unlisted-piece failure needs a reader")
        return 1
    print(f"all {len(SHADERS) + 1} shader pieces, their assembly and {len(PRESETS)} presets are the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
