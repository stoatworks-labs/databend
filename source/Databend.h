#pragma once

#include "Model.h"
#include "PassBuffer.h"
#include "StateBuffer.h"

#include <FFGLSDK.h>

#include <cstdint>
#include <string>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

/**
	Databend -- the raster as a PCM stream through audio effects, as an
	FFGL effect.

	**The one idea.** Databending is opening a picture in an audio editor
	and running audio effects on it. The editor sees one long stream of
	samples in the order the file lays them out, and a delay-line effect on
	that stream does not know where the lines are: an echo of D = kL + m
	samples comes back k lines down and m samples across, with feedback the
	echoes march down the picture on a slant, a flanger is a comb across the
	scanlines, a phaser smears the picture rightwards keeping its energy, a
	pitch shifter stretches the scan inside each grain and skips or repeats
	at the seams -- and the file's layout and sample format are part of the
	mechanism: an interleaved delay that is not a multiple of three comes
	back in the wrong colour, and an 8-bit file wraps past full scale.

	**Seven passes over two R32F stream buffers.** The stream pass lays the
	picture out as the file; echo, flanger, phaser and pitch each read one
	buffer and write the other (a bypassed effect is skipped, not copied);
	the display exports the last buffer to the Format and puts it back on
	the picture. The phaser is a windowed restart with a proved bound, run
	once per chunk boundary into a state buffer and continued from there;
	the echo is a finite sum truncated below one code. Nothing carries across
	frames but a frame counter for the LFOs' phase, so a resize cannot
	lose anything. See AGENTS.md.
*/
class Databend : public CFFGLPlugin
{
public:
	Databend();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//--- test hooks. Read by dbtest; the plugin's own operation never uses
	//--- them, and the perturbation is always 0 outside the harness.

	/// Negative-control hooks, a bitmask of `model::Perturb`.
	void SetPerturbForTest( int bits )
	{
		perturb = bits;
	}

	/// The frame counter the next ProcessOpenGL will phase the LFOs with.
	uint32_t FrameIndexForTest() const
	{
		return frameIndex;
	}

	/// What the last ProcessOpenGL chose: the stream's geometry, the echo's
	/// tap count and the phaser's window, so the harness can print the
	/// settings it is holding the picture to.
	struct Chosen
	{
		databendfx::model::Geometry geometry;
		int echoTaps     = 0;
		int phaserWindow = 0;
		int lfoPeriodF   = 0;
		int lfoPeriodP   = 0;
		int lfoOffsetF   = 0;
		int lfoOffsetP   = 0;
	};
	const Chosen& ChosenForTest() const
	{
		return chosen;
	}

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Stream
		PT_LAYOUT,
		PT_PADDING,
		PT_DIRECTION,
		PT_FORMAT,
		PT_OVERFLOW,

		//Echo
		PT_ECHO_ON,
		PT_DELAY_LINES,
		PT_DELAY_SAMPLES,
		PT_FEEDBACK,
		PT_ECHO_MIX,

		//Flanger
		PT_FLANGER_ON,
		PT_BASE_DELAY,
		PT_DEPTH,
		PT_RATE,
		PT_FLANGER_MIX,

		//Phaser
		PT_PHASER_ON,
		PT_STAGES,
		PT_PHASER_RATE,
		PT_PHASER_DEPTH,
		PT_PHASER_MIX,

		//Pitch
		PT_PITCH_ON,
		PT_RATIO,
		PT_GRAIN,
		PT_PITCH_MIX,

		//Output
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	ffglex::FFGLShader streamShader;
	ffglex::FFGLShader echoShader;
	ffglex::FFGLShader flangerShader;
	ffglex::FFGLShader phaserStateShader;
	ffglex::FFGLShader phaserShader;
	ffglex::FFGLShader pitchShader;
	ffglex::FFGLShader displayShader;
	ffglex::FFGLScreenQuad quad;

	databendfx::PassBuffer streams[ 2 ];///< the stream, ping-ponged between effects
	databendfx::StateBuffer states;     ///< the cascade's state at every chunk boundary

	uint32_t frameIndex = 0;///< phases the LFOs; the only state across frames

	int perturb = 0;
	Chosen chosen;

	/// Zero-initialised: the About block's ids are never stored to.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
