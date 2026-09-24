/**
 * Databend — browser demo.
 *
 * The raster as a PCM stream through audio effects. The one idea, from
 * `source/Model.h`: databending is opening a picture in an audio editor and
 * running audio effects on it, and the editor sees one long stream of samples
 * in the order the file lays them out — row by row, one plane after another
 * (Planar) or R, G, B per pixel (Interleaved), with a row stride that may carry
 * padding — so a delay-line effect on that stream does not know where the
 * lines are. An echo of D = k·L + m samples comes back k lines down and m
 * samples across and marches on a slant with feedback; a flanger, a delay swept
 * by an LFO in stream time, is a comb across the scanlines; a phaser, a cascade
 * of all-pass stages, smears the picture rightwards with ringing and keeps its
 * energy; a pitch shifter stretches the scan inside each grain. The file's
 * sample format decides what happens past full scale: an 8-bit file wraps or
 * clips, a float file clips.
 *
 * This plugin is **almost entirely a shader**: every sample of every effect is
 * computed in GLSL, and the C++ converts sliders to physical units
 * (`Controls.cpp`), computes two integers once a frame — the echo's tap count
 * and the phaser's restart window, both from a bound in `Model.h` — and
 * schedules the passes (`Databend.cpp`). So the two halves of this page are not
 * equally faithful:
 *
 *   The shaders are the plugin's. The ten GLSL bodies below — the vertex
 *   shader, the stream library, five whole fragment mains, the phaser library
 *   and its two mains — are `kVertex`, `kStreamLibrary`, `kStream`, `kEcho`,
 *   `kFlanger`, `kPhaserLibrary`, `kPhaserState`, `kPhaser`, `kPitch` and
 *   `kDisplay` from `source/Shaders.cpp`, copied across unedited and assembled
 *   the way the plugin assembles them (`assemble` and `assemblePhaser` below
 *   are Shaders.cpp's). `demo/tools/check_shaders.py` compares all ten
 *   character for character, and the one header line, and `tools/verify.sh`
 *   runs it.
 *
 *   The CPU half is a PORT — of `Controls.cpp` (every slider to its unit), of
 *   `Model.h` (`GeometryOf`, `StreamIndex`, `CodeOf`, `EchoTaps`, and
 *   `PhaserWindow`, the rigorous worst-case bound iterated in double until the
 *   output error is under half a code of the format), and of the per-frame
 *   arithmetic and pass order in `Databend::ProcessOpenGL` (the delay in
 *   samples, the echo's input magnitude, the LFOs' frame offsets, the state
 *   buffer's size) — function for function, in the same double precision (a
 *   JavaScript number is an IEEE double, which is what the C++ computes in;
 *   the host's float values are rounded through Math.fround first, as the
 *   plugin holds them). Nothing checks a port but a reader. The repository's
 *   `dbtest --laws` and `--window` check the C++ against the model's statement
 *   and have never heard of this page.
 *
 * ------------------------------------------------------- the buffers
 *
 * As in the plugin: two R32F stream buffers of Stride × Rows texels, one per
 * sample, ping-ponged between the effects (a bypassed effect is skipped, not
 * copied); a three-attachment RGBA32F state buffer holding the phaser
 * cascade's twelve states at every 16-sample chunk boundary; and the display
 * pass onto the canvas. Every one is Nearest and read by texelFetch. WebGL2
 * renders into float textures only with EXT_color_buffer_float, and the page
 * refuses to start without it rather than quantise the stream to 8 bits.
 *
 * ------------------------------------------------------- the clock
 *
 * The plugin has no clock (SetTimeSupported( false )): the LFOs run on the
 * file's own nominal 44.1 kHz, and their phase carries from frame to frame on
 * a frame counter, as if the clip were one file. Here that counter is the
 * kit's frame index — advanced while playing and by Step, held by Pause, sent
 * to 0 by Restart — which wraps at 100,000 frames where the plugin's uint32
 * does not; at that frame the LFOs' phase jumps once.
 *
 * ------------------------------------------------------- what is missing
 *
 * **Nothing audio.** Databend has no audio path: the "audio effects" run on
 * the picture's samples, and no sound is involved anywhere. **The About block
 * is absent**, as on every page in this suite. **The four integer controls are
 * dropdowns**: Line Padding, Delay Lines, Delay Samples and Stages are
 * FF_TYPE_INTEGER in the plugin with real ranges, the kit has no integer
 * control, so — as copperlist, galvo, teletext and toner did — each is a
 * dropdown of every value in its range (Delay Samples has 4,096 of them). The
 * harness-only `Perturb` uniform is set to what the shipped plugin sets it
 * to: 0.
 *
 * And what every page in this suite is not: this is the plugin's shaders and a
 * port of its C++, not the plugin. No Resolume, no composition, no FFGL, and
 * GLSL ES 3.00 in a browser rather than desktop GL 4.1 core.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture, GLError } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here. The vertex
// shader carries its own #version line; every fragment shader is assembled as
// header + stream library (+ phaser library) + body, exactly as Shaders.cpp's
// `assemble` and `assemblePhaser` do.
//---------------------------------------------------------------------------

const HEADER = '#version 410 core\n';

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

const STREAM_LIB = `
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
`;

const STREAM = `
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
`;

const ECHO = `
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
`;

const FLANGER = `
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
`;

const PHASER_LIB = `
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
`;

const PHASER_STATE = `
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
`;

const PHASER = `
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
`;

const PITCH = `
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
`;

const DISPLAY = `
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
`;

const assemble = (body) => HEADER + STREAM_LIB + body;
const assemblePhaser = (body) => HEADER + STREAM_LIB + PHASER_LIB + body;

//===========================================================================
// Model.h, ported. The stream and the effects as arithmetic: the constants,
// the option tables, the geometry, the echo's tap count and the phaser's
// window bound. The effects themselves are the GLSL above.
//===========================================================================

/// The file's nominal sample rate: what turns an LFO rate in Hz into a
/// period in samples of the scan.
const K_SAMPLE_RATE = 44100.0;

/// The largest pole the phaser's sweep reaches: the top of the sweep range,
/// and kappa in the window bound.
const K_POLE_MAX = 0.9;

/// The restart window is never longer than this.
const K_MAX_WINDOW = 1024;

/// The phaser's state pass runs once per chunk boundary (one fragment per
/// kChunk samples); the main pass continues for at most kChunk steps.
const K_CHUNK = 16;

/// The state buffer's width: boundary b is texel ( b mod kStateWidth, b / kStateWidth ).
const K_STATE_WIDTH = 4096;

/// A float file has no code; for the truncation and restart bounds one
/// 16-bit code is taken.
const K_FLOAT_CODE = 1.0 / 65536.0;

const K_PLANAR = 0;
const K_INTERLEAVED = 1;
const LAYOUT_NAMES = ['Planar', 'Interleaved'];

const K_ROWS = 0;
const K_COLUMNS = 1;
const DIRECTION_NAMES = ['Rows', 'Columns'];

const K_U8 = 0;
const K_S16 = 1;
const K_FLOAT = 2;
const FORMAT_NAMES = ['8-bit unsigned', '16-bit signed', 'Float'];

const K_WRAP = 0;
const K_CLIP = 1;
const OVERFLOW_NAMES = ['Wrap', 'Clip'];

/// Whether a format's zero is mid-grey (signed) rather than black.
const isSigned = (format) => format !== K_U8;

/// One code of a format in SIGNAL units.
function codeOf(format) {
  switch (format) {
    case K_U8: return 1.0 / 255.0;
    case K_S16: return 1.0 / 32768.0;
    default: return K_FLOAT_CODE;
  }
}

/// Integer parameter ranges.
const K_PADDING_MAX = 255;
const K_DELAY_LINES_MAX = 32;
const K_DELAY_SAMPLES_MAX = 4095;
const K_STAGES_MIN = 2;
const K_STAGES_MAX = 12;

/// The stream's geometry for a picture of `width` x `height`.
function geometryOf(width, height, layout, direction, padding) {
  const lines = direction === K_COLUMNS ? width : height;
  const pixels = direction === K_COLUMNS ? height : width;
  const stride = (layout === K_INTERLEAVED ? 3 * pixels : pixels) + padding;
  const rows = layout === K_INTERLEAVED ? lines : 3 * lines;
  // int64_t in the C++; a double is exact to 2^53 and a stream is under 2^27.
  const total = stride * rows;
  return { lines, pixels, stride, rows, total };
}

/// The echo's tap count: the first K at which the remainder of a unit
/// input's echoes, g^( K + 1 ) / ( 1 - g ), is at or under one code. 0 for
/// no feedback.
function echoTaps(g, code) {
  if (g <= 0.0) return 0;
  let K = 0;
  let remainder = g / (1.0 - g);
  while (remainder > code) {
    remainder *= g;
    K += 1;
  }
  return K;
}

/// The phaser's restart window: the rigorous worst-case bound iterated
/// until the output error is under half a code and stays there for
/// `chunk` more steps. Model.h's PhaserWindow, line for line, Newton's
/// square root included (the C++ avoids <cmath> there; the same twelve-odd
/// steps give the same double here).
function phaserWindow(stages, kappa, smax, x, total, code, mix, chunk) {
  if (stages <= 0 || kappa <= 0.0 || mix <= 0.0) return 1;
  const W = new Float64Array(K_STAGES_MAX);
  let xi = x;
  let root = total > 1.0 ? total : 1.0;
  for (let i = 0; i < 60; i += 1) root = 0.5 * (root + total / root);
  const energy = x * root;
  for (let i = 0; i < stages; i += 1) {
    let wi = xi / (1.0 - kappa);
    if (wi > energy) wi = energy;
    W[i] = wi;
    xi = kappa * xi + smax * wi;
  }
  const ew = new Float64Array(K_STAGES_MAX);
  for (let i = 0; i < stages; i += 1) ew[i] = W[i];
  let streakStart = -1;
  for (let t = 1; t <= K_MAX_WINDOW + chunk; t += 1) {
    let eyPrev = 0.0;
    for (let i = 0; i < stages; i += 1) {
      const ey = kappa * eyPrev + smax * ew[i];
      ew[i] = smax * eyPrev + kappa * ew[i];
      eyPrev = ey;
    }
    if (mix * eyPrev < code / 2.0) {
      if (streakStart < 0) streakStart = t;
      if (t - streakStart + 1 >= chunk) return streakStart - 1;
    } else {
      streakStart = -1;
    }
  }
  return K_MAX_WINDOW;
}

//===========================================================================
// Controls.cpp, ported. What a host parameter means.
//
// The plugin stores every host value as a `float`, and each law takes that
// float. The page's values are doubles from a slider, so each is rounded
// through Math.fround first — the same 24-bit value the plugin holds — before
// the law is applied in double, as the C++ does.
//===========================================================================

const unit = (value) => Math.min(Math.max(Math.fround(value), 0.0), 1.0);
const lround = (value) => (value < 0 ? -Math.round(-value) : Math.round(value));

/// 2^e as ldexp( exp2( e - floor e ), floor e ): a multiply by a power of
/// two is exact, so the dyadic settings are dyadic in fact.
function pow2(e) {
  const whole = Math.floor(e);
  return Math.pow(2.0, e - whole) * Math.pow(2.0, whole);
}

const optionIndex = (value, count) => Math.min(Math.max(lround(Math.fround(value)), 0), count - 1);
const integerOf = (value, low, high) => Math.min(Math.max(lround(Math.fround(value)), low), high);

/// Feedback: g = 7/8 v -- 7/16 at the default 0.5, 7/8 at 1, never 1.
const feedbackOf = (value) => 0.875 * unit(value);
/// The three effect mixes and the output mix are the slider itself.
const mixOf = (value) => unit(value);
/// Base Delay and Depth: 64 v samples of the stream, 0..64.
const flangerSamples = (value) => 64.0 * unit(value);
/// Rate and Phaser Rate: 0.05 x 400^v Hz, 1 Hz at the default 0.5.
const rateHz = (value) => 0.05 * Math.pow(400.0, unit(value));
/// An LFO's period in samples, round( 44100 / rate ), at least 2.
const lfoPeriod = (value) => Math.max(2, lround(K_SAMPLE_RATE / rateHz(value)));
/// Phaser Depth: the sweep's bottom pole, kPoleMax ( 1 - v ).
const poleMin = (value) => K_POLE_MAX * (1.0 - unit(value));
/// The largest s = sqrt( 1 - k^2 ) the sweep reaches, from its smallest pole.
function sMax(value) {
  const k = poleMin(value);
  return Math.sqrt(1.0 - k * k);
}
/// Ratio: 2^( 2 v - 1 ), half to double, unity at 0.5.
const ratioOf = (value) => pow2(2.0 * unit(value) - 1.0);
/// Grain: round( 64 x 2^( 6 v ) ) samples made even, 64 to 4096, 512 at 0.5.
function grainOf(value) {
  const g = lround(64.0 * pow2(6.0 * unit(value)));
  return Math.min(Math.max(g + (g & 1), 64), 4096);
}

//===========================================================================
// The state buffer: StateBuffer.cpp, in WebGL2. A three-attachment RGBA32F
// framebuffer, Nearest, cleared on allocation. The kit's PassBuffer has one
// attachment, so this is its own class.
//===========================================================================

class StateBuffer {
  constructor(gl) {
    this.gl = gl;
    this.width = 0;
    this.height = 0;
    this.fbo = null;
    this.textures = [];
  }

  ensure(width, height) {
    const gl = this.gl;
    if (this.fbo && this.width === width && this.height === height) return this;
    this.dispose();
    this.width = width;
    this.height = height;
    const attachments = [gl.COLOR_ATTACHMENT0, gl.COLOR_ATTACHMENT1, gl.COLOR_ATTACHMENT2];
    this.fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
    for (let q = 0; q < 3; q += 1) {
      const texture = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA32F, width, height);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, attachments[q], gl.TEXTURE_2D, texture, 0);
      this.textures.push(texture);
    }
    gl.drawBuffers(attachments);
    const status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
    if (status === gl.FRAMEBUFFER_COMPLETE) {
      // Cleared: a state buffer whose contents are undefined is whatever the
      // driver handed back.
      gl.viewport(0, 0, width, height);
      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.bindTexture(gl.TEXTURE_2D, null);
    if (status !== gl.FRAMEBUFFER_COMPLETE) {
      this.dispose();
      throw new GLError(`the phaser's state buffer is incomplete (0x${status.toString(16)}) at ${width}x${height}: three RGBA32F attachments`);
    }
    return this;
  }

  bind() {
    const gl = this.gl;
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
    gl.viewport(0, 0, this.width, this.height);
    return this;
  }

  dispose() {
    const gl = this.gl;
    if (this.fbo) gl.deleteFramebuffer(this.fbo);
    for (const t of this.textures) gl.deleteTexture(t);
    this.fbo = null;
    this.textures = [];
    this.width = 0;
    this.height = 0;
  }
}

//===========================================================================
// The renderer: Databend::ProcessOpenGL, in its order. The settings in
// physical units; the geometry, the delay, the tap count and the window; the
// buffers; then the stream pass, each effect that is on (the phaser as two
// passes), and the display onto the canvas.
//===========================================================================

/// What the line under the canvas reports. Filled by the renderer.
const telemetry = {
  lines: 0, pixels: 0, stride: 0, rows: 0, total: 0, delay: 0, taps: 0, window: 0,
  periodF: 0, periodP: 0, offsetF: 0, offsetP: 0, frame: 0, stateRows: 0, chain: '',
};

/// The harness-only negative-control bitmask, always 0 in the shipped plugin.
const PERTURB = 0;

function createRenderer(gl, quad) {
  const program = (fragment, label) => new Program(gl, VERTEX, fragment, label);
  const streamShader = program(assemble(STREAM), 'stream');
  const echoShader = program(assemble(ECHO), 'echo');
  const flangerShader = program(assemble(FLANGER), 'flanger');
  const phaserStateShader = program(assemblePhaser(PHASER_STATE), 'phaser state');
  const phaserShader = program(assemblePhaser(PHASER), 'phaser');
  const pitchShader = program(assemble(PITCH), 'pitch');
  const displayShader = program(assemble(DISPLAY), 'display');

  // The two stream buffers, R32F Nearest, as PassBuffer::Ensure( …, GL_R32F, Nearest ).
  const nearest = { filter: 'nearest' };
  const streams = [new PassBuffer(gl, nearest), new PassBuffer(gl, nearest)];
  const states = new StateBuffer(gl);

  /// Databend.cpp's setLibrary: what every pass over the stream needs.
  const setLibrary = (shader, layout, g) => {
    shader.setInt('Layout', layout);
    shader.setInt('Lines', g.lines);
    shader.setInt('Pixels', g.pixels);
    shader.setInt('Stride', g.stride);
    shader.setInt('Rows', g.rows);
    shader.setInt('Perturb', PERTURB);
  };

  return {
    render({ input, params, width: vpW, height: vpH, frameIndex }) {
      const p = (id) => params.get(id);
      const picture = input;
      const width = picture.width;
      const height = picture.height;
      const frame = frameIndex;

      //------------------------------------------------------------------
      // The settings, in physical units.
      //------------------------------------------------------------------
      const layout = optionIndex(p('layout'), LAYOUT_NAMES.length);
      const padding = integerOf(integerValue('padding', p('padding')), 0, K_PADDING_MAX);
      const direction = optionIndex(p('direction'), DIRECTION_NAMES.length);
      const format = optionIndex(p('format'), FORMAT_NAMES.length);
      const overflow = optionIndex(p('overflow'), OVERFLOW_NAMES.length);
      const code = codeOf(format);

      const echoOn = Math.fround(p('echoOn')) >= 0.5;
      const delayLines = integerOf(integerValue('delayLines', p('delayLines')), 0, K_DELAY_LINES_MAX);
      const delaySamples = integerOf(integerValue('delaySamples', p('delaySamples')), 0, K_DELAY_SAMPLES_MAX);
      const feedback = feedbackOf(p('feedback'));
      const echoMix = mixOf(p('echoMix'));

      const flangerOn = Math.fround(p('flangerOn')) >= 0.5;
      const baseDelay = flangerSamples(p('baseDelay'));
      const depth = flangerSamples(p('depth'));
      const periodF = lfoPeriod(p('rate'));
      const flangerMix = mixOf(p('flangerMix'));

      const phaserOn = Math.fround(p('phaserOn')) >= 0.5;
      const stages = integerOf(integerValue('stages', p('stages')), K_STAGES_MIN, K_STAGES_MAX);
      const periodP = lfoPeriod(p('phaserRate'));
      const pole = poleMin(p('phaserDepth'));
      const smax = sMax(p('phaserDepth'));
      const phaserMix = mixOf(p('phaserMix'));

      const pitchOn = Math.fround(p('pitchOn')) >= 0.5;
      const ratio = ratioOf(p('ratio'));
      const grain = grainOf(p('grain'));
      const pitchMix = mixOf(p('pitchMix'));

      //------------------------------------------------------------------
      // Geometry: the file's layout. The perturbations are 0 here.
      //------------------------------------------------------------------
      const g = geometryOf(width, height, layout, direction, padding);
      const delay = delayLines * g.stride + delaySamples;
      const taps = echoTaps(feedback, code);

      // The phaser's input can be larger than the file's samples: the echo
      // adds up to m_e g / ( 1 - g ) of a unit input.
      let magnitude = 1.0;
      if (echoOn && feedback > 0.0) magnitude *= 1.0 + echoMix * feedback / (1.0 - feedback);
      const window = phaserWindow(stages, K_POLE_MAX, smax, magnitude, g.total, code, phaserMix, K_CHUNK);

      // The LFOs' phase at this frame's first sample, as if the clip were one
      // file: frame x total samples, reduced. The C++ does this in int64; a
      // double is exact to 2^53 and frame x total is under 2^45 here.
      const offsetF = (frame * g.total) % periodF;
      const offsetP = (frame * g.total) % periodP;

      //------------------------------------------------------------------
      // Buffers. Every allocation before anything binds a texture.
      //------------------------------------------------------------------
      const boundaries = Math.floor((g.total + K_CHUNK - 1) / K_CHUNK);
      const stateRows = Math.floor((boundaries + K_STATE_WIDTH - 1) / K_STATE_WIDTH);
      const maxSize = gl.getParameter(gl.MAX_TEXTURE_SIZE);
      if (g.stride > maxSize || g.rows > maxSize) {
        throw new GLError(`the stream buffer would be ${g.stride} samples x ${g.rows} rows and this GPU allows ${maxSize}: choose a smaller composition, Rows, or Planar`);
      }
      streams[0].ensure(g.stride, g.rows, gl.R32F);
      streams[1].ensure(g.stride, g.rows, gl.R32F);
      if (phaserOn) states.ensure(K_STATE_WIDTH, stateRows);

      gl.disable(gl.BLEND);

      //------------------------------------------------------------------
      // 1. The stream.
      //------------------------------------------------------------------
      streams[0].bind();
      streamShader.use();
      bindTexture(gl, 0, picture.texture);
      setLibrary(streamShader, layout, g);
      streamShader.setSampler('InputTexture', 0);
      streamShader.setInt('InH', height);
      streamShader.setInt('Direction', direction);
      streamShader.setInt('Signed', isSigned(format) ? 1 : 0);
      quad.draw();

      //------------------------------------------------------------------
      // 2. The effects, each from one buffer into the other. A bypassed
      //    effect is skipped: the next one reads what the last one wrote.
      //------------------------------------------------------------------
      let current = 0;
      const chain = [];
      const effect = (shader, uniforms) => {
        const target = 1 - current;
        streams[target].bind();
        shader.use();
        bindTexture(gl, 0, streams[current].texture);
        setLibrary(shader, layout, g);
        shader.setSampler('In', 0);
        uniforms();
        quad.draw();
        current = target;
      };

      if (echoOn) {
        chain.push('echo');
        effect(echoShader, () => {
          echoShader.setInt('Delay', delay);
          echoShader.setInt('Taps', taps);
          echoShader.set('Gain', Math.fround(feedback));
          echoShader.set('EchoMix', Math.fround(echoMix));
        });
      }
      if (flangerOn) {
        chain.push('flanger');
        effect(flangerShader, () => {
          flangerShader.set('Base', Math.fround(baseDelay));
          flangerShader.set('Width', Math.fround(depth));
          flangerShader.setInt('Period', periodF);
          flangerShader.setInt('Offset', offsetF);
          flangerShader.set('FlangerMix', Math.fround(flangerMix));
        });
      }
      if (phaserOn) {
        chain.push('phaser');
        const phaserUniforms = (shader) => {
          shader.setInt('Stages', stages);
          shader.setInt('Window', window);
          shader.setInt('Chunk', K_CHUNK);
          shader.setInt('StateW', K_STATE_WIDTH);
          shader.set('PoleTop', Math.fround(K_POLE_MAX));
          shader.set('PoleMin', Math.fround(pole));
          shader.setInt('Period', periodP);
          shader.setInt('Offset', offsetP);
        };
        // The state pass: one fragment per chunk boundary, into the three
        // attachments of the state buffer.
        states.bind();
        phaserStateShader.use();
        bindTexture(gl, 0, streams[current].texture);
        setLibrary(phaserStateShader, layout, g);
        phaserStateShader.setSampler('In', 0);
        phaserUniforms(phaserStateShader);
        quad.draw();
        // The main pass, continuing from its chunk's boundary state.
        {
          const target = 1 - current;
          streams[target].bind();
          phaserShader.use();
          bindTexture(gl, 0, streams[current].texture);
          for (let q = 0; q < 3; q += 1) bindTexture(gl, 1 + q, states.textures[q]);
          setLibrary(phaserShader, layout, g);
          phaserUniforms(phaserShader);
          phaserShader.setSampler('In', 0);
          phaserShader.setSampler('State0', 1);
          phaserShader.setSampler('State1', 2);
          phaserShader.setSampler('State2', 3);
          phaserShader.set('PhaserMix', Math.fround(phaserMix));
          quad.draw();
          // Unbound, as the plugin does: the state textures are the next
          // frame's render target, and a texture that is both is a feedback
          // loop WebGL refuses to draw.
          for (let q = 3; q >= 0; q -= 1) bindTexture(gl, q, null);
          current = target;
        }
      }
      if (pitchOn) {
        chain.push('pitch');
        effect(pitchShader, () => {
          pitchShader.setInt('Grain', grain);
          pitchShader.set('Ratio', Math.fround(ratio));
          pitchShader.set('PitchMix', Math.fround(pitchMix));
        });
      }

      //------------------------------------------------------------------
      // 3. Display, onto the canvas. The host's viewport is the whole canvas.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, vpW, vpH);
      displayShader.use();
      bindTexture(gl, 0, picture.texture);
      bindTexture(gl, 1, streams[current].texture);
      setLibrary(displayShader, layout, g);
      displayShader.setSampler('InputTexture', 0);
      displayShader.setSampler('Final', 1);
      displayShader.setInt('InW', width);
      displayShader.setInt('InH', height);
      displayShader.setInt('Direction', direction);
      displayShader.setInt('Format', format);
      displayShader.setInt('Wrap', overflow === K_WRAP ? 1 : 0);
      displayShader.setInt('VpX', 0);
      displayShader.setInt('VpY', 0);
      displayShader.setInt('VpW', vpW);
      displayShader.setInt('VpH', vpH);
      displayShader.set('MixAmount', Math.fround(p('mix')));
      quad.draw();
      // Unbind so nothing reads a framebuffer's own texture next frame.
      bindTexture(gl, 1, null);
      bindTexture(gl, 0, null);

      telemetry.lines = g.lines;
      telemetry.pixels = g.pixels;
      telemetry.stride = g.stride;
      telemetry.rows = g.rows;
      telemetry.total = g.total;
      telemetry.delay = delay;
      telemetry.taps = echoOn ? taps : 0;
      telemetry.window = phaserOn ? window : 0;
      telemetry.periodF = periodF;
      telemetry.periodP = periodP;
      telemetry.offsetF = offsetF;
      telemetry.offsetP = offsetP;
      telemetry.frame = frame;
      telemetry.stateRows = phaserOn ? stateRows : 0;
      telemetry.chain = chain.join(' → ') || 'nothing';
    },
  };
}

//===========================================================================
// The controls, read out of Databend::Databend(). Same names, same groups,
// same order, same defaults, same dropdown elements. Absent: the About block.
//===========================================================================

/// FF_TYPE_INTEGER is exempt from the 0..1 clamp, so the plugin stores each
/// count as the integer itself with a real range. The kit has no integer
/// control, so -- as copperlist, galvo, teletext and toner did -- each is a
/// dropdown of every value in the plugin's range; `integerValue` turns the
/// dropdown's index back into the integer.
const INTEGER_RANGES = {
  padding: [0, K_PADDING_MAX],
  delayLines: [0, K_DELAY_LINES_MAX],
  delaySamples: [0, K_DELAY_SAMPLES_MAX],
  stages: [K_STAGES_MIN, K_STAGES_MAX],
};
const INTEGER_ELEMENTS = {};
for (const [id, [low, high]] of Object.entries(INTEGER_RANGES)) {
  INTEGER_ELEMENTS[id] = [];
  for (let v = low; v <= high; v += 1) INTEGER_ELEMENTS[id].push(String(v));
}
function integerValue(id, index) {
  const [low, high] = INTEGER_RANGES[id];
  return Math.min(high, Math.max(low, low + Math.round(index)));
}
const integerIndex = (id, value) => value - INTEGER_RANGES[id][0];

const integer = (id, name, value, group, hint) => ({ id, name, type: 'option', elements: INTEGER_ELEMENTS[id], default: integerIndex(id, value), group, hint });
const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });

const hz = (v) => {
  const r = rateHz(v);
  return `${r < 1 ? r.toFixed(3) : r.toFixed(2)} Hz, period ${lfoPeriod(v)} samples`;
};

const demo = mountDemo({
  name: 'Databend',
  pluginId: 'DB01',
  tagline:
    'The raster as a PCM stream through audio effects. The picture is laid out as an audio editor would read the file — planar or interleaved, row by row, with a row stride — and real delay-line effects run on that stream: an echo comes back lines down and samples across and marches on a slant with feedback; a flanger is a comb across the scanlines; a phaser smears the picture rightwards and keeps its energy; a pitch shifter stretches the scan inside each grain. The sample format decides what happens past full scale: 8-bit wraps or clips, float clips. The shaders here are the plugin’s own; the control laws, the echo’s tap count and the phaser’s window bound are a port of its C++.',
  repo: 'https://github.com/stoatworks-labs/databend',
  page: 'https://stoatworks-labs.com/software/databend/',
  // The stock sentence says "same maths", which is only most of the truth
  // here: the shaders are the plugin's, the two integers they run on are a port.
  blurb:
    'It is Databend’s own GLSL — the stream pass, the echo, the flanger, the two-pass phaser, the pitch shifter and the display — ported from the repository to WebGL2, with the CPU half — every slider’s law, the echo’s tap count and the phaser’s restart window from the bound in Model.h, computed in double once a frame — ported to JavaScript by hand; nothing checks that port but a reader. It runs on generated clips in this page, with the plugin’s own parameters and no install.',
  // Both stream buffers are R32F and the state buffer RGBA32F, as in the
  // plugin: a stream quantised to 8 bits mid-chain would be a plausible wrong
  // picture, and a signed format's -1..1 would not fit at all.
  needFloat: true,
  params: [
    opt('layout', 'Layout', LAYOUT_NAMES, K_PLANAR, 'Stream',
      'How the file lays the samples out. Planar: all of R, then all of G, then all of B, so a delay past a plane’s start reads the previous plane. Interleaved: R, G, B per pixel, so a delay that is not a multiple of 3 lands in the wrong colour, and every effect rotates hue.'),
    integer('padding', 'Line Padding', 0, 'Stream',
      'Extra samples of silence on every line, 0 to 255: the row stride. It sets the slant an echo marches on. FF_TYPE_INTEGER in the plugin; a dropdown of the same 256 values here.'),
    opt('direction', 'Direction', DIRECTION_NAMES, K_ROWS, 'Stream',
      'What a line is: a row of the picture, or a column.'),
    opt('format', 'Format', FORMAT_NAMES, K_U8, 'Stream',
      'The sample format the file is exported to. 8-bit unsigned: zero at black, 256 codes. 16-bit signed and Float: zero at mid-grey, so silence — padding, anything before the file’s start — is grey.'),
    opt('overflow', 'Overflow', OVERFLOW_NAMES, K_CLIP, 'Stream',
      'What an integer format does past full scale: wrap round modularly, as the bytes would (the harsh databend look), or clip. A float file always clips.'),

    bool('echoOn', 'Echo On', 1, 'Echo', 'The echo: a feedback delay on the stream, e = x + m Σ gᵏ x[n − kD].'),
    integer('delayLines', 'Delay Lines', 3, 'Echo',
      'Whole lines of the stream in the delay, 0 to 32: the echo comes back this many lines down. FF_TYPE_INTEGER in the plugin; a dropdown here.'),
    integer('delaySamples', 'Delay Samples', 24, 'Echo',
      'Samples on top of the lines, 0 to 4095: the echo comes back this many samples across. In an interleaved stream a count that is not a multiple of 3 lands in the wrong colour. FF_TYPE_INTEGER in the plugin; a dropdown of all 4,096 values here.'),
    std('feedback', 'Feedback', 0.5, 'Echo', {
      display: (v) => `g ${feedbackOf(v).toFixed(4)}`,
      hint: 'g = 7/8 v: each echo is this much of the last, 7/16 at the default, 7/8 at 1, never 1, so every echo train ends. The tap count is the first K whose remainder is under one code of the Format.',
    }),
    std('echoMix', 'Echo Mix', 0.5, 'Echo', { hint: 'How much of the echo train is added to the dry stream.' }),

    bool('flangerOn', 'Flanger On', 1, 'Flanger', 'The flanger: a feed-forward delay swept by a triangle LFO in stream time, crossfaded with the dry stream.'),
    std('baseDelay', 'Base Delay', 0.0625, 'Flanger', {
      display: (v) => `${flangerSamples(v).toFixed(2)} samples`,
      hint: '64 v samples of the stream, 0 to 64; 4 at the default.',
    }),
    std('depth', 'Depth', 0.375, 'Flanger', {
      display: (v) => `${flangerSamples(v).toFixed(2)} samples`,
      hint: 'How far the LFO sweeps the delay above the base, 64 v samples, 0 to 64; 24 at the default.',
    }),
    std('rate', 'Rate', 0.5, 'Flanger', {
      display: hz,
      hint: '0.05 × 400^v Hz in stream time at the file’s nominal 44.1 kHz — a twentieth of a hertz to twenty, 1 Hz at the default — so the period in samples is 44100 over the rate. Stream time is the scan, so the delay changes line by line: a comb across the scanlines.',
    }),
    std('flangerMix', 'Flanger Mix', 0.3, 'Flanger', { hint: 'A crossfade, not a sum: the comb is deepest at a half.' }),

    bool('phaserOn', 'Phaser On', 1, 'Phaser', 'The phaser: a cascade of first-order all-pass sections in lattice form, their pole swept by a triangle LFO, crossfaded with the dry stream. Unity gain at every frequency: the picture keeps its energy.'),
    integer('stages', 'Stages', 4, 'Phaser',
      'All-pass sections in the cascade, 2 to 12. More stages, more ringing, and a longer restart window. FF_TYPE_INTEGER in the plugin; a dropdown here.'),
    std('phaserRate', 'Phaser Rate', 0.35, 'Phaser', {
      display: hz,
      hint: 'The sweep, 0.05 × 400^v Hz in stream time; 0.4 Hz at the default.',
    }),
    std('phaserDepth', 'Phaser Depth', 0.5, 'Phaser', {
      display: (v) => `pole ${poleMin(v).toFixed(3)}..${K_POLE_MAX.toFixed(1)}`,
      hint: 'How far the pole sweeps down from 0.9: to 0.9 (1 − v). At 0 the pole sits still at 0.9; at 1 it sweeps to 0.',
    }),
    std('phaserMix', 'Phaser Mix', 0.35, 'Phaser', { hint: 'A crossfade: at 1 the picture is pure all-pass and keeps its energy exactly.' }),

    bool('pitchOn', 'Pitch On', 0, 'Pitch', 'The pitch shifter: two taps on a sawtooth delay of period Grain, half a grain apart, crossfaded by complementary triangles. Inside a grain the scan is read at the Ratio; at the seam it repeats or skips.'),
    std('ratio', 'Ratio', 0.25, 'Pitch', {
      display: (v) => `× ${ratioOf(v).toFixed(4)}`,
      hint: '2^( 2 v − 1 ): half at 0, unity at 0.5, double at 1. 1/√2 at the default.',
    }),
    std('grain', 'Grain', 0.5, 'Pitch', {
      display: (v) => `${grainOf(v)} samples`,
      hint: 'round( 64 × 2^( 6 v ) ) samples, made even: 64 to 4096, 512 at the default.',
    }),
    std('pitchMix', 'Pitch Mix', 1.0, 'Pitch', { hint: 'The shifted stream against the unshifted one.' }),

    std('mix', 'Mix', 1.0, 'Output', {
      hint: 'The exported picture against the input. At 1 the export is shown as it is, never a mix’s rounding of itself.',
    }),
  ],
  // Bars show the echo's slant and the wrong-colour echo as flat fields; the
  // scene is the general case; the grid reads the flanger's comb and the
  // pitch shifter's seams; the ramp shows what wrapping past full scale does.
  sources: ['bars', 'scene', 'grid', 'ramp', 'detail', 'spot'],
  // The plugin ships no factory presets. These are the page's own, expressed
  // entirely in the plugin's parameters and reachable with the controls.
  presets: {
    'The wrong colour (interleaved)': { layout: K_INTERLEAVED },
    'Wrap past full scale': { overflow: K_WRAP },
    'Signed 16-bit': { format: K_S16 },
    'Columns': { direction: K_COLUMNS },
    'Five samples of padding': { padding: integerIndex('padding', 5) },
    'Echo only, marching': { flangerOn: 0, phaserOn: 0, delayLines: integerIndex('delayLines', 1), delaySamples: integerIndex('delaySamples', 7), feedback: 0.9, echoMix: 1.0 },
    'Flanger only': { echoOn: 0, phaserOn: 0, flangerMix: 0.5, depth: 1.0 },
    'Phaser only, pure all-pass': { echoOn: 0, flangerOn: 0, phaserMix: 1.0, stages: integerIndex('stages', 8) },
    'Pitch shifter, an octave down': { echoOn: 0, flangerOn: 0, phaserOn: 0, pitchOn: 1, ratio: 0.0 },
    'Interleaved, 8-bit, wrapping': { layout: K_INTERLEAVED, overflow: K_WRAP, delayLines: integerIndex('delayLines', 4), delaySamples: integerIndex('delaySamples', 61), feedback: 0.6 },
  },
  differences: [
    'The CPU half of this plugin is a PORT, not the plugin’s own code. Databend converts every slider by its own law in Controls.cpp and computes two integers once a frame — the echo’s tap count, the first K whose remaining echo energy is under one code of the Format, and the phaser’s restart window, a rigorous worst-case bound iterated in double until the output error stays under half a code for a whole chunk (Model.h, PhaserWindow) — plus the stream’s geometry, the delay in samples, the echo’s input magnitude and the LFOs’ frame offsets in ProcessOpenGL. All of that is ported here function for function, in JavaScript doubles, rounded to float where the plugin hands a float uniform over. Nothing checks a port but a reader; the repository’s dbtest --laws and --window check the C++ against the model’s statement and have never heard of this page.',
    'The GPU half is not a port. The stream pass, the echo, the flanger, the phaser’s state and main passes, the pitch shifter and the display are the plugin’s own GLSL, assembled as the plugin assembles them, and demo/tools/check_shaders.py fails the repository’s verify script if a character of any of the ten bodies drifts.',
    'The buffers are the plugin’s: two R32F stream buffers of Stride × Rows samples, ping-ponged between the effects, and a three-attachment RGBA32F state buffer holding the phaser cascade’s twelve states at every 16-sample chunk boundary, every one Nearest and read by texelFetch. WebGL2 renders into float textures only with EXT_color_buffer_float; the page refuses to start without it rather than fall back to 8 bits. A stream buffer wider or taller than this GPU’s largest texture — an interleaved 1080p file is 5,760 samples wide — is refused with a message rather than clipped.',
    'Line Padding, Delay Lines, Delay Samples and Stages are FF_TYPE_INTEGER in the plugin, with real ranges. The kit has no integer control, so each is a dropdown of the same values (Delay Samples has 4,096 entries).',
    'The plugin stores each host value as a float; the page’s sliders are doubles, so every value is rounded through Math.fround before its law is applied, and the defaults are the plugin’s float defaults.',
    'The plugin has no clock (SetTimeSupported( false )); the LFOs’ phase carries across frames on a frame counter, and here that counter is the page’s frame index: Pause holds it, Step advances it by one, Restart sends it to 0. It wraps at 100,000 frames where the plugin’s does not, so the LFOs’ phase jumps once about 28 minutes in at 60 fps.',
    'The harness-only Perturb uniform is set to what the shipped plugin sets it to, 0. The seven negative controls dbtest drives through it are not on this page.',
    'There is no audio caveat on this page: Databend has no audio path, and no sound is involved anywhere — the “audio effects” run on the picture’s samples. The About block is absent, as on every page in this suite.',
    'Nothing here is real-time-rated. The plugin’s render cost is measured in the repository (the phaser’s window is most of it, and 4K is not real time with the phaser on); a browser’s cost on this machine is whatever it is, and the state pass at a large composition may be slow.',
    'Measured once against the real plugin, 2026-09-24: on the same 960 × 540 colour-bars frame this page and dbtest --pipe agreed on every pixel exactly with the echo, the export and three layouts (Planar; Interleaved with Wrap; 16-bit signed, Columns, five samples of padding), and within 1/255 with the pitch shifter on — SwiftShader against Metal GL, with the flanger and the phaser off because their LFOs’ phase rides on a frame counter the page and the pipe do not share. One frame, four settings, one machine; not a proof of the page.',
    'The plugin’s proof — an echo of a line + k samples landing a line down and k across with zero error against the closed form, the nth echo at n·(k, 1) with amplitude m gⁿ, the wrong-colour echo in an interleaved stream, the flanger’s ghost at the LFO’s value on every line, the phaser held to a serial double cascade within a bound it proves, the formats’ wrap and clip, the pitch shifter’s period, and seven negative controls — is an offline harness in the repository, at two rasters. Nothing on this page measures anything; the line under the canvas reports what the port computed.',
  ],
  createRenderer,
});

//---------------------------------------------------------------------------
// The line under the canvas. It reports the ported half's own numbers: the
// stream's geometry, the delay in samples, the echo's tap count, the phaser's
// window, the LFO periods and this frame's phase offsets. Skipped in embed
// mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    stage.append(line);
    setInterval(() => {
      const t = telemetry;
      if (!t.total) return;
      line.textContent =
        `Stream: ${t.lines} lines × ${t.stride} samples (${t.pixels} pixels + padding), ${t.rows} rows, ${t.total.toLocaleString('en-GB')} samples; chain ${t.chain}. `
        + `Echo delay ${t.delay} samples, ${t.taps} tap${t.taps === 1 ? '' : 's'}; `
        + `phaser window ${t.window} samples over ${t.stateRows} state row${t.stateRows === 1 ? '' : 's'}; `
        + `LFO periods ${t.periodF} / ${t.periodP} samples, frame ${t.frame} offsets ${t.offsetF} / ${t.offsetP}.`;
    }, 250);
  }
}
