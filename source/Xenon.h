#pragma once

#include "Audio.h"
#include "Controls.h"
#include "PassBuffer.h"
#include "Rig.h"
#include "Shaders.h"
#include "SliceMap.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <mutex>
#include <string>
#include <vector>

namespace xenon
{
/// A head as the light pass draws it: its region in output pixels (y up) and
/// where its edges sit in the edge texture.
struct HeadPixels
{
	std::vector< Point > outline;
	std::vector< Point > mask;
	int maskMode = shaders::kMaskNone;
	double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;///< bounds, padded a pixel, clipped to the frame
	int outlineStart = 0, outlineCount = 0;
	int maskStart = 0, maskCount = 0;
};

/// The plugin's own hooks for the negative controls, above the shaders'
/// (shaders::Hook, the low bits, passed to both passes). A shipped plugin
/// sets 0.
enum PluginHook : int
{
	kHookResizeForgets = 1 << 8///< a new raster empties every bank
};

/**
    The plugin.

    Three models in sequence, each in its own file: the slices (SliceMap.h:
    the operator's Advanced Output preset, followed and re-read when Arena
    saves it), the rig (Rig.h: the chase, each head's bank and tube, and what
    each frame catches of the light), and the picture (Shaders.h: the light
    laid into each head's region, then composited over the clip). See
    AGENTS.md for why each is built the way it is.
*/
class XenonPlugin : public CFFGLPlugin
{
public:
	XenonPlugin();

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// The Preset File's path, and the About line accepting its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// on a fresh instance and deletes the instance if one fails, and
	/// CFFGLPlugin's SetTextParameter is a stub that returns exactly that
	/// failure -- so without this override no real host can load the plugin,
	/// while every offline harness here carries on passing.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	FFResult SetTime( double time ) override;
	void SetBeatInfo( float bpm, float barPhase ) override;

	//-------------------------------------------------------------------
	// For the harness. Nothing in the plugin's own operation calls these;
	// they exist so `xntest` can measure what the shipping passes produced,
	// rather than a copy of it computed for the test.
	//-------------------------------------------------------------------

	/// The offline harness DECLARES its clock unit (seconds) rather than
	/// letting the plugin vote on it against the wall clock.
	void SetClockScaleForTest( double scale )
	{
		clockScale = scale;
	}
	void SetHooksForTest( int bits )
	{
		hooks = bits;
	}
	void SetRigHooksForTest( int bits )
	{
		rig.SetHooksForTest( bits );
	}
	void SetReaderHooksForTest( int bits )
	{
		source.SetReaderHooksForTest( bits );
	}
	void SetDocumentsForTest( const std::string& folder )
	{
		source.SetDocumentsForTest( folder );
	}
	void SetUnprimedForTest( bool unprimed )
	{
		analyser.SetUnprimedForTest( unprimed );
	}

	const Rig& RigState() const
	{
		return rig;
	}
	const MapSource& Source() const
	{
		return source;
	}
	/// Each head's light on the last frame, Intensity included.
	const std::vector< float >& LastLight() const
	{
		return light;
	}
	const std::vector< HeadPixels >& Pixels() const
	{
		return pixels;
	}
	/// The chase's order on the last frame (position -> head).
	const std::vector< int >& Order() const
	{
		return order;
	}
	/// Outline's band on the last frame, output pixels.
	double LastBand() const
	{
		return band;
	}
	long long FrameCount() const
	{
		return frameCount;
	}
	double Now() const
	{
		return now;
	}
	/// Map reloads the plugin has acted on (the harness's --reread).
	int MapChanges() const
	{
		return mapChanges;
	}

private:
	void UpdateClock();
	void UpdateAudio( double dt );
	bool rebuildGeometry( int width, int height );

	float params[ PT_COUNT ] = {};
	std::mutex textMutex;
	std::string presetFile;
	char textReturn[ 4096 ] = {};

	ffglex::FFGLShader lightShader;
	ffglex::FFGLShader compositeShader;
	ffglex::FFGLScreenQuad quad;
	GLuint vertexArray  = 0;
	GLuint vertexBuffer = 0;
	GLuint edgeTexture  = 0;
	PassBuffer lightBuffer;

	MapSource source;
	Rig rig;
	audio::Analyser analyser;
	std::vector< HeadPixels > pixels;
	std::vector< float > exposure;
	std::vector< float > light;
	std::vector< int > order;
	std::vector< float > vertices;
	bool geometryDirty = true;
	int geometryWidth  = 0;
	int geometryHeight = 0;
	double band        = 0.0;
	int mapChanges     = 0;

	//-------------------------------------------------------------------
	// Time. See rosette: the host's clock unit is voted on against the wall
	// clock, because Resolume has sent both seconds and milliseconds.
	//-------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	double clockScale   = 0.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	int clockFrames     = 0;
	double now          = 0.0;
	double lastNow      = -1.0;
	bool settledJump    = false;
	long long frameCount = 0;

	//-------------------------------------------------------------------
	// The beat, as SetBeatInfo last said.
	//-------------------------------------------------------------------
	float bpm          = 120.0f;
	float barPhase     = 0.0f;
	float lastBarPhase = -1.0f;
	bool hostBeatSeen  = false;

	int firePresses = 0;
	bool fireHeld   = false;
	int hooks       = 0;
};

} // namespace xenon
