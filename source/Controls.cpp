#include "Controls.h"

#include "Model.h"

#include <algorithm>
#include <cmath>

namespace databendfx::controls
{
namespace
{
double unit( float value )
{
	return std::clamp( static_cast< double >( value ), 0.0, 1.0 );
}
} // namespace

double Pow2( double e )
{
	const double whole = std::floor( e );
	return std::ldexp( std::exp2( e - whole ), static_cast< int >( whole ) );
}

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

int Integer( float value, int low, int high )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), low, high );
}

double Feedback( float value )
{
	return 0.875 * unit( value );
}

double Mix( float value )
{
	return unit( value );
}

double FlangerSamples( float value )
{
	return 64.0 * unit( value );
}

double RateHz( float value )
{
	return 0.05 * std::pow( 400.0, unit( value ) );
}

int LfoPeriod( float value )
{
	return std::max( 2, static_cast< int >( std::lround( model::kSampleRate / RateHz( value ) ) ) );
}

double PoleMin( float value )
{
	return model::kPoleMax * ( 1.0 - unit( value ) );
}

double SMax( float value )
{
	const double k = PoleMin( value );
	return std::sqrt( 1.0 - k * k );
}

double Ratio( float value )
{
	return Pow2( 2.0 * unit( value ) - 1.0 );
}

int Grain( float value )
{
	const int g = static_cast< int >( std::lround( 64.0 * Pow2( 6.0 * unit( value ) ) ) );
	return std::clamp( g + ( g & 1 ), 64, 4096 );
}

} // namespace databendfx::controls
