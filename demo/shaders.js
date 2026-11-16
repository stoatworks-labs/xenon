// GENERATED from source/Shaders.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape allowed is \` for a backtick inside a comment.
export const VERSION = '#version 410 core\n';

// kQuadVertex, source/Shaders.cpp
export const QUAD_VERTEX = `
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

// kLightVertex, source/Shaders.cpp
export const LIGHT_VERTEX = `
layout( location = 0 ) in vec2 vCorner;
layout( location = 1 ) in vec4 vRunsIn;
layout( location = 2 ) in vec2 vExtraIn;

uniform vec2 Size;

flat out vec4 runs;
flat out vec2 extra;

void main()
{
	gl_Position = vec4( vCorner / Size * 2.0 - 1.0, 0.0, 1.0 );
	runs        = vRunsIn;
	extra       = vExtraIn;
}
`;

// kCommon, source/Shaders.cpp
export const COMMON = `
vec3 toLinear( vec3 code )
{
	vec3 c = max( code, vec3( 0.0 ) );
	return mix( c / 12.92, pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) ), step( vec3( 0.04045 ), c ) );
}
vec3 toCode( vec3 light )
{
	vec3 l = max( light, vec3( 0.0 ) );
	return mix( l * 12.92, 1.055 * pow( l, vec3( 1.0 / 2.4 ) ) - 0.055, step( vec3( 0.0031308 ), l ) );
}
`;

// kLightFragment, source/Shaders.cpp
export const LIGHT_FRAGMENT = `
uniform sampler2D Edges;
uniform int Outline;  //1: only within Band of the region's edge
uniform float Band;   //output pixels
uniform int Hooks;    //the harness's wrong models (Shaders.h); 0 shipped

flat in vec4 runs;
flat in vec2 extra;

out vec4 fragColour;

vec4 edgeAt( int i )
{
	return texelFetch( Edges, ivec2( i & 1023, i >> 10 ), 0 );
}

//Even-odd: an edge counts when it straddles the pixel centre's row with the
//half-open rule (one end strictly above) and crosses to its right. The same
//test, in the same form, as SliceMap.cpp's InsidePolygon, which the harness
//holds the picture to. With Outline, also the nearest edge, squared.
void scanEdges( int start, int count, vec2 p, inout bool inside, inout float nearest )
{
	for( int i = 0; i < count; ++i )
	{
		vec4 e = edgeAt( start + i );
		vec2 a = e.xy;
		vec2 b = e.zw;
		if( ( a.y > p.y ) != ( b.y > p.y ) )
		{
			float t = ( p.y - a.y ) / ( b.y - a.y );
			if( p.x < a.x + t * ( b.x - a.x ) )
				inside = !inside;
		}
		if( Outline == 1 )
		{
			vec2 ab    = b - a;
			float len2 = dot( ab, ab );
			float s    = len2 > 0.0 ? clamp( dot( p - a, ab ) / len2, 0.0, 1.0 ) : 0.0;
			vec2 off   = a + s * ab - p;
			nearest    = min( nearest, dot( off, off ) );
		}
	}
}

void main()
{
	vec2 p        = gl_FragCoord.xy;
	bool inside   = false;
	float nearest = 1e30;
	scanEdges( int( runs.x ), int( runs.y ), p, inside, nearest );
	int maskMode = int( extra.x + 0.5 );
	if( maskMode != 0 )
	{
		//A point inside both the slice and its mask's kept side is nearest
		//the region's edge at the nearer of the two outlines.
		bool inMask = false;
		scanEdges( int( runs.z ), int( runs.w ), p, inMask, nearest );
		inside = inside && ( maskMode == 1 ? inMask : !inMask );
	}
	if( ( Hooks & 1 ) == 0 && ( !inside || ( Outline == 1 && nearest > Band * Band ) ) )
		discard;
	fragColour = vec4( extra.y, 0.0, 0.0, 0.0 );
}
`;

// kCompositeFragment, source/Shaders.cpp
export const COMPOSITE_FRAGMENT = `
uniform sampler2D InputTexture;
uniform sampler2D Light;
uniform vec2 MaxUV;
uniform ivec2 ViewOrigin;
uniform vec3 Colour;  //linear
uniform float Room;
uniform int Look;     //0 Fill, 1 Outline, 2 Gate
uniform int Hooks;    //the harness's wrong models (Shaders.h); 0 shipped

in vec2 uv;
out vec4 fragColour;

void main()
{
	vec4 src      = texture( InputTexture, uv * MaxUV );
	float light   = max( texelFetch( Light, ivec2( gl_FragCoord.xy ) - ViewOrigin, 0 ).r, 0.0 );
	vec3 surface  = src.a > 0.0 ? toLinear( src.rgb / src.a ) : vec3( 0.0 );
	vec3 lamp     = Colour * light;
	vec3 rgb;
	float alpha;
	if( Look == 2 )
	{
		//The clip is a surface, and the strobe lights it: no light where
		//there is no clip, and no new matter either.
		rgb   = surface * ( Room + lamp );
		alpha = src.a;
	}
	else
	{
		//The slice is a lamp in front of the clip: its light adds to the
		//clip's (premultiplied, at Room Light) and makes the pixel opaque.
		alpha = ( Hooks & 2 ) != 0 ? src.a : max( src.a, min( 1.0, max( lamp.r, max( lamp.g, lamp.b ) ) ) );
		rgb   = alpha > 0.0 ? ( surface * Room * src.a + lamp ) / alpha : vec3( 0.0 );
	}
	vec3 code  = clamp( toCode( rgb ), 0.0, 1.0 );
	fragColour = vec4( code * alpha, alpha );
}
`;
