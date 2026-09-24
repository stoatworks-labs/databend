#include "Shaders.h"

namespace databendfx::shaders
{

const char* const kVertex = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

namespace
{

const char* const kHeader = "#version 410 core\n";

//---------------------------------------------------------------------------
// The stream library: what every pass over the stream needs.
//
// Stream sample n lives at texel ( n mod Stride, n / Stride ). Anything
// before the file's start or past its end is silence, 0 -- which is black
// in an unsigned format and mid-grey in a signed one, because the stream
// holds the signal in the file's own domain (see the stream pass).
//
// The LFO is a triangle in stream time: phase = ( ( n + Off ) mod P ) / P,
// tri = 1 - | 2 phase - 1 |, both exact in float for P under 2^24 -- so a
// check against it can derive its tolerance rather than fit it, which a
// GPU sin (no accuracy requirement in GLSL 4.10) would not allow. Off
// carries the phase from frame to frame; Perturb 8 freezes it at 0 (a
// negative control).
//---------------------------------------------------------------------------
const char* const kStreamLibrary = R"(
uniform int Layout;    //0 Planar, 1 Interleaved
uniform int Lines;     //scan lines
uniform int Pixels;    //pixels per line
uniform int Stride;    //samples per line, padding included
uniform int Rows;      //texture rows of a stream buffer
uniform int Perturb;

float fetch( sampler2D stream, int n )
{
	if( n < 0 || n >= Stride * Rows )
		return 0.0;
	return texelFetch( stream, ivec2( n % Stride, n / Stride ), 0 ).r;
}

float tri( float phase )
{
	return 1.0 - abs( 2.0 * phase - 1.0 );
}

float lfo( int n, int period, int offset )
{
	if( ( Perturb & 8 ) != 0 )
		return 0.0;
	return tri( float( ( n + offset ) % period ) / float( period ) );
}

//A fractional delay: the linear interpolation of the two neighbouring
//samples, which is what a delay line with a fractional read head does.
float tap( sampler2D stream, int n, float delay )
{
	int whole = int( floor( delay ) );
	float part = delay - float( whole );
	return mix( fetch( stream, n - whole ), fetch( stream, n - whole - 1 ), part );
}
)";

//---------------------------------------------------------------------------
// stream: the picture as the file lays it out.
//
// Texel ( col, row ) of the stream buffer is sample n = row Stride + col.
// Interleaved: line row, pixel col / 3, channel col mod 3; samples at or
// past 3 Pixels are padding. Planar: plane row / Lines, line row mod Lines,
// pixel col; samples at or past Pixels are padding. A line is a row of the
// picture (Direction 0) or a column (1); the host's texel row 0 is the
// bottom. The value is the picture's, in the file's domain: an unsigned
// format keeps 0..1 with 0 at black; a signed one takes 2 v - 1, so silence
// is mid-grey. Padding is silence.
//---------------------------------------------------------------------------
const char* const kStream = R"(
uniform sampler2D InputTexture;
uniform int InH;        //the picture's rows
uniform int Direction;  //0 rows, 1 columns
uniform int Signed;     //1: the format's zero is mid-grey

out vec4 fragColor;

void main()
{
	int col = int( gl_FragCoord.x );
	int row = int( gl_FragCoord.y );
	int c, l, x;
	bool padding;
	if( Layout == 1 )
	{
		l       = row;
		c       = col % 3;
		x       = col / 3;
		padding = col >= 3 * Pixels;
	}
	else
	{
		c       = row / Lines;
		l       = row - c * Lines;
		x       = col;
		padding = col >= Pixels;
	}
	float v = 0.0;
	if( !padding )
	{
		ivec2 t = Direction == 1 ? ivec2( l, InH - 1 - x ) : ivec2( x, InH - 1 - l );
		v       = texelFetch( InputTexture, t, 0 )[ c ];
		if( Signed == 1 )
			v = 2.0 * v - 1.0;
	}
	fragColor = vec4( v, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// echo: e = x + m sum_{k=1..K} g^k x[n - kD], evaluated as a Horner sum
// from the farthest tap: acc = g ( x[n - kD] + acc ). K is the plugin's
// tap count for the format's code (Model.h, EchoTaps). Perturb 4 gives
// every tap the gain g (a negative control).
//---------------------------------------------------------------------------
const char* const kEcho = R"(
uniform sampler2D In;
uniform int Delay;      //D, samples
uniform int Taps;       //K
uniform float Gain;     //g
uniform float EchoMix;  //m_e

out vec4 fragColor;

void main()
{
	int n = int( gl_FragCoord.y ) * Stride + int( gl_FragCoord.x );
	float acc = 0.0;
	if( ( Perturb & 4 ) != 0 )
	{
		for( int k = 1; k <= Taps; ++k )
			acc += Gain * fetch( In, n - k * Delay );
	}
	else
	{
		for( int k = Taps; k >= 1; --k )
			acc = Gain * ( fetch( In, n - k * Delay ) + acc );
	}
	fragColor = vec4( fetch( In, n ) + EchoMix * acc, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// flanger: f = ( 1 - m ) e + m e[n - d(n)], d(n) = Base + Width tri( phase( n ) ).
// Feed-forward: the delayed signal is the input's, not the output's, so
// the ghost is one ghost and the comb has one tooth per line crossing. A
// crossfade rather than a sum, so the comb's notches are deepest at m =
// 1/2 and nothing is pushed past full scale by the flanger alone.
//---------------------------------------------------------------------------
const char* const kFlanger = R"(
uniform sampler2D In;
uniform float Base;        //samples
uniform float Width;       //samples
uniform int Period;        //P, samples
uniform int Offset;        //this frame's phase offset, samples
uniform float FlangerMix;  //m_f

out vec4 fragColor;

void main()
{
	int n   = int( gl_FragCoord.y ) * Stride + int( gl_FragCoord.x );
	float d = Base + Width * lfo( n, Period, Offset );
	fragColor = vec4( ( 1.0 - FlangerMix ) * fetch( In, n ) + FlangerMix * tap( In, n, d ), 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// phaser: q = ( 1 - m ) f + m A_S( f ), A_S a cascade of Stages first-order
// all-pass sections in the normalised lattice form,
//
//     y = -k x + s w_prev,  w = s x + k w_prev,  s = sqrt( 1 - k^2 )
//
// a rotation of ( x, w_prev ) -- lossless per sample, whatever k(n) does.
// The pole k(n) sweeps between PoleTop and PoleMin on the LFO, which is
// counted along rather than reduced per sample: the counter starts at
// ( a + Offset ) mod Period and wraps, the same values lfo() gives.
//
// Two passes. The STATE pass has one fragment per chunk boundary b (sample
// b Chunk): it re-runs the cascade from zero state over the Window samples
// before the boundary and writes the state there, twelve floats over three
// attachments. The MAIN pass reads its chunk's boundary state and
// continues from it for at most Chunk steps. The error at a sample is the
// boundary state's error carried j <= Chunk steps further, which for a
// linear system is the homogeneous response and does not depend on the
// signal; Window is chosen on the CPU so the rigorous bound on it is under
// half a code for every step of the chunk (Model.h, PhaserWindow).
//---------------------------------------------------------------------------
const char* const kPhaserLibrary = R"(
uniform int Stages;       //S, 2..12
uniform int Window;       //K
uniform int Chunk;        //samples per chunk
uniform int StateW;       //boundary b is state texel ( b mod StateW, b / StateW )
uniform float PoleTop;    //kappa
uniform float PoleMin;    //kappa ( 1 - depth )
uniform int Period;
uniform int Offset;

//The cascade from sample a to sample b inclusive, from the state in w;
//returns the last output.
float cascade( sampler2D In, inout float w[ 12 ], int a, int b )
{
	float y = 0.0;
	int ph  = ( a + Offset ) % Period;
	bool frozen = ( Perturb & 8 ) != 0;
	for( int i = a; i <= b; ++i )
	{
		float k = PoleTop - ( PoleTop - PoleMin ) * ( frozen ? 0.0 : tri( float( ph ) / float( Period ) ) );
		float s = sqrt( 1.0 - k * k );
		float x = fetch( In, i );
		for( int j = 0; j < Stages; ++j )
		{
			float wp = w[ j ];
			y        = -k * x + s * wp;
			w[ j ]   = s * x + k * wp;
			x        = y;
		}
		if( ++ph == Period )
			ph = 0;
	}
	return y;
}
)";

const char* const kPhaserState = R"(
uniform sampler2D In;

layout( location = 0 ) out vec4 out0;
layout( location = 1 ) out vec4 out1;
layout( location = 2 ) out vec4 out2;

void main()
{
	int b     = int( gl_FragCoord.y ) * StateW + int( gl_FragCoord.x );
	int start = b * Chunk;
	float w[ 12 ];
	for( int j = 0; j < 12; ++j )
		w[ j ] = 0.0;
	int from = max( 0, start - Window );
	if( start < Stride * Rows && from < start )
		cascade( In, w, from, start - 1 );
	out0 = vec4( w[ 0 ], w[ 1 ], w[ 2 ], w[ 3 ] );
	out1 = vec4( w[ 4 ], w[ 5 ], w[ 6 ], w[ 7 ] );
	out2 = vec4( w[ 8 ], w[ 9 ], w[ 10 ], w[ 11 ] );
}
)";

const char* const kPhaser = R"(
uniform sampler2D In;
uniform sampler2D State0;
uniform sampler2D State1;
uniform sampler2D State2;
uniform float PhaserMix;  //m_p

out vec4 fragColor;

void main()
{
	int n     = int( gl_FragCoord.y ) * Stride + int( gl_FragCoord.x );
	int b     = n / Chunk;
	ivec2 at  = ivec2( b % StateW, b / StateW );
	vec4 s0   = texelFetch( State0, at, 0 );
	vec4 s1   = texelFetch( State1, at, 0 );
	vec4 s2   = texelFetch( State2, at, 0 );
	float w[ 12 ];
	w[ 0 ] = s0.x; w[ 1 ] = s0.y; w[ 2 ]  = s0.z; w[ 3 ]  = s0.w;
	w[ 4 ] = s1.x; w[ 5 ] = s1.y; w[ 6 ]  = s1.z; w[ 7 ]  = s1.w;
	w[ 8 ] = s2.x; w[ 9 ] = s2.y; w[ 10 ] = s2.z; w[ 11 ] = s2.w;
	float y   = cascade( In, w, b * Chunk, n );
	float dry = fetch( In, n );
	fragColor = vec4( ( 1.0 - PhaserMix ) * dry + PhaserMix * y, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// pitch: two taps on a sawtooth delay of period Grain and slope 1 - Ratio,
// half a grain apart, crossfaded by complementary triangles. Tap A at
// grain phase q reads n - d_A, d_A = ( 1 - r ) q + max( 0, r - 1 ) Grain:
// inside a grain the input is read at rate r; at the seam the read jumps
// back by ( 1 - r ) Grain (repeats for r < 1, skips for r > 1). Its weight
// is 1 - | 2 q / Grain - 1 |, zero at the seam where its delay jumps; tap
// B is the same half a grain on, so its seam is under A's peak, and the
// weights sum to 1. Perturb 64 reads at 1 / r (a negative control).
//---------------------------------------------------------------------------
const char* const kPitch = R"(
uniform sampler2D In;
uniform int Grain;       //G, samples, even
uniform float Ratio;     //r
uniform float PitchMix;  //m_t

out vec4 fragColor;

float sawDelay( float q, float r )
{
	return ( 1.0 - r ) * q + max( 0.0, r - 1.0 ) * float( Grain );
}

void main()
{
	int n    = int( gl_FragCoord.y ) * Stride + int( gl_FragCoord.x );
	float r  = ( Perturb & 64 ) != 0 ? 1.0 / Ratio : Ratio;
	float qa = float( n % Grain );
	float qb = float( ( n + Grain / 2 ) % Grain );
	float wa = tri( qa / float( Grain ) );
	float wb = 1.0 - wa;
	float wet = wa * tap( In, n, sawDelay( qa, r ) ) + wb * tap( In, n, sawDelay( qb, r ) );
	fragColor = vec4( ( 1.0 - PitchMix ) * fetch( In, n ) + PitchMix * wet, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// display: the stream back onto the host's framebuffer.
//
// Output pixel ( X, Y ), top-first, shows input pixel ( col, rowT ) under
// its centre; its line and position give each channel's stream index, and
// the sample there is exported to the Format: an integer format rounds
// to a code and either wraps it into the format's range (modular, as the
// bytes would) or clips it; a float file clips at full scale. Then back to
// the picture's 0..1 -- a signed format's zero is mid-grey. Mix 1 returns
// early so the exported picture is never a mix's rounding of itself.
//---------------------------------------------------------------------------
const char* const kDisplay = R"(
uniform sampler2D InputTexture;
uniform sampler2D Final;
uniform int InW;
uniform int InH;
uniform int Direction;
uniform int Format;      //0 8-bit unsigned, 1 16-bit signed, 2 float
uniform int Wrap;        //1: an integer format wraps past full scale
uniform int VpX;
uniform int VpY;
uniform int VpW;
uniform int VpH;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

int streamIndex( int l, int x, int c )
{
	if( Layout == 1 )
		return l * Stride + 3 * x + c;
	return ( c * Lines + l ) * Stride + x;
}

//Modular reduction in float, exact for whole numbers under 2^24; GLSL's
//integer % is undefined on a negative operand.
float wrapTo( float code, float low, float count )
{
	return code - count * floor( ( code - low ) / count );
}

float exportSample( float v )
{
	bool wrap = Wrap == 1 && ( Perturb & 32 ) == 0;
	if( Format == 0 )
	{
		float code = floor( v * 255.0 + 0.5 );
		code       = wrap ? wrapTo( code, 0.0, 256.0 ) : clamp( code, 0.0, 255.0 );
		return code / 255.0;
	}
	if( Format == 1 )
	{
		float code = floor( v * 32768.0 + 0.5 );
		code       = wrap ? wrapTo( code, -32768.0, 65536.0 ) : clamp( code, -32768.0, 32767.0 );
		return 0.5 * ( code / 32768.0 + 1.0 );
	}
	return 0.5 * ( clamp( v, -1.0, 1.0 ) + 1.0 );
}

void main()
{
	int X = int( gl_FragCoord.x ) - VpX;
	int Y = VpH - 1 - ( int( gl_FragCoord.y ) - VpY );

	int col  = clamp( ( ( 2 * X + 1 ) * InW ) / ( 2 * VpW ), 0, InW - 1 );
	int rowT = clamp( ( ( 2 * Y + 1 ) * InH ) / ( 2 * VpH ), 0, InH - 1 );
	vec4 x   = texelFetch( InputTexture, ivec2( col, InH - 1 - rowT ), 0 );

	int l = Direction == 1 ? col : rowT;
	int p = Direction == 1 ? rowT : col;

	vec3 rgb;
	for( int c = 0; c < 3; ++c )
		rgb[ c ] = exportSample( fetch( Final, streamIndex( l, p, c ) ) );
	vec4 exported = vec4( rgb, max( x.a, max( rgb.r, max( rgb.g, rgb.b ) ) ) );

	if( MixAmount >= 1.0 )
	{
		fragColor = exported;
		return;
	}
	fragColor = mix( x, exported, MixAmount );
}
)";

std::string assemble( const char* body )
{
	return std::string( kHeader ) + kStreamLibrary + body;
}

std::string assemblePhaser( const char* body )
{
	return std::string( kHeader ) + kStreamLibrary + kPhaserLibrary + body;
}

} // namespace

std::string Stream()
{
	return assemble( kStream );
}
std::string Echo()
{
	return assemble( kEcho );
}
std::string Flanger()
{
	return assemble( kFlanger );
}
std::string PhaserState()
{
	return assemblePhaser( kPhaserState );
}
std::string Phaser()
{
	return assemblePhaser( kPhaser );
}
std::string Pitch()
{
	return assemble( kPitch );
}
std::string Display()
{
	return assemble( kDisplay );
}

} // namespace databendfx::shaders
