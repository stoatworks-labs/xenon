#!/usr/bin/env bash
#
# Mutation testing: change ONE character of the shipped code and require that a
# check fails. A check that still passes against a mutant was not looking at
# the code it claims to cover -- and a harness that compiled its own copy of
# the shaders would pass every GLSL mutant here.
#
# Each mutant is a copy of the tree in a temporary directory (the FFGL SDK is
# symlinked, not copied), built arm64-only, with only the named check run at
# CI's raster, under a time limit (a mutant can hang: honeydew's lesson; a
# timeout counts as caught).
#
#     tools/mutate.sh
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# file | the exact text | its one-character mutant | the check that must fail | what it is
# The fields are split on '|', so neither a target nor a mutant may contain one
# (conway's trap).
MUTANTS=(
	"source/Shaders.cpp|if( ( a.y > p.y ) != ( b.y > p.y ) )|if( ( a.y > p.y ) == ( b.y > p.y ) )|--inside|GLSL: an edge counted when it does NOT straddle the row, != -> =="
	"source/Shaders.cpp|float t = ( p.y - a.y ) / ( b.y - a.y );|float t = ( p.y + a.y ) / ( b.y - a.y );|--inside|GLSL: the crossing found from the sum of heights, - -> +"
	"source/Shaders.cpp|nearest > Band * Band|nearest > Band + Band|--inside|GLSL: Outline's band compared as 2 Band, not Band^2, * -> +"
	"source/Shaders.cpp|fragColour = vec4( extra.y, 0.0, 0.0, 0.0 );|fragColour = vec4( extra.x, 0.0, 0.0, 0.0 );|--charge|GLSL: the mask mode drawn as the light, y -> x"
	"source/Shaders.cpp|rgb   = surface * ( Room + lamp );|rgb   = surface * ( Room * lamp );|--alpha|GLSL: Gate's light multiplying the room light, + -> *"
	"source/Shaders.cpp|( surface * Room * src.a + lamp ) / alpha|( surface * Room * src.a - lamp ) / alpha|--alpha|GLSL: Fill's lamp taking light away, + -> -"
	"source/Rig.cpp|? q : q * q;|? q : q + q;|--charge|C++: a flash's energy 2q, not q^2, * -> +"
	"source/Rig.cpp|( boundary - p0 ) / ( p1 - p0 );|( boundary + p0 ) / ( p1 - p0 );|--chase|C++: the beat's place in its frame from the sum of phases, - -> +"
	"source/Rig.cpp|std::exp( -std::max( 0.0, t0 - f.time ) / tau )|std::exp( -std::max( 0.0, t0 + f.time ) / tau )|--energy|C++: a frame's catch measured from the sum of times, - -> +"
	"source/SliceMap.cpp|h.outline.push_back( { p.x / map.compWidth, p.y / map.compHeight } );|h.outline.push_back( { p.x / map.compWidth, p.y * map.compHeight } );|--reader|C++: y multiplied by the composition, not divided, / -> *"
	"source/SliceMap.cpp|b[ i ].x ) > kSameRegion|b[ i ].x ) < kSameRegion|--reader|C++: two regions the same only when their x differ, > -> <"
	"source/Xenon.cpp|out.push_back( { p.x * width, ( 1.0 - p.y ) * height } );|out.push_back( { p.x * width, ( 1.0 + p.y ) * height } );|--inside|C++: the map not flipped to GL's y up, - -> +"
	"source/Xenon.cpp|? double( height ) / map.compHeight : 1.0;|? double( height ) * map.compHeight : 1.0;|--inside|C++: Outline's Width scaled the wrong way, / -> *"
)

limit() {
	# macOS has no timeout(1): perl's alarm is enough.
	perl -e 'alarm shift; exec @ARGV' "$@"
}

caught=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r file original mutant check what <<<"$entry"
	tree="$WORK/tree"
	rm -rf "$tree"
	mkdir -p "$tree/external"
	cp -R "$REPO/source" "$REPO/tools" "$REPO/cmake" "$REPO/CMakeLists.txt" "$tree/"
	ln -s "$REPO/external/ffgl" "$tree/external/ffgl"

	python3 - "$tree/$file" "$original" "$mutant" <<'PY'
import sys, pathlib
path, original, mutant = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(original) != 1:
    sys.exit(f"mutation target found {text.count(original)} times in {path}: '{original}'")
if sum(a != b for a, b in zip(original, mutant)) != 1 or len(original) != len(mutant):
    sys.exit("a mutant must differ by exactly one character")
path.write_text(text.replace(original, mutant))
PY

	printf '\n== mutant: %s\n' "$what"
	cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
	cmake --build "$tree/build" --target xntest -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1
	status=0
	extra=()
	[[ "$check" == "--reader" || "$check" == "--pointer" ]] || extra=( --size 320x180 )
	limit 300 "$tree/build/xntest" "$check" "${extra[@]}" >"$WORK/log" 2>&1 || status=$?
	if [[ "$status" -eq 0 ]]; then
		printf '   FAIL  %s still PASSES -- the check does not cover this code\n' "$check"
	else
		printf '   ok    %s fails against the mutant (exit %d):\n' "$check" "$status"
		# A mutant that timed out printed no FAIL: grep finding none must not end the run.
		{ grep -E '^  FAIL' "$WORK/log" || true; } | head -2 | cut -c1-160 | sed 's/^/        /'
		caught=$(( caught + 1 ))
	fi
done

printf '\nmutants: %d, caught: %d\n' "${#MUTANTS[@]}" "$caught"
[[ "$caught" -eq "${#MUTANTS[@]}" ]]
