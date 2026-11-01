// GL entry points for the VR module (see vr_gl.h). Shared by every backend.
#include <stdlib.h>
#include <string.h>
#include "vr_gl.h"

namespace VR
{

// ---------------------------------------------------------------------------
GLFuncs gl;

#define VRGL_LOAD(field, name) ok &= ((field = (decltype(field))UA_GL_GetProcAddress(name)) != NULL)

bool GLFuncs::Load()
{
    bool ok = true;
    VRGL_LOAD(GenFramebuffers, "glGenFramebuffers");
    VRGL_LOAD(DeleteFramebuffers, "glDeleteFramebuffers");
    VRGL_LOAD(BindFramebuffer, "glBindFramebuffer");
    VRGL_LOAD(FramebufferTexture2D, "glFramebufferTexture2D");
    VRGL_LOAD(FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    VRGL_LOAD(CheckFramebufferStatus, "glCheckFramebufferStatus");
    VRGL_LOAD(GenRenderbuffers, "glGenRenderbuffers");
    VRGL_LOAD(DeleteRenderbuffers, "glDeleteRenderbuffers");
    VRGL_LOAD(BindRenderbuffer, "glBindRenderbuffer");
    VRGL_LOAD(RenderbufferStorage, "glRenderbufferStorage");
    VRGL_LOAD(RenderbufferStorageMultisample, "glRenderbufferStorageMultisample");
    VRGL_LOAD(BlitFramebuffer, "glBlitFramebuffer");
    VRGL_LOAD(BlendFuncSeparate, "glBlendFuncSeparate");
    VRGL_LOAD(ActiveTexture, "glActiveTexture");
    VRGL_LOAD(CreateShader, "glCreateShader");
    VRGL_LOAD(ShaderSource, "glShaderSource");
    VRGL_LOAD(CompileShader, "glCompileShader");
    VRGL_LOAD(GetShaderiv, "glGetShaderiv");
    VRGL_LOAD(GetShaderInfoLog, "glGetShaderInfoLog");
    VRGL_LOAD(DeleteShader, "glDeleteShader");
    VRGL_LOAD(CreateProgram, "glCreateProgram");
    VRGL_LOAD(AttachShader, "glAttachShader");
    VRGL_LOAD(BindAttribLocation, "glBindAttribLocation");
    VRGL_LOAD(LinkProgram, "glLinkProgram");
    VRGL_LOAD(GetProgramiv, "glGetProgramiv");
    VRGL_LOAD(DeleteProgram, "glDeleteProgram");
    VRGL_LOAD(UseProgram, "glUseProgram");
    VRGL_LOAD(GetUniformLocation, "glGetUniformLocation");
    VRGL_LOAD(Uniform1i, "glUniform1i");
    VRGL_LOAD(Uniform4f, "glUniform4f");
    VRGL_LOAD(UniformMatrix4fv, "glUniformMatrix4fv");
    VRGL_LOAD(GenVertexArrays, "glGenVertexArrays");
    VRGL_LOAD(BindVertexArray, "glBindVertexArray");
    VRGL_LOAD(GenBuffers, "glGenBuffers");
    VRGL_LOAD(BindBuffer, "glBindBuffer");
    VRGL_LOAD(BufferData, "glBufferData");
    VRGL_LOAD(EnableVertexAttribArray, "glEnableVertexAttribArray");
    VRGL_LOAD(VertexAttribPointer, "glVertexAttribPointer");

    bool tq = true;
#ifdef UA_GLES
    // GL_TIME_ELAPSED and the 64-bit result come from EXT_disjoint_timer_query on GLES.
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    tq = ext && strstr(ext, "GL_EXT_disjoint_timer_query");
    tq &= ((GenQueries = (PFNGLGENQUERIESPROC)UA_GL_GetProcAddress("glGenQueriesEXT")) != NULL);
    tq &= ((DeleteQueries = (PFNGLDELETEQUERIESPROC)UA_GL_GetProcAddress("glDeleteQueriesEXT")) != NULL);
    tq &= ((BeginQuery = (PFNGLBEGINQUERYPROC)UA_GL_GetProcAddress("glBeginQueryEXT")) != NULL);
    tq &= ((EndQuery = (PFNGLENDQUERYPROC)UA_GL_GetProcAddress("glEndQueryEXT")) != NULL);
    tq &= ((GetQueryObjectiv = (PFNGLGETQUERYOBJECTIVPROC)UA_GL_GetProcAddress("glGetQueryObjectivEXT")) != NULL);
    tq &= ((GetQueryObjectui64v = (PFNGLGETQUERYOBJECTUI64VPROC)UA_GL_GetProcAddress("glGetQueryObjectui64vEXT")) != NULL);
#else
    tq &= ((GenQueries = (PFNGLGENQUERIESPROC)UA_GL_GetProcAddress("glGenQueries")) != NULL);
    tq &= ((DeleteQueries = (PFNGLDELETEQUERIESPROC)UA_GL_GetProcAddress("glDeleteQueries")) != NULL);
    tq &= ((BeginQuery = (PFNGLBEGINQUERYPROC)UA_GL_GetProcAddress("glBeginQuery")) != NULL);
    tq &= ((EndQuery = (PFNGLENDQUERYPROC)UA_GL_GetProcAddress("glEndQuery")) != NULL);
    tq &= ((GetQueryObjectiv = (PFNGLGETQUERYOBJECTIVPROC)UA_GL_GetProcAddress("glGetQueryObjectiv")) != NULL);
    tq &= ((GetQueryObjectui64v = (PFNGLGETQUERYOBJECTUI64VPROC)UA_GL_GetProcAddress("glGetQueryObjectui64v")) != NULL);
#endif
    timerQueries = tq && !getenv("UA_VR_NO_GPU_TIMER");
    return ok;
}

} // namespace VR
