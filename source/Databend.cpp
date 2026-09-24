#include "Databend.h"

#include "Controls.h"
#include "Diag.h"
#include "Model.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9). The symptom without it is an unknown-type error on
//ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace databendfx;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Databend >,// Create method
	"DB01",                   // Plugin unique ID of maximum length 4.
	"SW Databend",            // Plugin name
	2,                        // API major version number
	1,                        // API minor version number
	0,                        // Plugin major version number
	1,                        // Plugin minor version number
	FF_EFFECT,                // Plugin type
	"The raster as a PCM stream through audio effects.\n\nThe picture is laid out as an audio editor would read the file -- planar or interleaved, row by row, with a row stride -- and real delay-line effects run on that stream: an echo comes back lines down and samples across and marches on a slant with feedback; a flanger is a comb across the scanlines; a phaser smears the picture rightwards and keeps its energy; a pitch shifter stretches the scan inside each grain. The sample format decides what happens past full scale: 8-bit wraps, float clips.\n\nStart with Delay Samples and Feedback; switch Layout for the colour shift.",// Plugin description
	"Databend FFGL effect"    // About
);

namespace
{
/// glGetString returns nullptr with no current context; a log line must never
/// be the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}
} // namespace

//---------------------------------------------------------------------------
Databend::Databend()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//No clock: nothing here moves with the host's time. The LFOs run on the
	//file's own clock, carried across frames by a frame counter, which is
	//the same in the harness as in a host.
	SetTimeSupported( false );

	//---------------------------------------------------------------------
	// Defaults: a PLANAR 8-bit file that CLIPS, an echo three lines and 24
	// samples down at 7/16 feedback and half mix, a light flanger, a
	// four-stage phaser at a third, the pitch shifter off. Judged on twelve
	// of Resolume's bundled demo clips through the harness (AGENTS.md): with
	// Wrap every phaser overshoot wrapped and eight of twelve clips flooded;
	// with Interleaved the hue -- which lives at the stream's period-3
	// frequency -- was rotated by every effect and every clip went pink.
	// Both are the mechanism, and both are one click away.
	//
	// Filled BEFORE any declaration: SetParamInfof reads its default out of
	// GetFloatParameter (compander's trap).
	//---------------------------------------------------------------------
	params[ PT_LAYOUT ]        = static_cast< float >( model::kPlanar );
	params[ PT_PADDING ]       = 0.0f;
	params[ PT_DIRECTION ]     = static_cast< float >( model::kRows );
	params[ PT_FORMAT ]        = static_cast< float >( model::kU8 );
	params[ PT_OVERFLOW ]      = static_cast< float >( model::kClip );

	params[ PT_ECHO_ON ]       = 1.0f;
	params[ PT_DELAY_LINES ]   = 3.0f;
	params[ PT_DELAY_SAMPLES ] = 24.0f;
	params[ PT_FEEDBACK ]      = 0.5f;//7/16
	params[ PT_ECHO_MIX ]      = 0.5f;

	params[ PT_FLANGER_ON ]    = 1.0f;
	params[ PT_BASE_DELAY ]    = 0.0625f;//4 samples
	params[ PT_DEPTH ]         = 0.375f;//24 samples
	params[ PT_RATE ]          = 0.5f;//1 Hz
	params[ PT_FLANGER_MIX ]   = 0.3f;

	params[ PT_PHASER_ON ]     = 1.0f;
	params[ PT_STAGES ]        = 4.0f;
	params[ PT_PHASER_RATE ]   = 0.35f;//0.4 Hz
	params[ PT_PHASER_DEPTH ]  = 0.5f;//pole 0.45..0.9
	params[ PT_PHASER_MIX ]    = 0.35f;

	params[ PT_PITCH_ON ]      = 0.0f;
	params[ PT_RATIO ]         = 0.25f;//1/sqrt 2
	params[ PT_GRAIN ]         = 0.5f;//512 samples
	params[ PT_PITCH_MIX ]     = 1.0f;

	params[ PT_MIX ]           = 1.0f;

	auto option = [ & ]( ParamID id, const char* name, int count, const char* const* names ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), names[ i ], static_cast< float >( i ) );
	};
	//A real integer with a real range: FF_TYPE_INTEGER is exempt from the
	//0..1 clamp of a STANDARD default.
	auto integer = [ & ]( ParamID id, const char* name, int low, int high ) {
		SetParamInfo( id, name, FF_TYPE_INTEGER, params[ id ] );
		SetParamRange( id, static_cast< float >( low ), static_cast< float >( high ) );
	};

	option( PT_LAYOUT, "Layout", model::kLayoutCount, model::kLayoutNames );
	integer( PT_PADDING, "Line Padding", 0, model::kPaddingMax );
	option( PT_DIRECTION, "Direction", model::kDirectionCount, model::kDirectionNames );
	option( PT_FORMAT, "Format", model::kFormatCount, model::kFormatNames );
	option( PT_OVERFLOW, "Overflow", model::kOverflowCount, model::kOverflowNames );

	SetParamInfo( PT_ECHO_ON, "Echo On", FF_TYPE_BOOLEAN, params[ PT_ECHO_ON ] >= 0.5f );
	integer( PT_DELAY_LINES, "Delay Lines", 0, model::kDelayLinesMax );
	integer( PT_DELAY_SAMPLES, "Delay Samples", 0, model::kDelaySamplesMax );
	SetParamInfof( PT_FEEDBACK, "Feedback", FF_TYPE_STANDARD );
	SetParamInfof( PT_ECHO_MIX, "Echo Mix", FF_TYPE_STANDARD );

	SetParamInfo( PT_FLANGER_ON, "Flanger On", FF_TYPE_BOOLEAN, params[ PT_FLANGER_ON ] >= 0.5f );
	SetParamInfof( PT_BASE_DELAY, "Base Delay", FF_TYPE_STANDARD );
	SetParamInfof( PT_DEPTH, "Depth", FF_TYPE_STANDARD );
	SetParamInfof( PT_RATE, "Rate", FF_TYPE_STANDARD );
	SetParamInfof( PT_FLANGER_MIX, "Flanger Mix", FF_TYPE_STANDARD );

	SetParamInfo( PT_PHASER_ON, "Phaser On", FF_TYPE_BOOLEAN, params[ PT_PHASER_ON ] >= 0.5f );
	integer( PT_STAGES, "Stages", model::kStagesMin, model::kStagesMax );
	SetParamInfof( PT_PHASER_RATE, "Phaser Rate", FF_TYPE_STANDARD );
	SetParamInfof( PT_PHASER_DEPTH, "Phaser Depth", FF_TYPE_STANDARD );
	SetParamInfof( PT_PHASER_MIX, "Phaser Mix", FF_TYPE_STANDARD );

	SetParamInfo( PT_PITCH_ON, "Pitch On", FF_TYPE_BOOLEAN, params[ PT_PITCH_ON ] >= 0.5f );
	SetParamInfof( PT_RATIO, "Ratio", FF_TYPE_STANDARD );
	SetParamInfof( PT_GRAIN, "Grain", FF_TYPE_STANDARD );
	SetParamInfof( PT_PITCH_MIX, "Pitch Mix", FF_TYPE_STANDARD );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//SetParamGroup collapses consecutive ids under one header, so each group
	//is a contiguous run of the enum.
	for( FFUInt32 i = PT_LAYOUT; i <= PT_OVERFLOW; ++i )
		SetParamGroup( i, "Stream" );
	for( FFUInt32 i = PT_ECHO_ON; i <= PT_ECHO_MIX; ++i )
		SetParamGroup( i, "Echo" );
	for( FFUInt32 i = PT_FLANGER_ON; i <= PT_FLANGER_MIX; ++i )
		SetParamGroup( i, "Flanger" );
	for( FFUInt32 i = PT_PHASER_ON; i <= PT_PHASER_MIX; ++i )
		SetParamGroup( i, "Phaser" );
	for( FFUInt32 i = PT_PITCH_ON; i <= PT_PITCH_MIX; ++i )
		SetParamGroup( i, "Pitch" );
	SetParamGroup( PT_MIX, "Output" );

	// The About block. Declared inline: SetParamInfo is protected on
	// CFFGLPlugin and nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Databend effect" );
	diag::init();
}

//---------------------------------------------------------------------------
FFResult Databend::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	} const stages[] = {
		{ &streamShader, shaders::Stream(), "stream" },
		{ &echoShader, shaders::Echo(), "echo" },
		{ &flangerShader, shaders::Flanger(), "flanger" },
		{ &phaserStateShader, shaders::PhaserState(), "phaser state" },
		{ &phaserShader, shaders::Phaser(), "phaser" },
		{ &pitchShader, shaders::Pitch(), "pitch" },
		{ &displayShader, shaders::Display(), "display" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( shaders::kVertex, stage.fragment.c_str() ) )
			continue;
		//FF_FAIL is invisible to an operator: the effect simply does nothing.
		//This line is the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Databend: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	frameIndex = 0;
	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Databend::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	//The host's viewport, before anything of ours changes it:
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	const int width  = static_cast< int >( picture.Width );
	const int height = static_cast< int >( picture.Height );
	const uint32_t frame = frameIndex++;

	//---------------------------------------------------------------------
	// The settings, in physical units.
	//---------------------------------------------------------------------
	const int layout      = controls::OptionIndex( params[ PT_LAYOUT ], model::kLayoutCount );
	const int padding     = controls::Integer( params[ PT_PADDING ], 0, model::kPaddingMax );
	const int direction   = controls::OptionIndex( params[ PT_DIRECTION ], model::kDirectionCount );
	const int format      = controls::OptionIndex( params[ PT_FORMAT ], model::kFormatCount );
	const int overflow    = controls::OptionIndex( params[ PT_OVERFLOW ], model::kOverflowCount );
	const double code     = model::CodeOf( format );

	const bool echoOn     = params[ PT_ECHO_ON ] >= 0.5f;
	const int delayLines  = controls::Integer( params[ PT_DELAY_LINES ], 0, model::kDelayLinesMax );
	const int delaySamples = controls::Integer( params[ PT_DELAY_SAMPLES ], 0, model::kDelaySamplesMax );
	const double feedback = controls::Feedback( params[ PT_FEEDBACK ] );
	const double echoMix  = controls::Mix( params[ PT_ECHO_MIX ] );

	const bool flangerOn  = params[ PT_FLANGER_ON ] >= 0.5f;
	const double baseDelay = controls::FlangerSamples( params[ PT_BASE_DELAY ] );
	const double depth    = controls::FlangerSamples( params[ PT_DEPTH ] );
	const int periodF     = controls::LfoPeriod( params[ PT_RATE ] );
	const double flangerMix = controls::Mix( params[ PT_FLANGER_MIX ] );

	const bool phaserOn   = params[ PT_PHASER_ON ] >= 0.5f;
	const int stages      = controls::Integer( params[ PT_STAGES ], model::kStagesMin, model::kStagesMax );
	const int periodP     = controls::LfoPeriod( params[ PT_PHASER_RATE ] );
	const double poleMin  = controls::PoleMin( params[ PT_PHASER_DEPTH ] );
	const double sMax     = controls::SMax( params[ PT_PHASER_DEPTH ] );
	const double phaserMix = controls::Mix( params[ PT_PHASER_MIX ] );

	const bool pitchOn    = params[ PT_PITCH_ON ] >= 0.5f;
	const double ratio    = controls::Ratio( params[ PT_RATIO ] );
	const int grain       = controls::Grain( params[ PT_GRAIN ] );
	const double pitchMix = controls::Mix( params[ PT_PITCH_MIX ] );

	//---------------------------------------------------------------------
	// Geometry: the file's layout. The two layout perturbations are
	// applied here, because they change the buffers' shape: the harness's
	// expectation is still the Layout and the Line Padding it set.
	//---------------------------------------------------------------------
	const int usedLayout  = ( perturb & model::kPerturbLayoutPlanar ) ? model::kPlanar : layout;
	const int usedPadding = ( perturb & model::kPerturbNoPadding ) ? 0 : padding;
	const model::Geometry g = model::GeometryOf( width, height, usedLayout, direction, usedPadding );
	const int delay       = delayLines * g.stride + delaySamples;
	const int echoTaps    = model::EchoTaps( feedback, code );

	//The phaser's input can be larger than the file's samples: the echo adds
	//up to m_e g / ( 1 - g ) of a unit input.
	double magnitude = 1.0;
	if( echoOn && feedback > 0.0 )
		magnitude *= 1.0 + echoMix * feedback / ( 1.0 - feedback );
	//(the flanger is a crossfade: it cannot enlarge the signal)
	int window = model::PhaserWindow( stages, model::kPoleMax, sMax, magnitude, static_cast< double >( g.total ), code, phaserMix, model::kChunk );
	if( perturb & model::kPerturbShortWindow )
		window = std::max( 1, window / 4 );

	//The LFOs' phase at this frame's first sample, as if the clip were one
	//file: frame x total samples, reduced in 64 bits. Exact.
	const int offsetF = static_cast< int >( ( static_cast< int64_t >( frame ) * g.total ) % periodF );
	const int offsetP = static_cast< int >( ( static_cast< int64_t >( frame ) * g.total ) % periodP );

	chosen.geometry     = g;
	chosen.echoTaps     = echoTaps;
	chosen.phaserWindow = window;
	chosen.lfoPeriodF   = periodF;
	chosen.lfoPeriodP   = periodP;
	chosen.lfoOffsetF   = offsetF;
	chosen.lfoOffsetP   = offsetP;

	//---------------------------------------------------------------------
	// Buffers. Every allocation happens here, before anything binds a
	// texture: FFGLFBO::Initialise sizes its colour texture under a scoped
	// binding, and every ffglex Scoped* binding CLEARS to 0 on exit.
	//---------------------------------------------------------------------
	const int64_t boundaries = ( g.total + model::kChunk - 1 ) / model::kChunk;
	const int stateRows      = static_cast< int >( ( boundaries + model::kStateWidth - 1 ) / model::kStateWidth );
	if( !streams[ 0 ].Ensure( g.stride, g.rows, GL_R32F, PassBuffer::Sampling::Nearest )
	    || !streams[ 1 ].Ensure( g.stride, g.rows, GL_R32F, PassBuffer::Sampling::Nearest )
	    || ( phaserOn && !states.Ensure( model::kStateWidth, stateRows ) ) )
	{
		diag::error( "could not allocate the stream buffers: " + std::to_string( g.stride ) + " samples x " + std::to_string( g.rows ) + " rows" );
		return FF_FAIL;
	}

	auto setLibrary = [ & ]( FFGLShader& shader ) {
		shader.Set( "Layout", usedLayout );
		shader.Set( "Lines", g.lines );
		shader.Set( "Pixels", g.pixels );
		shader.Set( "Stride", g.stride );
		shader.Set( "Rows", g.rows );
		shader.Set( "Perturb", perturb );
	};

	//---------------------------------------------------------------------
	// 1. The stream.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( streams[ 0 ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		streams[ 0 ].ResizeViewPort();
		ScopedShaderBinding shader( streamShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( picture.Handle );
		setLibrary( streamShader );
		streamShader.Set( "InputTexture", 0 );
		streamShader.Set( "InH", height );
		streamShader.Set( "Direction", direction );
		streamShader.Set( "Signed", model::IsSigned( format ) ? 1 : 0 );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 2. The effects, each from one buffer into the other. A bypassed
	//    effect is skipped: the next one reads what the last one wrote.
	//---------------------------------------------------------------------
	int current = 0;
	auto effect = [ & ]( FFGLShader& shader, auto&& uniforms ) {
		const int target = 1 - current;
		ScopedFBOBinding fbo( streams[ target ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		streams[ target ].ResizeViewPort();
		ScopedShaderBinding binding( shader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( streams[ current ].TextureID() );
		setLibrary( shader );
		shader.Set( "In", 0 );
		uniforms();
		quad.Draw();
		current = target;
	};

	if( echoOn )
		effect( echoShader, [ & ] {
			echoShader.Set( "Delay", delay );
			echoShader.Set( "Taps", echoTaps );
			echoShader.Set( "Gain", static_cast< float >( feedback ) );
			echoShader.Set( "EchoMix", static_cast< float >( echoMix ) );
		} );
	if( flangerOn )
		effect( flangerShader, [ & ] {
			flangerShader.Set( "Base", static_cast< float >( baseDelay ) );
			flangerShader.Set( "Width", static_cast< float >( depth ) );
			flangerShader.Set( "Period", periodF );
			flangerShader.Set( "Offset", offsetF );
			flangerShader.Set( "FlangerMix", static_cast< float >( flangerMix ) );
		} );
	if( phaserOn )
	{
		auto phaserUniforms = [ & ]( FFGLShader& shader ) {
			shader.Set( "Stages", stages );
			shader.Set( "Window", window );
			shader.Set( "Chunk", model::kChunk );
			shader.Set( "StateW", model::kStateWidth );
			shader.Set( "PoleTop", static_cast< float >( model::kPoleMax ) );
			shader.Set( "PoleMin", static_cast< float >( poleMin ) );
			shader.Set( "Period", periodP );
			shader.Set( "Offset", offsetP );
		};
		//The state pass: one fragment per chunk boundary, into the three
		//attachments of the state buffer. Raw binds, cleared afterwards.
		{
			glBindFramebuffer( GL_FRAMEBUFFER, states.FBO() );
			glViewport( 0, 0, states.Width(), states.Height() );
			ScopedShaderBinding binding( phaserStateShader.GetGLID() );
			glActiveTexture( GL_TEXTURE0 );
			glBindTexture( GL_TEXTURE_2D, streams[ current ].TextureID() );
			setLibrary( phaserStateShader );
			phaserStateShader.Set( "In", 0 );
			phaserUniforms( phaserStateShader );
			quad.Draw();
			glBindTexture( GL_TEXTURE_2D, 0 );
			glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		}
		//The main pass. Raw binds again: a Scoped* binding made inside the
		//uniform lambda would be CLEARED at the lambda's end, before the draw
		//(every ffglex Scoped* clears to 0 on exit) -- the driver then reads a
		//zero texture for the states and says so only in a log line.
		{
			const int target = 1 - current;
			glBindFramebuffer( GL_FRAMEBUFFER, streams[ target ].GetGLID() );
			streams[ target ].ResizeViewPort();
			ScopedShaderBinding binding( phaserShader.GetGLID() );
			glActiveTexture( GL_TEXTURE0 );
			glBindTexture( GL_TEXTURE_2D, streams[ current ].TextureID() );
			for( int q = 0; q < 3; ++q )
			{
				glActiveTexture( GL_TEXTURE1 + q );
				glBindTexture( GL_TEXTURE_2D, states.Texture( q ) );
			}
			setLibrary( phaserShader );
			phaserUniforms( phaserShader );
			phaserShader.Set( "In", 0 );
			phaserShader.Set( "State0", 1 );
			phaserShader.Set( "State1", 2 );
			phaserShader.Set( "State2", 3 );
			phaserShader.Set( "PhaserMix", static_cast< float >( phaserMix ) );
			quad.Draw();
			for( int q = 3; q >= 0; --q )
			{
				glActiveTexture( GL_TEXTURE0 + q );
				glBindTexture( GL_TEXTURE_2D, 0 );
			}
			glBindFramebuffer( GL_FRAMEBUFFER, 0 );
			current = target;
		}
	}
	if( pitchOn )
		effect( pitchShader, [ & ] {
			pitchShader.Set( "Grain", grain );
			pitchShader.Set( "Ratio", static_cast< float >( ratio ) );
			pitchShader.Set( "PitchMix", static_cast< float >( pitchMix ) );
		} );

	//---------------------------------------------------------------------
	// 3. Display.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( displayShader.GetGLID() );
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding input( picture.Handle );
		ScopedSamplerActivation s1( 1 );
		Scoped2DTextureBinding last( streams[ current ].TextureID() );

		setLibrary( displayShader );
		displayShader.Set( "InputTexture", 0 );
		displayShader.Set( "Final", 1 );
		displayShader.Set( "InW", width );
		displayShader.Set( "InH", height );
		displayShader.Set( "Direction", direction );
		displayShader.Set( "Format", format );
		displayShader.Set( "Wrap", overflow == model::kWrap ? 1 : 0 );
		displayShader.Set( "VpX", hostViewport[ 0 ] );
		displayShader.Set( "VpY", hostViewport[ 1 ] );
		displayShader.Set( "VpW", hostViewport[ 2 ] );
		displayShader.Set( "VpH", hostViewport[ 3 ] );
		displayShader.Set( "MixAmount", params[ PT_MIX ] );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Databend::DeInitGL()
{
	streamShader.FreeGLResources();
	echoShader.FreeGLResources();
	flangerShader.FreeGLResources();
	phaserStateShader.FreeGLResources();
	phaserShader.FreeGLResources();
	pitchShader.FreeGLResources();
	displayShader.FreeGLResources();
	quad.Release();

	streams[ 0 ].Destroy();
	streams[ 1 ].Destroy();
	states.Destroy();
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Databend::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Databend::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

char* Databend::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Databend::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only; it has to say so
	// successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}
