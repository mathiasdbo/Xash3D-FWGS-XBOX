/*
GL/gl.h - just the two constants this SDK's own CFrustum.cpp wants, for nxdk

Part of compat/xbox; see that directory's README.

Xbox has no real OpenGL, and no header providing one - but
deps/hlsdk/cl_dll/particleman/CFrustum.cpp's own header comment says
exactly why it includes this: "The triangle API uses OpenGL constants for
certain functions." Confirmed directly (grep across the whole file): it
never calls a single real GL function - only passes GL_PROJECTION_MATRIX/
GL_MODELVIEW_MATRIX as plain `int` values to the ENGINE's own
gEngfuncs.pTriAPI->GetMatrix(const int pname, float* matrix)
(deps/hlsdk/common/triangleapi.h:53), which is this SDK's own real,
already-working abstraction over "get me a transform matrix" - not real
GL at all. So a full GL/gl.h (or any real rendering capability) is not
what this file needs; these two symbolic constants are.

Values verified against this repo's own already-vendored, authoritative
sources - not guessed: deps/hlsdk/external/SDL2/SDL_opengl.h and this
project's own real reference renderer, ref/gl/gl_export.h, both agree.
*/
#ifndef R3D_COMPAT_GL_GL_H
#define R3D_COMPAT_GL_GL_H

#ifndef GL_MODELVIEW_MATRIX
#define GL_MODELVIEW_MATRIX 0x0BA6
#endif

#ifndef GL_PROJECTION_MATRIX
#define GL_PROJECTION_MATRIX 0x0BA7
#endif

#endif
