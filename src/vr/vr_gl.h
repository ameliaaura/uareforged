#ifndef UA_VR_GL_H_INCLUDED
#define UA_VR_GL_H_INCLUDED

// GL entry points the VR module uses, loaded through SDL_GL_GetProcAddress so
// the module never depends on platform prototypes (desktop GL 4.1 core today,
// GLES 3.2 on Quest later: every function here exists in GLES 3.0+ except
// glGetQueryObjectui64v / GL_TIME_ELAPSED, which come from
// GL_EXT_disjoint_timer_query there).

#include "../system/glapi.h"

namespace VR
{

struct GLFuncs
{
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = NULL;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = NULL;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = NULL;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D = NULL;
    PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer = NULL;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus = NULL;
    PFNGLGENRENDERBUFFERSPROC GenRenderbuffers = NULL;
    PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers = NULL;
    PFNGLBINDRENDERBUFFERPROC BindRenderbuffer = NULL;
    PFNGLRENDERBUFFERSTORAGEPROC RenderbufferStorage = NULL;
    PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC RenderbufferStorageMultisample = NULL;
    PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer = NULL;

    PFNGLGENQUERIESPROC GenQueries = NULL;
    PFNGLDELETEQUERIESPROC DeleteQueries = NULL;
    PFNGLBEGINQUERYPROC BeginQuery = NULL;
    PFNGLENDQUERYPROC EndQuery = NULL;
    PFNGLGETQUERYOBJECTIVPROC GetQueryObjectiv = NULL;
    PFNGLGETQUERYOBJECTUI64VPROC GetQueryObjectui64v = NULL;

    PFNGLBLENDFUNCSEPARATEPROC BlendFuncSeparate = NULL;
    PFNGLACTIVETEXTUREPROC ActiveTexture = NULL;

    PFNGLCREATESHADERPROC CreateShader = NULL;
    PFNGLSHADERSOURCEPROC ShaderSource = NULL;
    PFNGLCOMPILESHADERPROC CompileShader = NULL;
    PFNGLGETSHADERIVPROC GetShaderiv = NULL;
    PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog = NULL;
    PFNGLDELETESHADERPROC DeleteShader = NULL;
    PFNGLCREATEPROGRAMPROC CreateProgram = NULL;
    PFNGLATTACHSHADERPROC AttachShader = NULL;
    PFNGLBINDATTRIBLOCATIONPROC BindAttribLocation = NULL;
    PFNGLLINKPROGRAMPROC LinkProgram = NULL;
    PFNGLGETPROGRAMIVPROC GetProgramiv = NULL;
    PFNGLDELETEPROGRAMPROC DeleteProgram = NULL;
    PFNGLUSEPROGRAMPROC UseProgram = NULL;
    PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation = NULL;
    PFNGLUNIFORM1IPROC Uniform1i = NULL;
    PFNGLUNIFORM4FPROC Uniform4f = NULL;
    PFNGLUNIFORMMATRIX4FVPROC UniformMatrix4fv = NULL;

    PFNGLGENVERTEXARRAYSPROC GenVertexArrays = NULL;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray = NULL;
    PFNGLGENBUFFERSPROC GenBuffers = NULL;
    PFNGLBINDBUFFERPROC BindBuffer = NULL;
    PFNGLBUFFERDATAPROC BufferData = NULL;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray = NULL;
    PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer = NULL;

    bool Load();
    bool timerQueries = false;
};

extern GLFuncs gl;

} // namespace VR

#endif
