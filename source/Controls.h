#pragma once

/**
	What a host parameter means.

	Every ranged host parameter is 0..1 (SetParamInfo clamps a STANDARD
	default into 0..1 before a range can be attached), and an option
	parameter's range reads back 0..1 whatever its element count -- so an
	option is its element INDEX, rounded and clamped here, never a fraction
	of a range. The integer counts (Line Padding, Delay Lines, Delay
	Samples, Stages) are real FF_TYPE_INTEGERs with real ranges. Every
	conversion to a physical unit lives here and nowhere else; dbtest
	states the same laws from their definitions and --laws holds the two
	together.

	The powers of two are deliberate: a slider at 0, 0.5 or 1 gives a
	feedback, a ratio or a grain that is exact in float, so the harness can
	choose settings in which an echo sum or a resampled bar pattern is
	exact arithmetic.
*/
namespace databendfx::controls
{

/// An option's stored value, as an index into its `count` elements.
int OptionIndex( float value, int count );

/// An integer parameter, clamped to its range.
int Integer( float value, int low, int high );

/// Feedback: g = 7/8 v -- 7/16 at the default 0.5, 7/8 at 1, never 1, so
/// every echo train ends. Dyadic at every dyadic slider.
double Feedback( float value );

/// The three effect mixes and the output mix are the slider itself.
double Mix( float value );

/// Base Delay and Depth: 64 v samples of the stream, 0..64.
double FlangerSamples( float value );

/// Rate and Phaser Rate: 0.05 x 400^v Hz, one twentieth of a hertz to
/// twenty, 1 Hz at the default 0.5.
double RateHz( float value );

/// An LFO's period in samples, round( 44100 / rate ), at least 2.
int LfoPeriod( float value );

/// Phaser Depth: the sweep's bottom pole, kPoleMax ( 1 - v ): at 0 the
/// pole sits at kPoleMax, at 1 it sweeps down to 0.
double PoleMin( float value );

/// The largest s = sqrt( 1 - k^2 ) the sweep reaches, from its smallest
/// pole: 1 when the sweep touches 0.
double SMax( float value );

/// Ratio: 2^( 2 v - 1 ), half to double, unity at 0.5.
double Ratio( float value );

/// Grain: round( 64 x 2^( 6 v ) ) samples made even, 64 to 4096, 512 at 0.5.
int Grain( float value );

/// 2^e, exact for an integral e on every libm (ldexp), so the dyadic
/// settings above are dyadic in fact and not only in intent.
double Pow2( double e );

} // namespace databendfx::controls
