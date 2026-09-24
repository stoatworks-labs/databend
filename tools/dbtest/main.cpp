/**
	dbtest -- render Databend offline, and read the stream back out of it.

	Where an echo lands and in which colour, where a flanger's ghost sits on
	every line, whether the phaser keeps a noise line's energy and matches a
	serial double run of the cascade, what an 8-bit file does past full
	scale: each has one right answer in the file's layout, the delay, the
	LFO and the format. Every check here drives the REAL plugin class
	through a headless GL context and measures the answer out of the
	picture it made:

		dbtest --out /tmp/frame.png     a picture, on the moving test card
		dbtest --list                   every parameter, its kind and default
		dbtest --echo                   a delay of a line and k samples ghosts one
		                                line down and k across; with padding, where
		                                the stride predicts
		dbtest --feedback               the nth echo sits at n ( k, 1 ) with
		                                amplitude m g^n, until it falls below a code
		dbtest --interleave             a delay = 1 (mod 3) puts R's echo in G, G's
		                                in B and B's in the next pixel's R
		dbtest --flanger                the ghost's offset on every line is the LFO's
		                                value at that line's stream time
		dbtest --allpass                the phaser keeps a noise line's energy, and
		                                the windowed GPU form matches the serial
		                                double cascade within its bound
		dbtest --wrap                   an 8-bit file wraps past full scale, a
		                                16-bit one wraps, a float one clips
		dbtest --pitch                  a bar pattern's period is stretched by 1 / r
		                                inside a grain
		dbtest --negative               every check above can FAIL
		dbtest --offline                the checks that need no GL
		dbtest --bench                  the render cost
		dbtest --dump-shaders DIR       the exact GLSL the plugin compiles
		dbtest --pipe                   raw frames in, raw frames out

	The control laws, the stream's layout, the LFO, the cascade and the
	format are stated HERE, from their definitions (Controls.h's comments
	and Model.h's description), and never read out of the plugin: a
	constant typed wrong there has to show up as a failed check, not as an
	agreement. What IS read from the plugin, and printed, is what it chose
	-- the tap count and the window -- so the report says what the picture
	was held to. AGENTS.md has one line per check on where each tolerance
	comes from.
*/

#include "Controls.h"
#include "Databend.h"
#include "Model.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
namespace model = databendfx::model;

int g_checks   = 0;
int g_failures = 0;

constexpr double kU = 1.0 / 16777216.0;///< one float ULP at 1

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The control laws, stated from their definitions (Controls.h's comments,
// which are the spec of each control). A check converts the FLOAT it hands
// the plugin, so the stated value is exactly what the plugin was asked for.
//---------------------------------------------------------------------------
double unit( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
double pow2( double e )
{
	const double whole = std::floor( e );
	return std::ldexp( std::exp2( e - whole ), static_cast< int >( whole ) );
}
double statedFeedback( float v )
{
	return 0.875 * unit( v );
}
double statedMix( float v )
{
	return unit( v );
}
double statedFlangerSamples( float v )
{
	return 64.0 * unit( v );
}
double statedRateHz( float v )
{
	return 0.05 * std::pow( 400.0, unit( v ) );
}
int statedLfoPeriod( float v )
{
	return std::max( 2, static_cast< int >( std::lround( 44100.0 / statedRateHz( v ) ) ) );
}
double statedPoleMin( float v )
{
	return 0.9 * ( 1.0 - unit( v ) );
}
double statedSMax( float v )
{
	const double k = statedPoleMin( v );
	return std::sqrt( 1.0 - k * k );
}
double statedRatio( float v )
{
	return pow2( 2.0 * unit( v ) - 1.0 );
}
int statedGrain( float v )
{
	const int g = static_cast< int >( std::lround( 64.0 * pow2( 6.0 * unit( v ) ) ) );
	return std::clamp( g + ( g & 1 ), 64, 4096 );
}
double statedCode( int format )
{
	return format == 0 ? 1.0 / 255.0 : format == 1 ? 1.0 / 32768.0 : 1.0 / 65536.0;
}
/// The echo's tap count: the first K whose remainder g^( K + 1 ) / ( 1 - g )
/// is at or under one code.
int statedEchoTaps( double g, double code )
{
	if( g <= 0.0 )
		return 0;
	int K = 0;
	for( double remainder = g / ( 1.0 - g ); remainder > code; remainder *= g )
		++K;
	return K;
}
/// The restart window: the worst-case error iteration of Model.h, stated.
int statedWindow( int S, double kappa, double smax, double X, double N, double code, double mix, int chunk = model::kChunk )
{
	if( S <= 0 || kappa <= 0.0 || mix <= 0.0 )
		return 1;
	std::vector< double > W( S ), ew( S );
	double xi = X;
	const double energy = X * std::sqrt( std::max( 1.0, N ) );
	for( int i = 0; i < S; ++i )
	{
		W[ i ] = std::min( xi / ( 1.0 - kappa ), energy );
		xi     = kappa * xi + smax * W[ i ];
	}
	ew = W;
	//The first K after which the bound stays under half a code for `chunk`
	//steps: the main pass continues from the boundary state that far.
	int streakStart = -1;
	for( int t = 1; t <= model::kMaxWindow + chunk; ++t )
	{
		double eyPrev = 0.0;
		for( int i = 0; i < S; ++i )
		{
			const double ey = kappa * eyPrev + smax * ew[ i ];
			ew[ i ]         = smax * eyPrev + kappa * ew[ i ];
			eyPrev          = ey;
		}
		if( mix * eyPrev < code / 2.0 )
		{
			if( streakStart < 0 )
				streakStart = t;
			if( t - streakStart + 1 >= chunk )
				return streakStart - 1;
		}
		else
			streakStart = -1;
	}
	return model::kMaxWindow;
}
/// The LFO, stated: a triangle of the stream position.
double statedTri( double phase )
{
	return 1.0 - std::fabs( 2.0 * phase - 1.0 );
}
double statedLfo( int64_t n, int period, int offset )
{
	return statedTri( static_cast< double >( ( n + offset ) % period ) / period );
}

/// Every control a check can move, as the sliders the plugin sees.
struct Knobs
{
	int layout      = 0;//Planar
	int padding     = 0;
	int direction   = 0;//Rows
	int format      = 0;//8-bit unsigned
	int overflow    = 1;//Clip
	bool echoOn     = true;
	int delayLines  = 3;
	int delaySamples = 24;
	float feedback  = 0.5f;//7/16
	float echoMix   = 0.5f;
	bool flangerOn  = true;
	float baseDelay = 0.0625f;
	float depth     = 0.375f;
	float rate      = 0.5f;
	float flangerMix = 0.3f;
	bool phaserOn   = true;
	int stages      = 4;
	float phaserRate = 0.35f;
	float phaserDepth = 0.5f;
	float phaserMix = 0.35f;
	bool pitchOn    = false;
	float ratio     = 0.25f;
	float grain     = 0.5f;
	float pitchMix  = 1.0f;
	float mix       = 1.0f;

	/// Everything off, so one effect can be looked at alone.
	static Knobs quiet()
	{
		Knobs k;
		k.echoOn    = false;
		k.flangerOn = false;
		k.phaserOn  = false;
		k.pitchOn   = false;
		return k;
	}
};

/// The stream's geometry, stated (Model.h).
struct Geometry
{
	int lines = 0, pixels = 0, stride = 0, rows = 0;
	int64_t total = 0;
};

Geometry statedGeometry( int W, int H, const Knobs& k )
{
	Geometry g;
	g.lines  = k.direction == 1 ? W : H;
	g.pixels = k.direction == 1 ? H : W;
	g.stride = ( k.layout == 1 ? 3 * g.pixels : g.pixels ) + k.padding;
	g.rows   = k.layout == 1 ? g.lines : 3 * g.lines;
	g.total  = static_cast< int64_t >( g.stride ) * g.rows;
	return g;
}

int64_t statedIndex( const Geometry& g, const Knobs& k, int l, int x, int c )
{
	if( k.layout == 1 )
		return static_cast< int64_t >( l ) * g.stride + 3 * x + c;
	return ( static_cast< int64_t >( c ) * g.lines + l ) * g.stride + x;
}

/// The inverse: which ( line, pixel, channel ) a stream index is, or false
/// for padding or out of range.
bool statedPlace( const Geometry& g, const Knobs& k, int64_t n, int& l, int& x, int& c )
{
	if( n < 0 || n >= g.total )
		return false;
	const int row = static_cast< int >( n / g.stride );
	const int col = static_cast< int >( n % g.stride );
	if( k.layout == 1 )
	{
		if( col >= 3 * g.pixels )
			return false;
		l = row;
		x = col / 3;
		c = col % 3;
		return true;
	}
	if( col >= g.pixels )
		return false;
	c = row / g.lines;
	l = row % g.lines;
	x = col;
	return true;
}

//---------------------------------------------------------------------------
// Pictures, float RGBA, top-first, on a 1/1024 grid so every value is exact.
//---------------------------------------------------------------------------
using Picture = std::vector< float >;

Picture flat( int W, int H, double level )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	for( size_t i = 0; i < p.size(); i += 4 )
	{
		p[ i ] = p[ i + 1 ] = p[ i + 2 ] = static_cast< float >( level );
		p[ i + 3 ] = 1.0f;
	}
	return p;
}

float* pixelAt( Picture& p, int W, int row, int col )
{
	return p.data() + ( static_cast< size_t >( row ) * W + col ) * 4;
}

float at( const std::vector< float >& img, int W, int r, int c, int ch = 0 )
{
	return img[ ( static_cast< size_t >( r ) * W + c ) * 4 + ch ];
}

uint32_t hashInt( uint32_t v )
{
	uint32_t state = v * 747796405u + 2891336453u;
	uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

/// The picture's value ( row, col, channel ) as the file's signal under the
/// format: 0..1 at black for unsigned, 2 v - 1 for signed.
double signalOf( float v, int format )
{
	return format == 0 ? v : 2.0 * v - 1.0;
}
double pictureOf( double signal, int format )
{
	return format == 0 ? signal : 0.5 * ( signal + 1.0 );
}

/// A measured value on the output's floor or ceiling was clipped by the
/// export and says nothing about the effect: a check that reads one is invalid.
bool clipped( double v )
{
	return v <= 1e-5 || v >= 1.0 - 1e-5;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Databend::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Databend& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Databend::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range; an integer's range is real.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		else if( p.type == FF_TYPE_INTEGER )
		{
			const RangeStruct range = plugin.GetParamRange( i );
			p.low                   = range.min;
			p.high                  = range.max;
		}
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Databend& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

/// Whether a parameter steps rather than ramps between --pipe cues.
bool stepsBetweenCues( Databend& plugin, unsigned int index )
{
	const unsigned int type = plugin.GetParamType( index );
	return type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_INTEGER || type == FF_TYPE_EVENT;
}

bool applySetting( Databend& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name = assignment.substr( 0, equals );
	const int index        = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

bool set( Databend& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

void apply( Databend& p, const Knobs& k )
{
	set( p, "Layout", static_cast< float >( k.layout ) );
	set( p, "Line Padding", static_cast< float >( k.padding ) );
	set( p, "Direction", static_cast< float >( k.direction ) );
	set( p, "Format", static_cast< float >( k.format ) );
	set( p, "Overflow", static_cast< float >( k.overflow ) );
	set( p, "Echo On", k.echoOn ? 1.0f : 0.0f );
	set( p, "Delay Lines", static_cast< float >( k.delayLines ) );
	set( p, "Delay Samples", static_cast< float >( k.delaySamples ) );
	set( p, "Feedback", k.feedback );
	set( p, "Echo Mix", k.echoMix );
	set( p, "Flanger On", k.flangerOn ? 1.0f : 0.0f );
	set( p, "Base Delay", k.baseDelay );
	set( p, "Depth", k.depth );
	set( p, "Rate", k.rate );
	set( p, "Flanger Mix", k.flangerMix );
	set( p, "Phaser On", k.phaserOn ? 1.0f : 0.0f );
	set( p, "Stages", static_cast< float >( k.stages ) );
	set( p, "Phaser Rate", k.phaserRate );
	set( p, "Phaser Depth", k.phaserDepth );
	set( p, "Phaser Mix", k.phaserMix );
	set( p, "Pitch On", k.pitchOn ? 1.0f : 0.0f );
	set( p, "Ratio", k.ratio );
	set( p, "Grain", k.grain );
	set( p, "Pitch Mix", k.pitchMix );
	set( p, "Mix", k.mix );
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output. No clock: the plugin has
// none; the LFOs' phase is the plugin's own frame counter.
//---------------------------------------------------------------------------
struct Session
{
	Databend plugin;
	int width        = 0;
	int height       = 0;
	bool floatOutput = true;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	bool renderNow()
	{
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %u\n", plugin.FrameIndexForTest() );
		return ok;
	}

	bool render( const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderNow();
	}

	bool render( const Picture& pixels )
	{
		const std::vector< float > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderNow();
	}

	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	std::vector< float > readBackFloat()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

/// One frame of one picture through a fresh session, read back as floats,
/// with what the plugin chose.
bool renderOnce( const Knobs& k, int perturb, int W, int H, const Picture& pic, std::vector< float >& out, Databend::Chosen* chosen = nullptr )
{
	Session s;
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	if( !s.begin( W, H ) || !s.render( pic ) )
		return false;
	out = s.readBackFloat();
	if( chosen )
		*chosen = s.plugin.ChosenForTest();
	s.end();
	return true;
}

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAIL";
}

int report( bool ok, bool quiet, const char* format, ... ) __attribute__( ( format( printf, 3, 4 ) ) );
int report( bool ok, bool quiet, const char* format, ... )
{
	++g_checks;
	if( !ok )
		++g_failures;
	//Quiet is a negative control's run: its failures are the point, and the
	//summary line says so. --perturb runs the same thing verbosely.
	if( !quiet )
	{
		va_list args;
		va_start( args, format );
		std::vprintf( format, args );
		va_end( args );
		std::printf( "  %s\n", verdict( ok ) );
	}
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// The echo, in closed form: an impulse of signal a at stream index n0 comes
// back at n0 + j D with amplitude m g^j, for every j >= 1, and nowhere
// else. Predicts a whole picture: grey (signal 0) everywhere, the impulse,
// its ghosts wherever they land on a visible sample (the stream is one
// stream, so a ghost past the end of a plane lands in the next plane's
// first lines, and a ghost in padding is not seen).
//---------------------------------------------------------------------------
struct Impulse
{
	int line, pixel, channel;
	double signal;
};

Picture predictedEcho( int W, int H, const Knobs& k, const std::vector< Impulse >& impulses, int64_t D, double g, double m, int taps, double grey )
{
	const Geometry geo = statedGeometry( W, H, k );
	Picture p = flat( W, H, grey );
	for( const Impulse& imp : impulses )
	{
		pixelAt( p, W, imp.line, imp.pixel )[ imp.channel ] = static_cast< float >( pictureOf( imp.signal, k.format ) );
		const int64_t n0 = statedIndex( geo, k, imp.line, imp.pixel, imp.channel );
		double amplitude = m;
		for( int j = 1; j <= taps; ++j )
		{
			amplitude *= g;
			int l, x, c;
			if( statedPlace( geo, k, n0 + j * D, l, x, c ) )
			{
				float* px = pixelAt( p, W, l, x );
				px[ c ]   = static_cast< float >( pictureOf( signalOf( px[ c ], k.format ) + amplitude * imp.signal, k.format ) );
			}
		}
	}
	return p;
}

/// The worst difference between two pictures over the three colour
/// channels, and where it is.
double worstDifference( const std::vector< float >& out, const Picture& want, int W, int H, int& worstRow, int& worstCol, int& worstCh )
{
	double worst = -1.0;
	for( int r = 0; r < H; ++r )
		for( int c = 0; c < W; ++c )
			for( int ch = 0; ch < 3; ++ch )
			{
				const double d = std::fabs( at( out, W, r, c, ch ) - at( want, W, r, c, ch ) );
				if( d > worst )
				{
					worst    = d;
					worstRow = r;
					worstCol = c;
					worstCh  = ch;
				}
			}
	return worst;
}

//---------------------------------------------------------------------------
// --echo
//
// Planar, a float file (no quantisation: the picture IS the signal, and
// silence is mid-grey), the echo alone at Echo Mix 1 and Feedback 7/16.
// One impulse on one line; four delays:
//   1 line + k samples, no padding        -> ( 1 down, k across )
//   Lp + k samples, no padding            -> the same place
//   Lp + k samples, p samples of padding  -> ( 1 down, k - p across ): the
//                                            stride is Lp + p now
//   1 line + k samples, p of padding      -> ( 1 down, k across ) again
// The whole picture is compared with the closed form, so a ghost anywhere
// else fails too. Tolerance: the Horner sum's float roundings, 2 ( K + 1 )
// ULPs of a unit, plus the truncated tail (under one code of the format by
// the tap count's definition) scaled by the impulse.
//---------------------------------------------------------------------------
double echoTolerance( int taps, double code, double m, double impulse )
{
	return 2.0 * ( taps + 1 ) * kU + m * code * std::fabs( impulse ) + 4.0 * kU;
}

int runEcho( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	Knobs k      = Knobs::quiet();
	k.layout     = 0;
	k.format     = 2;
	k.echoOn     = true;
	k.feedback   = 0.5f;
	k.echoMix    = 1.0f;
	const double g = statedFeedback( k.feedback );
	const double m = statedMix( k.echoMix );
	const int kAcross = 7, pad = 5;
	const int l0 = 2, x0 = W / 3;
	const int Lp = W;
	struct Case
	{
		int lines, samples, padding, downWant, acrossWant;
		const char* what;
	} const cases[] = {
		{ 1, kAcross, 0, 1, kAcross, "1 line + k, no padding" },
		{ 0, Lp + kAcross, 0, 1, kAcross, "Lp + k samples, no padding" },
		{ 0, Lp + kAcross, pad, 1, kAcross - pad, "Lp + k samples, p of padding" },
		{ 1, kAcross, pad, 1, kAcross, "1 line + k, p of padding" },
	};
	for( const Case& cs : cases )
	{
		k.delayLines   = cs.lines;
		k.delaySamples = cs.samples;
		k.padding      = cs.padding;
		const Geometry geo = statedGeometry( W, H, k );
		const int64_t D    = static_cast< int64_t >( cs.lines ) * geo.stride + cs.samples;
		const int taps     = statedEchoTaps( g, statedCode( k.format ) );

		Picture pic = flat( W, H, 0.5 );
		for( int c = 0; c < 3; ++c )
			pixelAt( pic, W, l0, x0 )[ c ] = 0.75f;
		std::vector< Impulse > impulses;
		for( int c = 0; c < 3; ++c )
			impulses.push_back( { l0, x0, c, 0.5 } );
		const Picture want = predictedEcho( W, H, k, impulses, D, g, m, taps, 0.5 );

		std::vector< float > out;
		Databend::Chosen chosen;
		if( !renderOnce( k, perturb, W, H, pic, out, &chosen ) )
			return report( false, quiet, "echo: could not render" );

		//Where the first ghost is, read off the picture: the brightest
		//pixel on line l0 + 1.
		int ghostCol = -1;
		double ghostLevel = 0.0;
		for( int c = 0; c < W; ++c )
			if( at( out, W, l0 + cs.downWant, c ) > ghostLevel + 1e-6 )
			{
				ghostLevel = at( out, W, l0 + cs.downWant, c );
				ghostCol   = c;
			}
		int wr, wc, wch;
		const double worst = worstDifference( out, want, W, H, wr, wc, wch );
		const double tol   = echoTolerance( taps, statedCode( k.format ), m, 0.5 );
		const bool ok      = ghostCol == x0 + cs.acrossWant && worst <= tol && chosen.echoTaps == taps;
		failures += report( ok, quiet,
		                    "echo %-30s D = %lld: ghost predicted at (+%d, %+d), brightest on that line at %+d with %.4f (predicted %.4f); whole picture within %.3g (tolerance %.3g, %d taps)",
		                    cs.what, static_cast< long long >( D ), cs.downWant, cs.acrossWant, ghostCol - x0, ghostLevel - 0.5, 0.25 * m * g, worst, tol, taps );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --feedback
//
// The same setting, one line plus k samples: the j-th echo sits at
// ( j down, j k across ) with amplitude m g^j, read one by one until it
// falls under one code of the format; the tap count the plugin chose is
// printed beside the count the code predicts.
//---------------------------------------------------------------------------
int runFeedback( int W, int H, int perturb = 0, bool quiet = false )
{
	Knobs k        = Knobs::quiet();
	k.layout       = 0;
	k.format       = 2;
	k.echoOn       = true;
	k.feedback     = 0.5f;
	k.echoMix      = 1.0f;
	k.delayLines   = 1;
	k.delaySamples = 7;
	const double g = statedFeedback( k.feedback );
	const double m = statedMix( k.echoMix );
	const double code = statedCode( k.format );
	const int taps = statedEchoTaps( g, code );
	const int l0 = 1, x0 = 8;

	Picture pic = flat( W, H, 0.5 );
	for( int c = 0; c < 3; ++c )
		pixelAt( pic, W, l0, x0 )[ c ] = 0.75f;
	std::vector< float > out;
	Databend::Chosen chosen;
	if( !renderOnce( k, perturb, W, H, pic, out, &chosen ) )
		return report( false, quiet, "feedback: could not render" );

	const double tol = echoTolerance( taps, code, m, 0.5 );
	double worst = 0.0;
	int counted  = 0;
	bool ok      = chosen.echoTaps == taps;
	double amplitude = m;
	for( int j = 1; j <= taps && l0 + j < H && x0 + 7 * j < W; ++j )
	{
		amplitude *= g;
		const double want = 0.5 * amplitude;//the impulse is 0.5 of signal
		const double got  = 2.0 * at( out, W, l0 + j, x0 + 7 * j ) - 1.0;
		worst = std::max( worst, std::fabs( got - want ) );
		ok    = ok && std::fabs( got - want ) <= tol;
		++counted;
	}
	return report( ok, quiet,
	               "feedback g = %.4f, m = %g: %d echoes at ( j, 7 j ) with amplitude m g^j, worst %.3g of signal (tolerance %.3g); the plugin summed %d taps, one code of a float file (%g) predicts %d",
	               g, m, counted, worst, tol, chosen.echoTaps, code, taps );
}

//---------------------------------------------------------------------------
// --interleave
//
// Interleaved, a float file, the echo alone. An impulse in ONE channel:
//   D = 4 ( = 1 mod 3 ): R's echo lands in G one pixel on, G's in B, B's in
//                        the next pixel's R
//   D = 3 ( = 0 mod 3 ): every colour's echo stays in its colour
//   D = 1 line + 1:      one line down, R in G at the same pixel
// Whole-picture comparison with the closed form, as --echo.
//---------------------------------------------------------------------------
int runInterleave( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	Knobs k      = Knobs::quiet();
	k.layout     = 1;
	k.format     = 2;
	k.echoOn     = true;
	k.feedback   = 0.5f;
	k.echoMix    = 1.0f;
	const double g = statedFeedback( k.feedback );
	const double m = statedMix( k.echoMix );
	const double code = statedCode( k.format );
	const int taps = statedEchoTaps( g, code );
	const int l0 = 3, x0 = W / 4;
	const char* const channelNames[ 3 ] = { "R", "G", "B" };
	struct Case
	{
		int lines, samples, channel;
		int wantChannel, wantAcross, wantDown;
		const char* what;
	} const cases[] = {
		{ 0, 4, 0, 1, 1, 0, "D = 4, R's echo" },
		{ 0, 4, 1, 2, 1, 0, "D = 4, G's echo" },
		{ 0, 4, 2, 0, 2, 0, "D = 4, B's echo" },
		{ 0, 3, 0, 0, 1, 0, "D = 3, R's echo" },
		{ 0, 3, 2, 2, 1, 0, "D = 3, B's echo" },
		{ 1, 1, 0, 1, 0, 1, "D = L + 1, R's echo" },
	};
	for( const Case& cs : cases )
	{
		k.delayLines   = cs.lines;
		k.delaySamples = cs.samples;
		const Geometry geo = statedGeometry( W, H, k );
		const int64_t D    = static_cast< int64_t >( cs.lines ) * geo.stride + cs.samples;
		Picture pic = flat( W, H, 0.5 );
		pixelAt( pic, W, l0, x0 )[ cs.channel ] = 0.75f;
		const Picture want = predictedEcho( W, H, k, { { l0, x0, cs.channel, 0.5 } }, D, g, m, taps, 0.5 );

		std::vector< float > out;
		if( !renderOnce( k, perturb, W, H, pic, out ) )
			return report( false, quiet, "interleave: could not render" );

		//The first ghost, read off the picture: the brightest channel of
		//any pixel on the predicted line other than the impulse itself.
		int gotChannel = -1, gotCol = -1;
		double level = 0.0;
		for( int c = 0; c < W; ++c )
			for( int ch = 0; ch < 3; ++ch )
			{
				if( cs.wantDown == 0 && c == x0 && ch == cs.channel )
					continue;
				const double v = at( out, W, l0 + cs.wantDown, c, ch );
				if( v > level + 1e-6 )
				{
					level      = v;
					gotChannel = ch;
					gotCol     = c;
				}
			}
		int wr, wc, wch;
		const double worst = worstDifference( out, want, W, H, wr, wc, wch );
		const double tol   = echoTolerance( taps, code, m, 0.5 );
		const bool ok      = gotChannel == cs.wantChannel && gotCol == x0 + cs.wantAcross && worst <= tol;
		failures += report( ok, quiet, "interleave %-22s predicted in %s at %+d pixels, %d down; found in %s at %+d; whole picture within %.3g (tolerance %.3g)",
		                    cs.what, channelNames[ cs.wantChannel ], cs.wantAcross, cs.wantDown, gotChannel < 0 ? "-" : channelNames[ gotChannel ], gotCol - x0, worst, tol );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --flanger
//
// Planar, a float file, the flanger alone at Flanger Mix 1 (a crossfade:
// all ghost, no dry): base 16 samples, depth 32, rate 1 Hz (a period of
// 44100 samples). One impulse per line at x0, in the R plane's stream.
// Output sample n reads e[n - d(n)]
// as the interpolation of samples n - floor d and n - floor d - 1, so the
// impulse at n0 lands on the output samples n1 with n1 - floor d(n1) = n0
// (weight 1 - frac d(n1)) and n2 with n2 - floor d(n2) - 1 = n0 (weight
// frac d(n2)), found by solving that from the stated LFO. The check reads
// the two values and recovers the offset as a centroid, n1 - n0 + v2 /
// ( v1 + v2 ), which is d(n1) up to how far the LFO moves in one sample.
// Tolerance on a value: the delay is exact to 2 ULPs of ( base + depth )
// and the interpolation weight inherits that, scaled by the impulse; on
// the offset: 2 depth / P (the LFO's slope) plus the same ULPs.
//---------------------------------------------------------------------------
int runFlanger( int W, int H, int perturb = 0, bool quiet = false )
{
	Knobs k       = Knobs::quiet();
	k.layout      = 0;
	k.format      = 2;
	k.flangerOn   = true;
	k.baseDelay   = 0.25f;//16 samples
	k.depth       = 0.5f;//32 samples
	k.rate        = 0.5f;//1 Hz
	k.flangerMix  = 1.0f;
	const double base  = statedFlangerSamples( k.baseDelay );
	const double width = statedFlangerSamples( k.depth );
	const int period   = statedLfoPeriod( k.rate );
	const double m     = statedMix( k.flangerMix );
	const Geometry geo = statedGeometry( W, H, k );
	const int x0       = W / 5;
	const double impulse = 0.5;

	Picture pic = flat( W, H, 0.5 );
	for( int l = 0; l < H; ++l )
		for( int c = 0; c < 3; ++c )
			pixelAt( pic, W, l, x0 )[ c ] = 0.75f;
	std::vector< float > out;
	Databend::Chosen chosen;
	if( !renderOnce( k, perturb, W, H, pic, out, &chosen ) )
		return report( false, quiet, "flanger: could not render" );

	auto delayAt = [ & ]( int64_t n ) { return base + width * statedLfo( n, period, 0 ); };
	const double tolValue  = 2.0 * ( base + width ) * kU * m * impulse + 8.0 * kU;
	const double tolOffset = 2.0 * width / period + 4.0 * ( base + width ) * kU;
	double worstValue = 0.0, worstOffset = 0.0, minOffset = 1e9, maxOffset = -1e9;
	int lines = 0;
	bool ok   = chosen.lfoPeriodF == period && chosen.lfoOffsetF == 0;
	for( int l = 0; l < H; ++l )
	{
		const int64_t n0 = statedIndex( geo, k, l, x0, 0 );
		int64_t n1 = -1, n2 = -1;
		for( int64_t n = n0; n <= n0 + static_cast< int64_t >( base + width ) + 2; ++n )
		{
			const int64_t whole = static_cast< int64_t >( std::floor( delayAt( n ) ) );
			if( n - whole == n0 && n1 < 0 )
				n1 = n;
			if( n - whole - 1 == n0 && n2 < 0 )
				n2 = n;
		}
		if( n1 < 0 || n2 < 0 || n2 - n1 != 1 )
			continue;//the LFO stepped between the two taps: not a clean measurement
		const double d1 = delayAt( n1 );
		const double f1 = d1 - std::floor( d1 );
		const double want1 = m * impulse * ( 1.0 - f1 );
		const double d2 = delayAt( n2 );
		const double want2 = m * impulse * ( d2 - std::floor( d2 ) );
		const int c1 = static_cast< int >( n1 - static_cast< int64_t >( l ) * geo.stride );
		if( c1 + 1 >= W )
			continue;
		const double v1 = 2.0 * at( out, W, l, c1 ) - 1.0;
		const double v2 = 2.0 * at( out, W, l, c1 + 1 ) - 1.0;
		const double offset = ( c1 - x0 ) + v2 / std::max( 1e-12, v1 + v2 );
		worstValue  = std::max( { worstValue, std::fabs( v1 - want1 ), std::fabs( v2 - want2 ) } );
		worstOffset = std::max( worstOffset, std::fabs( offset - d1 ) );
		minOffset   = std::min( minOffset, offset );
		maxOffset   = std::max( maxOffset, offset );
		ok          = ok && std::fabs( v1 - want1 ) <= tolValue && std::fabs( v2 - want2 ) <= tolValue && std::fabs( offset - d1 ) <= tolOffset;
		++lines;
	}
	ok = ok && lines >= H / 2;
	return report( ok, quiet,
	               "flanger base %g + depth %g samples, P = %d: on %d of %d lines the ghost's offset is the LFO's value at that stream time, %.3f..%.3f samples across the picture; worst offset %.2g (tolerance %.2g), worst value %.2g (tolerance %.2g)",
	               base, width, period, lines, H, minOffset, maxOffset, worstOffset, tolOffset, worstValue, tolValue );
}

//---------------------------------------------------------------------------
// The serial reference for the phaser: the cascade in double, sample by
// sample, over the whole stream, from the description in Model.h.
//---------------------------------------------------------------------------
std::vector< double > serialPhaser( const std::vector< double >& x, int S, double poleTop, double poleMin, int period, int offset, double mix )
{
	std::vector< double > w( S, 0.0 ), y( x.size() );
	for( size_t n = 0; n < x.size(); ++n )
	{
		const double kk = poleTop - ( poleTop - poleMin ) * statedLfo( static_cast< int64_t >( n ), period, offset );
		const double s  = std::sqrt( 1.0 - kk * kk );
		double v        = x[ n ];
		for( int j = 0; j < S; ++j )
		{
			const double wp = w[ j ];
			const double o  = -kk * v + s * wp;
			w[ j ]          = s * v + kk * wp;
			v               = o;
		}
		y[ n ] = ( 1.0 - mix ) * x[ n ] + mix * v;
	}
	return y;
}

/// The stream of a picture, stated.
std::vector< double > streamOf( const Picture& pic, int W, int H, const Knobs& k )
{
	const Geometry geo = statedGeometry( W, H, k );
	std::vector< double > s( static_cast< size_t >( geo.total ), 0.0 );
	for( int l = 0; l < geo.lines; ++l )
		for( int x = 0; x < geo.pixels; ++x )
			for( int c = 0; c < 3; ++c )
			{
				const int row = k.direction == 1 ? x : l;
				const int col = k.direction == 1 ? l : x;
				s[ static_cast< size_t >( statedIndex( geo, k, l, x, c ) ) ] = signalOf( at( pic, W, row, col, c ), k.format );
			}
	return s;
}

/// The stream read back out of a rendered float-format picture: the
/// visible samples; padding stays 0 and is not compared.
std::vector< double > streamOfOutput( const std::vector< float >& out, int W, int H, const Knobs& k )
{
	return streamOf( out, W, H, k );
}

//---------------------------------------------------------------------------
// --allpass
//
// Interleaved, a float file, the phaser alone at Phaser Mix 1. A noise
// picture (+-1/8 about grey) on the first quarter of the lines, grey below.
//
// (a) Energy. Every lattice section is a rotation, so the cascade is
//     lossless: over the whole stream the output energy equals the input
//     energy less what is still in the states at the end, and after three
//     quarters of a picture of silence with |k| <= 0.9 that is nothing.
//     Tolerance: the restart error is under half a code of the file per
//     sample, delta, so the energy is off by at most 2 delta sqrt( N E ) +
//     N delta^2; the float arithmetic adds 32 ULPs per sample of the same
//     shape (a model of the roundings, not a bound: said so in AGENTS.md).
//     A reading on the floor or ceiling invalidates the check.
// (b) Reference. Every visible sample against the serial double cascade:
//     the restart bound, half a code, plus the float model, 32 ULPs times
//     the square root of the window plus a chunk (a random walk over the
//     re-run).
//     The plugin's window is printed against the stated one; the same
//     error is also measured at half and a quarter of the window, so the
//     bound's slack is on the record.
//---------------------------------------------------------------------------
int runAllpass( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	for( int stages : { 4, 12 } )
	{
		Knobs k       = Knobs::quiet();
		k.layout      = 1;
		k.format      = 2;
		k.phaserOn    = true;
		k.stages      = stages;
		k.phaserRate  = 0.5f;//1 Hz, P = 44100
		k.phaserDepth = 1.0f;//the sweep 0..0.9
		k.phaserMix   = 1.0f;
		const int period  = statedLfoPeriod( k.phaserRate );
		const double code = statedCode( k.format );
		const double kappa = 0.9, poleMin = statedPoleMin( k.phaserDepth ), smax = statedSMax( k.phaserDepth );
		const Geometry geo = statedGeometry( W, H, k );
		const int window   = statedWindow( stages, kappa, smax, 1.0, static_cast< double >( geo.total ), code, 1.0 );

		Picture pic = flat( W, H, 0.5 );
		for( int r = 0; r < H / 4; ++r )
			for( int c = 0; c < W; ++c )
				for( int ch = 0; ch < 3; ++ch )
				{
					const uint32_t h = hashInt( 0x9E3779B9u * static_cast< uint32_t >( r * W + c ) * 3u + static_cast< uint32_t >( ch ) + 7u );
					pixelAt( pic, W, r, c )[ ch ] = static_cast< float >( 0.5 + ( static_cast< double >( h % 256u ) / 256.0 - 0.5 ) * 0.125 );
				}
		const std::vector< double > x = streamOf( pic, W, H, k );
		const std::vector< double > y = serialPhaser( x, stages, kappa, poleMin, period, 0, 1.0 );

		std::vector< float > out;
		Databend::Chosen chosen;
		if( !renderOnce( k, perturb, W, H, pic, out, &chosen ) )
			return report( false, quiet, "allpass: could not render" );
		const std::vector< double > got = streamOfOutput( out, W, H, k );

		bool anyClipped = false;
		for( int r = 0; r < H; ++r )
			for( int c = 0; c < W; ++c )
				for( int ch = 0; ch < 3; ++ch )
					anyClipped = anyClipped || clipped( at( out, W, r, c, ch ) );

		//(a)
		double eIn = 0.0, eOut = 0.0, sumAbs = 0.0;
		for( size_t n = 0; n < x.size(); ++n )
		{
			eIn += x[ n ] * x[ n ];
			eOut += got[ n ] * got[ n ];
			sumAbs += std::fabs( y[ n ] );
		}
		const double delta   = code / 2.0 + 32.0 * kU;
		const double N       = static_cast< double >( x.size() );
		const double eTol    = 2.0 * delta * std::sqrt( N * eIn ) + N * delta * delta;
		const bool energyOk  = !anyClipped && std::fabs( eOut - eIn ) <= eTol;
		failures += report( energyOk, quiet, "allpass %2d stages, energy: in %.6f, out %.6f, off by %.3g (tolerance %.3g)%s", stages, eIn, eOut, std::fabs( eOut - eIn ), eTol,
		                    anyClipped ? " -- a reading was clipped, invalid" : "" );

		//(b)
		double worst = 0.0;
		for( size_t n = 0; n < x.size(); ++n )
			worst = std::max( worst, std::fabs( got[ n ] - y[ n ] ) );
		const double tol = code / 2.0 + 32.0 * kU * std::sqrt( static_cast< double >( window + model::kChunk ) );
		const bool refOk = worst <= tol && chosen.phaserWindow == window;
		failures += report( refOk, quiet, "allpass %2d stages, reference: %zu samples, worst %.3g against the serial double cascade (tolerance %.3g: half a code %.3g + float); the plugin's window %d, stated %d",
		                    stages, x.size(), worst, tol, code / 2.0, chosen.phaserWindow, window );

		//The slack, for the record: the same error at fractions of the
		//window, on the perturbed plugin -- not pass/fail.
		if( !quiet && perturb == 0 )
		{
			std::vector< float > shortOut;
			if( renderOnce( k, model::kPerturbShortWindow, W, H, pic, shortOut ) )
			{
				const std::vector< double > sg = streamOfOutput( shortOut, W, H, k );
				double w4 = 0.0;
				for( size_t n = 0; n < x.size(); ++n )
					w4 = std::max( w4, std::fabs( sg[ n ] - y[ n ] ) );
				std::printf( "   (at a quarter of the window, %d samples, the worst is %.3g: %.0f tolerances)\n", std::max( 1, window / 4 ), w4, w4 / tol );
			}
		}
	}
	return failures;
}

//---------------------------------------------------------------------------
// --wrap
//
// Planar, the echo alone at Echo Mix 1 and Feedback 7/16, a delay of one
// line. A white block two lines tall: its second line sees itself plus
// 7/16 of itself, 1.4375 of full scale, and the line under the block sees
// 7/16 of the second line and 7/16^2 of the first, 0.62890625 of white.
// What the export makes of 1.4375:
//   8-bit unsigned, Wrap   round( 1.4375 x 255 ) = 367 -> 367 - 256 = 111 -> 111/255
//   8-bit unsigned, Clip   255 -> 1
//   16-bit signed, Wrap    white is +1: round( 1.4375 x 32768 ) = 47104 ->
//                          47104 - 65536 = -18432 -> ( -18432/32768 + 1 ) / 2
//   16-bit signed, Clip    32767 -> ( 32767/32768 + 1 ) / 2
//   float                  clips at 1
// and of 0.62890625 (no overflow: the quantiser alone). Every product here
// is a dyadic times 255 or 32768, exact in float, and none lands within
// 1/8 of a half code, so the rounding is the same on both sides.
// Tolerance: two ULPs, for the final division.
//---------------------------------------------------------------------------
int runWrap( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	struct Case
	{
		int format, overflow;
		const char* what;
	} const cases[] = {
		{ 0, 0, "8-bit unsigned, Wrap" },
		{ 0, 1, "8-bit unsigned, Clip" },
		{ 1, 0, "16-bit signed, Wrap" },
		{ 1, 1, "16-bit signed, Clip" },
		{ 2, 0, "Float (Wrap asked)" },
		{ 2, 1, "Float, Clip" },
	};
	const int top = 4;
	for( const Case& cs : cases )
	{
		Knobs k        = Knobs::quiet();
		k.layout       = 0;
		k.format       = cs.format;
		k.overflow     = cs.overflow;
		k.echoOn       = true;
		k.feedback     = 0.5f;
		k.echoMix      = 1.0f;
		k.delayLines   = 1;
		k.delaySamples = 0;
		const double g = statedFeedback( k.feedback );
		const double m = statedMix( k.echoMix );
		//Below the block the signal is 0 (silence: black for 8-bit, grey
		//for the signed formats), so only the block's own lines overflow.
		Picture pic = flat( W, H, cs.format == 0 ? 0.0 : 0.5 );
		for( int r = top; r < top + 2; ++r )
			for( int c = 0; c < W; ++c )
				for( int ch = 0; ch < 3; ++ch )
					pixelAt( pic, W, r, c )[ ch ] = 1.0f;

		auto exported = [ & ]( double signal ) {
			const bool wrap = cs.overflow == 0;
			if( cs.format == 0 )
			{
				double code = std::floor( signal * 255.0 + 0.5 );
				code        = wrap ? code - 256.0 * std::floor( code / 256.0 ) : std::clamp( code, 0.0, 255.0 );
				return code / 255.0;
			}
			if( cs.format == 1 )
			{
				double code = std::floor( signal * 32768.0 + 0.5 );
				code        = wrap ? code - 65536.0 * std::floor( ( code + 32768.0 ) / 65536.0 ) : std::clamp( code, -32768.0, 32767.0 );
				return 0.5 * ( code / 32768.0 + 1.0 );
			}
			return 0.5 * ( std::clamp( signal, -1.0, 1.0 ) + 1.0 );
		};
		const double white  = cs.format == 0 ? 1.0 : 1.0;//white is full scale in both domains
		const double second = white + m * g * white;//the block's second line
		const double under  = m * ( g + g * g ) * white;//the line under the block: two echoes
		const double wantSecond = exported( second );
		const double wantUnder  = exported( under );

		std::vector< float > out;
		if( !renderOnce( k, perturb, W, H, pic, out ) )
			return report( false, quiet, "wrap: could not render" );
		double worst = 0.0;
		for( int c = 0; c < W; ++c )
			for( int ch = 0; ch < 3; ++ch )
			{
				worst = std::max( worst, std::fabs( at( out, W, top + 1, c, ch ) - wantSecond ) );
				worst = std::max( worst, std::fabs( at( out, W, top + 2, c, ch ) - wantUnder ) );
			}
		const double tol = 2.0 * kU;
		failures += report( worst <= tol, quiet, "wrap %-22s 1.4375 of full scale exports as %.6f, measured %.6f; 0.6289 as %.6f, measured %.6f; worst %.2g (tolerance %.2g)",
		                    cs.what, wantSecond, at( out, W, top + 1, W / 2 ), wantUnder, at( out, W, top + 2, W / 2 ), worst, tol );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --pitch
//
// Planar, a float file, the pitch shifter alone at Pitch Mix 1, grains of
// 512 samples. Vertical bars of period 16 pixels (8 bright, 8 dark, +-1/2 of
// signal). Inside a grain the scan is read at rate r, so the bars come out
// with period 16 / r; and with a 512-sample grain the two taps, half a
// grain apart, read the pattern ( 1 - r ) 256 samples apart -- a whole
// number of the stretched periods for r = 1/2 and r = 2 -- so the crossfade
// blends two copies in phase and the line is one clean wave. Measured out
// of the picture as an autocorrelation along a line: R( 16 / r ) is above
// half of R( 0 ), and R( 8 / r ), half a period, is negative; with the
// shifter bypassed, the same at the original period. No tolerance to
// derive: the assertions are on signs and a ratio of a square wave's
// autocorrelation (1 at a period, -1 at a half period).
//---------------------------------------------------------------------------
int runPitch( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	struct Case
	{
		bool on;
		float ratio;
		const char* what;
	} const cases[] = {
		{ true, 0.0f, "Ratio 1/2" },
		{ true, 1.0f, "Ratio 2" },
		{ false, 0.0f, "bypassed" },
	};
	const int bar = 16;
	for( const Case& cs : cases )
	{
		Knobs k     = Knobs::quiet();
		k.layout    = 0;
		k.format    = 2;
		k.pitchOn   = cs.on;
		k.ratio     = cs.ratio;
		k.grain     = 0.5f;//512
		k.pitchMix  = 1.0f;
		const double r     = cs.on ? statedRatio( k.ratio ) : 1.0;
		const int period   = static_cast< int >( std::lround( bar / r ) );

		Picture pic = flat( W, H, 0.5 );
		for( int row = 0; row < H; ++row )
			for( int c = 0; c < W; ++c )
				for( int ch = 0; ch < 3; ++ch )
					pixelAt( pic, W, row, c )[ ch ] = ( c % bar ) < bar / 2 ? 0.75f : 0.25f;
		std::vector< float > out;
		if( !renderOnce( k, perturb, W, H, pic, out ) )
			return report( false, quiet, "pitch: could not render" );

		auto autocorrelation = [ & ]( int row, int lag ) {
			double sum = 0.0;
			for( int c = 0; c + lag < W; ++c )
				sum += ( 2.0 * at( out, W, row, c ) - 1.0 ) * ( 2.0 * at( out, W, row, c + lag ) - 1.0 );
			return sum;
		};
		bool ok = true;
		double worstRatio = 1e9, worstHalf = -1e9;
		for( int row = 0; row < H; row += std::max( 1, H / 8 ) )
		{
			const double r0 = autocorrelation( row, 0 ), rp = autocorrelation( row, period ), rh = autocorrelation( row, period / 2 );
			worstRatio = std::min( worstRatio, rp / r0 );
			worstHalf  = std::max( worstHalf, rh / r0 );
			ok         = ok && rp > 0.5 * r0 && rh < 0.0;
		}
		failures += report( ok, quiet, "pitch %-10s bars of %d read as period %d: R(%d)/R(0) at least %.3f (want > 0.5), R(%d)/R(0) at most %.3f (want < 0)",
		                    cs.what, bar, period, period, worstRatio, period / 2, worstHalf );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --laws, --names, --window: no GL.
//---------------------------------------------------------------------------
int runLaws( bool quiet = false )
{
	namespace controls = databendfx::controls;
	int failures = 0;
	double worst = 0.0;
	auto rel     = []( double a, double b ) { return b == 0.0 ? std::fabs( a ) : std::fabs( a - b ) / std::fabs( b ); };
	for( int i = 0; i <= 20; ++i )
	{
		const float v = static_cast< float >( i ) / 20.0f;
		worst = std::max( { worst, rel( controls::Feedback( v ), statedFeedback( v ) ), rel( controls::Mix( v ), statedMix( v ) ),
		                    rel( controls::FlangerSamples( v ), statedFlangerSamples( v ) ), rel( controls::RateHz( v ), statedRateHz( v ) ),
		                    rel( controls::PoleMin( v ), statedPoleMin( v ) ), rel( controls::SMax( v ), statedSMax( v ) ),
		                    rel( controls::Ratio( v ), statedRatio( v ) ) } );
		if( controls::LfoPeriod( v ) != statedLfoPeriod( v ) || controls::Grain( v ) != statedGrain( v ) )
			worst = 1.0;
		for( int count : { 2, 3 } )
			if( controls::OptionIndex( v * ( count - 1 ), count ) != std::clamp( static_cast< int >( std::lround( v * ( count - 1 ) ) ), 0, count - 1 ) )
				worst = 1.0;
	}
	failures += report( worst < 1e-12, quiet, "laws: Feedback, the mixes, Base Delay, Depth, Rate, Phaser Depth, Ratio, Grain and the option index as stated at 21 points, worst %.2g relative", worst );

	const bool dyadic = controls::Feedback( 0.5f ) == 7.0 / 16.0 && controls::Feedback( 1.0f ) == 0.875 && controls::Ratio( 0.0f ) == 0.5 && controls::Ratio( 0.5f ) == 1.0
	                    && controls::Ratio( 1.0f ) == 2.0 && controls::Grain( 0.0f ) == 64 && controls::Grain( 0.5f ) == 512 && controls::Grain( 1.0f ) == 4096
	                    && controls::FlangerSamples( 0.25f ) == 16.0 && controls::FlangerSamples( 0.5f ) == 32.0 && controls::LfoPeriod( 0.5f ) == 44100;
	failures += report( dyadic, quiet, "laws: the dyadic promises hold exactly -- Feedback 7/16 and 7/8, Ratio 1/2, 1 and 2, Grain 64, 512 and 4096, 16 and 32 samples of delay, a 1 Hz period of 44100" );

	//The echo's tap count and the code of each format.
	bool taps = true;
	for( int format = 0; format < 3; ++format )
		for( int i = 0; i <= 20; ++i )
		{
			const double g = statedFeedback( static_cast< float >( i ) / 20.0f );
			taps           = taps && model::EchoTaps( g, model::CodeOf( format ) ) == statedEchoTaps( g, statedCode( format ) ) && model::CodeOf( format ) == statedCode( format );
		}
	failures += report( taps && model::EchoTaps( 7.0 / 16.0, 1.0 / 255.0 ) == 7 && model::EchoTaps( 0.875, 1.0 / 65536.0 ) == 98, quiet,
	                    "laws: the echo's tap count as stated for every format and feedback; 7 taps at 7/16 in an 8-bit file (the remainder 7/16^8 / (9/16) = %.2g codes), 98 at 7/8 in a float one",
	                    std::pow( 7.0 / 16.0, 8 ) / ( 9.0 / 16.0 ) * 255.0 );
	return failures;
}

int runNames( bool quiet = false )
{
	Databend plugin;
	std::set< std::string > seen;
	int bad = 0;
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name.size() > 16 || !seen.insert( p.name ).second )
			++bad;

	//The name the host reads: plugMain's info block, 16 bytes, not
	//null-terminated.
	const FFMixed info            = plugMain( FF_GET_INFO, FFMixed{ 0 }, 0 );
	const PluginInfoStruct* block = static_cast< const PluginInfoStruct* >( info.PointerValue );
	std::string name, id;
	if( block )
	{
		name.assign( block->PluginName, strnlen( block->PluginName, 16 ) );
		id.assign( block->PluginUniqueID, 4 );
	}
	return report( bad == 0 && block && name == "SW Databend" && id == "DB01" && block->PluginType == FF_EFFECT, quiet,
	               "names: %zu parameters, unique and within 16 characters; the host reads '%s' / %s / %s", seen.size(), name.c_str(),
	               id.c_str(), block && block->PluginType == FF_EFFECT ? "effect" : "not an effect" );
}

/// --window: the restart window over the whole control range is under
/// kMaxWindow (so every setting ships with a proved bound), the plugin's
/// iteration agrees with the stated one, and the table is printed.
int runWindow( bool quiet = false )
{
	int worstK = 0;
	bool agree = true;
	if( !quiet )
		std::printf( "   stages  depth   8-bit  16-bit  float   (worst input, a 4K interleaved stream)\n" );
	for( int stages : { 2, 4, 6, 8, 12 } )
		for( float depth : { 0.0f, 0.5f, 1.0f } )
		{
			int K[ 3 ];
			for( int format = 0; format < 3; ++format )
			{
				//The largest input the chain can hand the phaser: unit
				//samples, the echo at 7/8 and Echo Mix 1 (the flanger is a
				//crossfade and adds nothing).
				const double X = 1.0 + 0.875 / 0.125;
				const double N = 3.0 * 3840.0 * 2160.0;
				K[ format ]    = statedWindow( stages, 0.9, statedSMax( depth ), X, N, statedCode( format ), 1.0 );
				const int plugin = model::PhaserWindow( stages, model::kPoleMax, statedSMax( depth ), X, N, model::CodeOf( format ), 1.0, model::kChunk );
				agree            = agree && plugin == K[ format ];
				worstK           = std::max( worstK, K[ format ] );
			}
			if( !quiet )
				std::printf( "   %6d  %5.2f  %6d  %6d  %6d\n", stages, depth, K[ 0 ], K[ 1 ], K[ 2 ] );
		}
	//And at the defaults, for the record.
	const int atDefault = statedWindow( 4, 0.9, statedSMax( 0.5f ), 1.0 + 0.5 * ( 7.0 / 16.0 ) / ( 9.0 / 16.0 ), 3.0 * 1920.0 * 1080.0, statedCode( 0 ), 0.35 );
	return report( agree && worstK <= model::kMaxWindow, quiet, "window: the phaser's restart window is %d samples at the defaults (1080p) and at most %d over the control range (limit %d); the plugin's bound agrees with the stated one", atDefault, worstK, model::kMaxWindow );
}

//---------------------------------------------------------------------------
// --negative: every check above can fail.
//---------------------------------------------------------------------------
struct NegativeControl
{
	const char* what;
	int failuresSeen;
};

int summariseNegatives( const std::vector< NegativeControl >& controls )
{
	int failures = 0;
	for( const NegativeControl& c : controls )
	{
		const bool ok = c.failuresSeen > 0;
		++g_checks;
		std::printf( "negative %-64s %s  %s\n", c.what, ok ? "it failed" : "it PASSED", verdict( ok ) );
		if( !ok )
		{
			++failures;
			++g_failures;
		}
	}
	std::printf( "%s\n", failures == 0 ? "negative: every perturbed effect is caught" : "negative: FAILURES -- a check cannot fail" );
	return failures;
}

/// Run a check against a perturbation quietly, and report whether it failed
/// -- without counting its failures as the run's.
template< typename F >
int caught( F&& check )
{
	const int checks = g_checks, failures = g_failures;
	const int seen   = check();
	g_checks         = checks;
	g_failures       = failures;
	return seen;
}

int runNegative( int W, int H )
{
	return summariseNegatives( {
		{ "echo: the stride ignores Line Padding", caught( [ & ] { return runEcho( W, H, model::kPerturbNoPadding, true ); } ) },
		{ "feedback: every echo at gain g instead of g^n", caught( [ & ] { return runFeedback( W, H, model::kPerturbEchoFlatGain, true ); } ) },
		{ "interleave: the stream laid out Planar (the spec's)", caught( [ & ] { return runInterleave( W, H, model::kPerturbLayoutPlanar, true ); } ) },
		{ "flanger: the LFO frozen at phase 0", caught( [ & ] { return runFlanger( W, H, model::kPerturbLfoFrozen, true ); } ) },
		{ "allpass: the window cut to a quarter of its bound (the spec's)", caught( [ & ] { return runAllpass( W, H, model::kPerturbShortWindow, true ); } ) },
		{ "wrap: an integer format clips whatever Overflow says", caught( [ & ] { return runWrap( W, H, model::kPerturbNoWrap, true ); } ) },
		{ "pitch: the shifter reads at 1 / r", caught( [ & ] { return runPitch( W, H, model::kPerturbPitchInverse, true ); } ) },
	} );
}

//---------------------------------------------------------------------------
// The moving card, for --out, the sweep, the bench and a default --pipe:
// colour bars (hard edges in every channel), a ramp, a block that cuts, a
// moving bar.
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, int64_t frame )
{
	std::vector< unsigned char > img( static_cast< size_t >( width ) * height * 4 );
	const double t    = static_cast< double >( frame ) / 60.0;
	const bool flash  = std::fmod( t, 0.4 ) < 0.2;
	const double barX = std::fmod( 40.0 + 240.0 * t, static_cast< double >( width ) );
	const unsigned char bars[ 7 ][ 3 ] = { { 191, 191, 191 }, { 191, 191, 0 }, { 0, 191, 191 }, { 0, 191, 0 },
		                                   { 191, 0, 191 },   { 191, 0, 0 },   { 0, 0, 191 } };
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double fx = ( x + 0.5 ) / width, fy = ( y + 0.5 ) / height;
			double r = 16, g = 16, b = 16;
			if( fy < 0.3 )
			{
				const unsigned char* c = bars[ std::min( 6, x * 7 / width ) ];
				r = c[ 0 ];
				g = c[ 1 ];
				b = c[ 2 ];
			}
			else if( fy < 0.45 )
				r = g = b = 255.0 * fx;//a ramp
			else if( fx > 0.3 && fx < 0.7 && fy > 0.5 && fy < 0.85 )
				r = g = b = flash ? 235.0 : 16.0;//the block that cuts
			else if( fy > 0.9 )
			{
				r = 200;
				g = 60;
				b = 30;
			}
			if( std::fabs( x + 0.5 - barX ) < std::max( 2.0, width / 120.0 ) )
				r = g = b = 250.0;
			unsigned char* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ] = static_cast< unsigned char >( r );
			px[ 1 ] = static_cast< unsigned char >( g );
			px[ 2 ] = static_cast< unsigned char >( b );
			px[ 3 ] = 255;
		}
	return img;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( const std::vector< std::string >& settings, int width, int height, int frames )
{
	Session session;
	session.floatOutput = false;
	for( const std::string& setting : settings )
	{
		std::string error;
		applySetting( session.plugin, setting, error );
	}
	if( !session.begin( width, height ) )
		return -1.0;

	std::vector< std::vector< unsigned char > > loop;
	for( int i = 0; i < 4; ++i )
		loop.push_back( buildCard( width, height, i * 7 ) );

	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( loop[ static_cast< size_t >( frame ) % loop.size() ] );
	glFinish();

	//Best of three: the GPU is shared with other builds on this machine.
	double best = 1e9;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int i = 0; i < frames; ++i )
			session.renderNow();
		glFinish();
		const double seconds = std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count();
		best                 = std::min( best, seconds * 1000.0 / frames );
	}
	session.end();
	return best;
}

int runBench( const std::vector< std::string >& settings, int frames )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = { { "1280x720  ", 1280, 720 }, { "1920x1080 ", 1920, 1080 }, { "3840x2160 ", 3840, 2160 } };
	std::printf( "%d frames each, best of three runs, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame   %% of a 60fps frame   every effect on\n" );
	std::vector< std::string > every = settings;
	every.push_back( "Pitch On=1" );
	for( const Size& size : sizes )
	{
		const double ms = benchAt( settings, size.width, size.height, frames );
		const double ev = benchAt( every, size.width, size.height, frames );
		std::printf( "%s    %7.3f        %5.1f%%             %7.3f\n", size.name, ms, ms / 16.667 * 100.0, ev );
	}
	std::printf( "\nEach frame: the stream pass, one pass per effect that is on, the display.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	namespace sh = databendfx::shaders;
	const std::pair< const char*, std::string > files[] = {
		{ "vertex.vert", sh::kVertex },   { "stream.frag", sh::Stream() }, { "echo.frag", sh::Echo() },       { "flanger.frag", sh::Flanger() },
		{ "phaser-state.frag", sh::PhaserState() }, { "phaser.frag", sh::Phaser() }, { "pitch.frag", sh::Pitch() },   { "display.frag", sh::Display() },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

/// A slider ramps between its cues; an option, a boolean or an integer
/// STEPS, holding each cue's value until the next cue's frame -- a
/// Layout interpolated halfway is not a layout.
float valueAt( const Track& track, int frame, bool steps )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			if( steps )
				return frame >= b.first ? b.second : a.second;
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"dbtest -- render and measure the Databend stream effects\n"
		"\n"
		"  --out PATH          render the moving card through the plugin (default /tmp/databend.png)\n"
		"  --size WxH          raster (default 1280x720); --width N / --height N also accepted\n"
		"  --frames N          frames to render before reading back (default 40)\n"
		"  --fps N             accepted for the fleet's --pipe contract; this plugin has no clock\n"
		"  --set \"Name=V\"      set a parameter by its display name (element index for options,\n"
		"                      the integer for the counts). Repeatable.\n"
		"  --list              every parameter, its kind, default and range\n"
		"\n"
		"  checks that render, at --size:\n"
		"  --echo              a delay of a line + k samples ghosts one line down and k across; padding moves it as the stride predicts\n"
		"  --feedback          the nth echo sits at n ( k, 1 ) with amplitude m g^n, until it falls below a code\n"
		"  --interleave        a delay = 1 (mod 3) puts R's echo in G, G's in B, B's in the next pixel's R\n"
		"  --flanger           the ghost's offset on every line is the LFO's value at that line's stream time\n"
		"  --allpass           the phaser keeps a noise line's energy; the windowed GPU form matches the serial cascade\n"
		"  --wrap              8-bit wraps past full scale, 16-bit wraps, float clips\n"
		"  --pitch             a bar pattern's period is stretched by 1 / r inside a grain\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks verbosely against a perturbed plugin (bits in Model.h)\n"
		"\n"
		"  checks that need no GL:\n"
		"  --laws              every control law against its statement; the dyadic promises; the tap count\n"
		"  --names             nothing the host will silently truncate; the host reads SW Databend / DB01\n"
		"  --window            the phaser's restart window over the control range, under its limit\n"
		"  --offline           all three; says loudly what it skipped. For CI.\n"
		"  --allow-no-gl       with the rendering checks: SKIP loudly, not FAIL, when no GL 4.1 context exists\n"
		"\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/databend.png";
	std::string scriptPath;
	std::string dumpDir;
	int width      = 1280;
	int height     = 720;
	int frames     = 40;
	int failRender = -1;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	const std::set< std::string > rendered = { "--echo", "--feedback", "--interleave", "--flanger", "--allpass", "--wrap", "--pitch", "--negative" };
	const std::set< std::string > offline  = { "--laws", "--names", "--window" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--fail-render-at" && hasNext )
			failRender = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( argument == "--offline" )
			for( const char* m : { "--laws", "--names", "--window" } )
				checks.push_back( m );
		else if( rendered.count( argument ) || offline.count( argument ) )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed: answered before a context is made, so it works in CI.
		Databend plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	if( !checks.empty() )
	{
		bool needGL     = false;
		bool offlineRan = false;
		for( const std::string& check : checks )
		{
			if( check == "--laws" )
				runLaws();
			else if( check == "--names" )
				runNames();
			else if( check == "--window" )
				runWindow();
			else
			{
				needGL = true;
				continue;
			}
			offlineRan = true;
			std::printf( "\n" );
		}
		if( offlineRan && !needGL )
			std::printf( "   OFFLINE: --echo, --feedback, --interleave, --flanger, --allpass, --wrap,\n"
			             "   --pitch and their negative controls were NOT run. Nothing here drew a pixel\n"
			             "   through a GL driver; the shaders were not exercised, only (in CI) compiled by\n"
			             "   glslc. The laws and the window have no negative control of their own.\n\n" );

		if( needGL )
		{
			CGLContextObj context = createContext();
			if( context == nullptr && allowNoGL )
				std::printf( "   SKIP  could not create an OpenGL 4.1 core context, accelerated or software.\n"
				             "         The rendering checks and their negative controls were NOT run.\n" );
			else if( context == nullptr )
			{
				std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
				++g_failures;
			}
			else
			{
				for( const std::string& check : checks )
				{
					if( check == "--echo" )
						runEcho( width, height, perturb );
					else if( check == "--feedback" )
						runFeedback( width, height, perturb );
					else if( check == "--interleave" )
						runInterleave( width, height, perturb );
					else if( check == "--flanger" )
						runFlanger( width, height, perturb );
					else if( check == "--allpass" )
						runAllpass( width, height, perturb );
					else if( check == "--wrap" )
						runWrap( width, height, perturb );
					else if( check == "--pitch" )
						runPitch( width, height, perturb );
					else if( check == "--negative" )
						runNegative( width, height );
					else
						continue;
					std::printf( "\n" );
				}
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
			}
		}
		std::printf( "%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( wantBench )
		return finish( runBench( settings, frames < 40 ? 60 : frames ) );

	Session session;
	session.floatOutput = false;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}

	if( wantPipe )
	{
		//Everything but the video goes to stderr: one stray byte in stdout is
		//a torn frame for the rest of the reel.
		std::map< unsigned int, std::pair< Track, bool > > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				const unsigned int id = static_cast< unsigned int >( index );
				automation[ id ]      = { entry.second, stepsBetweenCues( session.plugin, id ) };
			}
		}

		//A closed stdout must be a failed write we can see, not a SIGPIPE
		//that kills the process with 141 before it can say so.
		std::signal( SIGPIPE, SIG_IGN );

		if( !session.begin( width, height ) )
			return finish( 1 );

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame is the end of the stream, never a frame.
			if( got < frame.size() )
			{
				if( got > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", got, frame.size(), width, height );
				break;
			}

			//Through the plugin's own setter, so a cue moves what a slider would.
			for( const auto& track : automation )
				session.plugin.SetFloatParameter( track.first, valueAt( track.second.first, index, track.second.second ) );

			const bool renderedOk = index != failRender && session.render( frame );
			if( !renderedOk )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	if( !session.begin( width, height ) )
		return finish( 1 );
	for( int frame = 0; frame < frames; ++frame )
		if( !session.render( buildCard( width, height, frame ) ) )
			return finish( 1 );

	const std::vector< unsigned char > image = session.readBack();
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
