#pragma once

#include <string>

/**
	The passes.

	1. **light** -- one quad per lit head, its bounding box in output pixels,
	   drawn additively into a float buffer the size of the output. Each
	   fragment decides whether its pixel centre is inside the head's region
	   (even-odd crossings over the head's run of edges, and its mask's) and,
	   for Outline, whether it lies within Band of the region's edge; if so it
	   adds the head's light. Hard edges, no antialiasing: Arena crops a
	   slice exactly, and the harness counts lit pixels outside a slice.
	2. **composite** -- the clip and the light buffer to the host, in linear
	   light: Fill and Outline add the light over the clip at Room Light,
	   Gate lights the clip with it.

	The edges live in a 1024-wide RGBA32F texture, one texel an edge
	(x0, y0, x1, y1) in output pixels, y up. A head's quad carries the start
	and count of its outline's run and its mask's as flat attributes.

	The fragment shaders are assembled from pieces at InitGL by `Assemble()`,
	the one place the order is written; tools/glslc.sh mirrors it.
*/
namespace xenon::shaders
{

extern const char* const kVersion;
extern const char* const kQuadVertex;
extern const char* const kLightVertex;
extern const char* const kCommon;
extern const char* const kLightFragment;
extern const char* const kCompositeFragment;

enum class Pass
{
	Light,
	Composite
};
std::string Assemble( Pass pass );

/// The light pass's vertex: position (2, output pixels), the runs (4:
/// outline start, outline count, mask start, mask count), extra (2: mask
/// mode, light).
constexpr int kVertexFloats = 8;
/// Edge texture width.
constexpr int kEdgeRow = 1024;

/// Wrong models for the harness's negative controls, as the shaders' `Hooks`
/// uniform. A shipped plugin sets 0.
enum Hook : int
{
	kHookQuadUnclipped = 1 << 0,///< light: the whole quad lit, no inside test (the bounding box)
	kHookClipAlpha     = 1 << 1 ///< composite: the clip's alpha passed through under Fill and Outline
};

/// Mask modes, as the light pass reads them.
constexpr int kMaskNone    = 0;
constexpr int kMaskInside  = 1;
constexpr int kMaskOutside = 2;

} // namespace xenon::shaders
