#ifndef UA_GLAPI_H_INCLUDED
#define UA_GLAPI_H_INCLUDED

// The one place that pulls in the GL API for the build flavour.
//
//  * Desktop (default): OpenGL 4.1 core (or 2.1 with gfx.vbo=false) through
//    SDL_opengl.h + glext.h. Unchanged from before this header existed.
//  * UA_GLES (Quest/Android, and the Mac ANGLE test build): OpenGL ES 3.x
//    through GLES3/gl32.h + gl2ext.h. Core entry points are linked directly
//    (libGLESv3 on Android, ANGLE's libGLESv2 on the Mac); the fixed-function
//    paths of the engine are compiled out (see UA_GLES in gfx.cpp).
//
// UA_GL_GetProcAddress: Android has no SDL video subsystem (the OpenXR shell
// owns EGL), so lookups go to eglGetProcAddress there; everywhere else they go
// through SDL as before.

#include <SDL2/SDL.h>

#ifdef UA_GLES

#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

// Desktop-only entry points the loader tables name. They stay NULL on GLES and
// are never called.
typedef void *(GL_APIENTRYP PFNGLMAPBUFFERPROC) (GLenum target, GLenum access);
typedef void (GL_APIENTRYP PFNGLGETBUFFERSUBDATAPROC) (GLenum target, GLintptr offset, GLsizeiptr size, void *data);
typedef void (GL_APIENTRYP PFNGLGETQUERYOBJECTUI64VPROC) (GLuint id, GLenum pname, GLuint64 *params);
typedef void (GL_APIENTRYP PFNGLGETQUERYOBJECTIVPROC) (GLuint id, GLenum pname, GLint *params);

#ifndef APIENTRY
#define APIENTRY GL_APIENTRY
#endif

// Enums used by desktop-only code that still has to compile.
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED 0x88BF            // GL_TIME_ELAPSED_EXT (EXT_disjoint_timer_query)
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

#if defined(__ANDROID__)
#include <EGL/egl.h>
inline void *UA_GL_GetProcAddress(const char *name) { return (void *)eglGetProcAddress(name); }
#else
inline void *UA_GL_GetProcAddress(const char *name) { return SDL_GL_GetProcAddress(name); }
#endif

#else // desktop GL

// Same order the GL users had before: SDL_opengl.h first (it pulls in gl.h
// and SDL's glext with the PFN typedefs), then the platform glext.h.
#include <SDL2/SDL_opengl.h>
#if defined(__APPLE__) && defined(__MACH__)
#include <OpenGL/glext.h>
#else
#include <GL/glext.h>
#endif

inline void *UA_GL_GetProcAddress(const char *name) { return SDL_GL_GetProcAddress(name); }

#endif // UA_GLES

#endif // UA_GLAPI_H_INCLUDED
