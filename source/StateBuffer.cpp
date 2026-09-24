#include "StateBuffer.h"

namespace databendfx
{

bool StateBuffer::Ensure( GLsizei requestedWidth, GLsizei requestedHeight )
{
	if( requestedWidth <= 0 || requestedHeight <= 0 )
		return false;
	if( fbo != 0 && width == requestedWidth && height == requestedHeight )
		return true;

	Destroy();

	GLint previousFBO = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previousFBO );

	glGenTextures( 3, textures );
	for( GLuint texture : textures )
	{
		glBindTexture( GL_TEXTURE_2D, texture );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, requestedWidth, requestedHeight, 0, GL_RGBA, GL_FLOAT, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	}
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	const GLenum buffers[ 3 ] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
	for( int q = 0; q < 3; ++q )
		glFramebufferTexture2D( GL_FRAMEBUFFER, buffers[ q ], GL_TEXTURE_2D, textures[ q ], 0 );
	glDrawBuffers( 3, buffers );
	const bool complete = glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
	if( complete )
	{
		//Cleared: a state buffer whose contents are undefined is whatever
		//the driver handed back.
		GLint previousViewport[ 4 ] = { 0, 0, 0, 0 };
		glGetIntegerv( GL_VIEWPORT, previousViewport );
		glViewport( 0, 0, requestedWidth, requestedHeight );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		glViewport( previousViewport[ 0 ], previousViewport[ 1 ], previousViewport[ 2 ], previousViewport[ 3 ] );
	}
	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( previousFBO ) );

	if( !complete )
	{
		Destroy();
		return false;
	}
	width  = requestedWidth;
	height = requestedHeight;
	return true;
}

void StateBuffer::Destroy()
{
	if( fbo != 0 )
		glDeleteFramebuffers( 1, &fbo );
	if( textures[ 0 ] != 0 || textures[ 1 ] != 0 || textures[ 2 ] != 0 )
		glDeleteTextures( 3, textures );
	fbo = 0;
	textures[ 0 ] = textures[ 1 ] = textures[ 2 ] = 0;
	width = height = 0;
}

} // namespace databendfx
