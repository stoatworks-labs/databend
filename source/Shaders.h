#pragma once

#include <string>

/**
	The passes. Every read is `texelFetch` at integer coordinates computed
	in integers, so nothing here depends on a texture unit's filtering or
	on where a rasteriser's interpolated uv lands. Every coefficient is
	computed on the CPU in double (`Controls.cpp`, `Model.h`) and handed
	over as a float or int uniform; the GPU multiplies, adds and compares.

	The stream buffers are R32F, Stride texels wide (L: samples per line,
	padding included) and Rows tall (lines for Interleaved, 3 x lines for
	Planar), and texel ( col, row ) holds stream sample n = row Stride +
	col. Only the stream pass, which reads the host's picture, and the
	display, which writes it, know that GL's row 0 is the bottom.

	  stream    host picture  -> the file: one R32F texel per sample, in the
	                             signal domain of the Format (0 at black or
	                             at mid-grey), padding as silence
	  echo      stream        -> x + m_e sum_k g^k x[n - kD], K taps
	  flanger   stream        -> e + m_f e[n - d(n)], two taps interpolated
	  phaser    stream        -> the all-pass cascade's state at every chunk
	  (state)                    boundary, a windowed restart of K samples
	                             before it (one fragment per kChunk samples)
	  phaser    stream        -> the cascade continued from the boundary's
	           + the states      state, at most kChunk steps
	  pitch     stream        -> two crossfaded taps on a sawtooth delay
	  display   stream        -> quantised to the Format, overflowed as it
	           + host picture   overflows, back to the pixel, and the mix

	The six stream passes share `kStreamLibrary` -- the geometry uniforms,
	`fetch()`, the LFO -- which is a fragment (no #version, no main) that
	`Assemble()` pastes under the header. `dbtest --dump-shaders` writes
	exactly what Assemble() returns, so what glslc compiles and what the
	harness measures is what the plugin hands the driver.
*/
namespace databendfx::shaders
{

extern const char* const kVertex;

/// The full source of each fragment shader, assembled.
std::string Stream();
std::string Echo();
std::string Flanger();
std::string PhaserState();
std::string Phaser();
std::string Pitch();
std::string Display();

} // namespace databendfx::shaders
