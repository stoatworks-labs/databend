#pragma once

#include <FFGLSDK.h>

namespace databendfx
{
/**
	A framebuffer with THREE colour attachments, for the phaser's state pass.

	The cascade's state at a chunk boundary is up to twelve numbers (one per
	stage) and one RGBA texel holds four. The SDK's FFGLFBO has one
	attachment, so this is its own small class: three RGBA32F textures on
	one framebuffer with glDrawBuffers set for all, Nearest filtering and
	clamp-to-edge, reallocated only when the size changes and cleared when
	it is. Boundary b lives at texel ( b mod width, b / width ) of each.

	Same rules as PassBuffer: Ensure() before anything binds a texture (it
	binds and unbinds textures itself), and Destroy() in DeInitGL. Copied
	from slope's two-attachment StateBuffer, one attachment wider.
*/
class StateBuffer
{
public:
	bool Ensure( GLsizei width, GLsizei height );
	void Destroy();

	GLuint FBO() const
	{
		return fbo;
	}
	/// Stages 4q .. 4q + 3.
	GLuint Texture( int q ) const
	{
		return textures[ q ];
	}
	GLsizei Width() const
	{
		return width;
	}
	GLsizei Height() const
	{
		return height;
	}

private:
	GLuint fbo           = 0;
	GLuint textures[ 3 ] = { 0, 0, 0 };
	GLsizei width        = 0;
	GLsizei height       = 0;
};

} // namespace databendfx
