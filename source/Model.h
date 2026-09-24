#pragma once

#include <cstdint>

/**
	The stream and the effects as arithmetic: what the shaders compute,
	written down once, with no GL in it. The plugin's C++ uses only the
	constants, the option tables and the window bound; the effects themselves
	run in GLSL (`Shaders.cpp`) and the harness restates them in double from
	this description.

	**The one idea.** Databending is opening a picture file in an audio
	editor and running audio effects on it. The editor sees one long stream
	of samples in the order the file lays them out -- row-major, one plane
	after another (Planar) or R, G, B per pixel (Interleaved), with a row
	stride that may carry padding -- and a delay-line effect on that stream
	does not know where the lines are. So:

	    an echo of D = k L + m samples      comes back k lines down and m
	                                         samples across: with feedback the
	                                         echoes march down the picture on
	                                         a slant, and an m that is not a
	                                         multiple of 3 in an interleaved
	                                         stream lands in the wrong colour
	    a flanger, a delay swept by an LFO   is a comb whose tooth spacing
	    in stream time                       changes line by line, because
	                                         stream time IS the scan
	    a phaser, all-pass stages swept      shifts the phase of every
	    by an LFO                            frequency and the gain of none:
	                                         the picture smears rightwards
	                                         with ringing and keeps its energy
	    a pitch shifter, grains resampled    stretches the scan inside each
	    and crossfaded                       grain and repeats or skips at the
	                                         seams
	    the file's sample format             decides what happens past full
	                                         scale: an 8-bit unsigned file
	                                         wraps (the harsh look) or clips;
	                                         a float file clips

	**The stream.** lines x samples. With Lp pixels on a line and p samples
	of padding, a Planar stream has L = Lp + p samples per line and three
	planes of `lines` lines each, plane-major: n = c lines L + l L + x. An
	Interleaved stream has L = 3 Lp + p: n = l L + 3 x + c. Padding samples
	are silence (0); so is anything before the file's start. The Direction
	chooses whether a line is a row of the picture or a column. The signal
	domain is the file's: an unsigned format's zero is black and the values
	run 0..1; a signed format's zero is mid-grey and the values run -1..1.
	The effects run in the editor's float domain; the format quantises and
	overflows ONCE, on export.

	**The effects**, in a fixed chain, each with a bypass, x -> e -> f -> q -> t:

	    echo      e[n] = x[n] + m_e ( y[n] - x[n] ),  y[n] = x[n] + g y[n - D]
	              so the k-th echo has amplitude m_e g^k. D = dl L + ds.
	    flanger   f[n] = ( 1 - m_f ) e[n] + m_f e[n - d(n)],  d(n) = b + w tri( phi(n) )
	              feed-forward, a crossfade (the comb is deepest at m_f =
	              1/2); a fractional d reads a linear interpolation of the
	              two neighbouring samples.
	    phaser    q[n] = ( 1 - m_p ) f[n] + m_p A_S( f )[n], A_S a cascade of
	              S first-order all-pass sections with pole k(n) = kappa ( 1 -
	              depth tri( phi(n) ) ), each in the normalised lattice form
	                  y = -k x + s w_prev,  w = s x + k w_prev,  s = sqrt( 1 - k^2 )
	              which is a rotation: lossless per sample, whatever k does.
	              ( 1 - m_p ) x + m_p A( x ) rather than x + A( x ), because an
	              all-pass has unity DC gain and a picture is mostly DC.
	    pitch     t[n] = ( 1 - m_t ) q[n] + m_t ( w_A q[n - d_A] + w_B q[n - d_B] )
	              two taps on a sawtooth delay of period G (the grain) and
	              slope 1 - r, half a grain apart, with complementary
	              triangular crossfades: inside a grain the scan is read at
	              rate r, at the seam it jumps back by ( 1 - r ) G.

	tri( phi ) = 1 - | 2 phi - 1 |, phi(n) = ( ( n + off ) mod P ) / P, where
	P = round( 44100 / rate ) samples: the file's nominal sample rate is
	44.1 kHz, so a rate in Hz is a period in samples of the scan. `off`
	carries the phase across frames as if the clip were one file (the delay
	memories do not: each frame's delay lines start empty). A triangle
	rather than a sine so the LFO's value is exact in float, which is what
	lets the flanger check derive its tolerance rather than fit it.

	**How the phaser runs on a GPU with no image stores.** An IIR filter
	over a stream of millions of samples cannot be run serially, and slope's
	chunked-exact form (one draw per 32 samples) would be N / 32 serial draws
	over the whole frame. What CAN be done is a windowed restart: each
	fragment re-runs the cascade from zero state K samples before its own,
	and the error that leaves it is the response to a wrong start state.
	That is sound here and was not in slope, because the system is LINEAR:
	the error is the homogeneous response and does not depend on the
	signal, and each lattice stage contracts its own state error by |k| <=
	kappa per sample whatever the input does. PhaserWindow() iterates the
	rigorous worst-case bound (every |k| at kappa, every s at its maximum,
	the triangle inequality across the cascade) from a bound on the true
	states at the restart point -- min( X / ( 1 - kappa ) chained through
	the stages, X sqrt N from losslessness: the state energy can never
	exceed the input energy so far ) -- until the output error falls under
	half a code of the format. Feedback around the cascade would break that
	proof (a swept-coefficient loop has no bound of this kind) and is not
	offered; see AGENTS.md.

	**The echo** is the finite sum sum_{k=1..K} g^k x[n - kD], K the first
	count at which the remainder g^{K+1} / ( 1 - g ) of a unit input is under
	one code of the format. Nothing else has memory: the flanger and the
	pitch shifter are taps, and the format is a function of one sample.
*/
namespace databendfx::model
{

/// Negative-control hooks: a bitmask the shipped plugin always carries at 0.
/// Each one perturbs the PLUGIN's shaders, never the harness's expectation.
enum Perturb : int
{
	kPerturbNone         = 0,
	kPerturbLayoutPlanar = 1 << 0,///< the stream is laid out Planar whatever Layout says (the spec's --interleave negative)
	kPerturbNoPadding    = 1 << 1,///< the line stride ignores Line Padding
	kPerturbEchoFlatGain = 1 << 2,///< every echo at gain g instead of g^k
	kPerturbLfoFrozen    = 1 << 3,///< both LFOs held at phase 0
	kPerturbShortWindow  = 1 << 4,///< the phaser's restart window cut to a quarter of its bound (the spec's --allpass negative)
	kPerturbNoWrap       = 1 << 5,///< an integer format clips even when Overflow says Wrap
	kPerturbPitchInverse = 1 << 6,///< the pitch shifter reads at 1 / r
};

/// The file's nominal sample rate: what turns an LFO rate in Hz into a
/// period in samples of the scan.
constexpr double kSampleRate = 44100.0;

/// The largest pole the phaser's sweep reaches: the top of the sweep range,
/// and kappa in the window bound.
constexpr double kPoleMax = 0.9;

/// The restart window is never longer than this; PhaserWindow() returns
/// the bound's K, and --window asserts it is under this over the whole
/// control range (so no setting ships with an unproved window).
constexpr int kMaxWindow = 1024;

/// The phaser runs in two passes: a state pass re-runs the K-sample window
/// once per chunk boundary (one fragment per kChunk samples of the
/// stream) and writes the cascade's state there; the main pass continues
/// from that state for at most kChunk steps. The error at a sample is the
/// boundary's state error propagated j <= kChunk steps, which is the
/// restart bound at K + j: so K is the first window after which the bound
/// stays under half a code for kChunk more steps. Cost per sample: K /
/// kChunk + kChunk steps instead of K.
constexpr int kChunk = 16;

/// The state buffer's width: boundary b is texel ( b mod kStateWidth,
/// b / kStateWidth ).
constexpr int kStateWidth = 4096;

/// A float file has no code; for the truncation and restart bounds one
/// 16-bit code is taken.
constexpr double kFloatCode = 1.0 / 65536.0;

enum Layout
{
	kPlanar      = 0,
	kInterleaved = 1,
	kLayoutCount
};
inline const char* const kLayoutNames[ kLayoutCount ] = { "Planar", "Interleaved" };

enum Direction
{
	kRows    = 0,
	kColumns = 1,
	kDirectionCount
};
inline const char* const kDirectionNames[ kDirectionCount ] = { "Rows", "Columns" };

enum Format
{
	kU8    = 0,///< 8-bit unsigned: zero at black, 256 codes, wraps or clips
	kS16   = 1,///< 16-bit signed: zero at mid-grey, 65536 codes, wraps or clips
	kFloat = 2,///< float: zero at mid-grey, no code, clips
	kFormatCount
};
inline const char* const kFormatNames[ kFormatCount ] = { "8-bit unsigned", "16-bit signed", "Float" };

enum Overflow
{
	kWrap = 0,
	kClip = 1,
	kOverflowCount
};
inline const char* const kOverflowNames[ kOverflowCount ] = { "Wrap", "Clip" };

/// Whether a format's zero is mid-grey (signed) rather than black.
inline bool IsSigned( int format )
{
	return format != kU8;
}

/// One code of a format in SIGNAL units (the 16-bit code is 1/32768 of a
/// signed unit; the float file's is taken as one 16-bit code of the
/// picture).
inline double CodeOf( int format )
{
	switch( format )
	{
	case kU8: return 1.0 / 255.0;
	case kS16: return 1.0 / 32768.0;
	default: return kFloatCode;
	}
}

/// Integer parameter ranges.
constexpr int kPaddingMax     = 255;
constexpr int kDelayLinesMax  = 32;
constexpr int kDelaySamplesMax = 4095;
constexpr int kStagesMin      = 2;
constexpr int kStagesMax      = 12;

/// The stream's geometry for a picture of `width` x `height`.
struct Geometry
{
	int lines   = 0;///< scan lines: rows, or columns
	int pixels  = 0;///< pixels per line
	int stride  = 0;///< L: samples per line, padding included
	int rows    = 0;///< texture rows of the stream buffer: lines, or 3 x lines for Planar
	int64_t total = 0;///< samples in the whole stream
};

inline Geometry GeometryOf( int width, int height, int layout, int direction, int padding )
{
	Geometry g;
	g.lines  = direction == kColumns ? width : height;
	g.pixels = direction == kColumns ? height : width;
	g.stride = ( layout == kInterleaved ? 3 * g.pixels : g.pixels ) + padding;
	g.rows   = layout == kInterleaved ? g.lines : 3 * g.lines;
	g.total  = static_cast< int64_t >( g.stride ) * g.rows;
	return g;
}

/// Stream index of pixel ( line l, position x along the line ), channel c.
inline int64_t StreamIndex( const Geometry& g, int layout, int l, int x, int c )
{
	if( layout == kInterleaved )
		return static_cast< int64_t >( l ) * g.stride + 3 * x + c;
	return ( static_cast< int64_t >( c ) * g.lines + l ) * g.stride + x;
}

/// The echo's tap count: the first K at which the remainder of a unit
/// input's echoes, g^( K + 1 ) / ( 1 - g ), is at or under one code. 0 for
/// no feedback.
inline int EchoTaps( double g, double code )
{
	if( g <= 0.0 )
		return 0;
	int K = 0;
	double remainder = g / ( 1.0 - g );
	while( remainder > code )
	{
		remainder *= g;
		++K;
	}
	return K;
}

/// The phaser's restart window: the rigorous worst-case bound iterated
/// until the output error is under half a code and stays there for
/// `chunk` more steps (the main pass's continuation). `stages` sections,
/// every pole at most `kappa` in magnitude and every s at most `smax`, an
/// input of magnitude at most `x` over a stream of `total` samples, the
/// result scaled by the phaser's mix. Returns kMaxWindow if the bound
/// never gets there (it does, over the whole control range: --window).
inline int PhaserWindow( int stages, double kappa, double smax, double x, double total, double code, double mix, int chunk )
{
	if( stages <= 0 || kappa <= 0.0 || mix <= 0.0 )
		return 1;
	double W[ kStagesMax ] = {};
	double xi = x;
	//sqrt in double, from <cmath>, would drag the header in; Newton's
	//method converges in a dozen steps and the bound is not sharper than
	//that.
	double root = total > 1.0 ? total : 1.0;
	for( int i = 0; i < 60; ++i )
		root = 0.5 * ( root + total / root );
	const double energy = x * root;
	for( int i = 0; i < stages; ++i )
	{
		double wi = xi / ( 1.0 - kappa );
		if( wi > energy )
			wi = energy;
		W[ i ] = wi;
		xi     = kappa * xi + smax * wi;
	}
	double ew[ kStagesMax ];
	for( int i = 0; i < stages; ++i )
		ew[ i ] = W[ i ];
	//After t steps eyPrev is the bound on the output error at step t. The
	//window K is the first for which steps K + 1 .. K + chunk are all under
	//half a code: the start of the first streak of `chunk` such steps, less
	//one.
	int streakStart = -1;
	for( int t = 1; t <= kMaxWindow + chunk; ++t )
	{
		double eyPrev = 0.0;
		for( int i = 0; i < stages; ++i )
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
	return kMaxWindow;
}

} // namespace databendfx::model
