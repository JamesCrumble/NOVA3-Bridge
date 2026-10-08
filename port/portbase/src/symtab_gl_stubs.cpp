/*
 * Vendor GL extension stubs for the Mali-G31.
 *
 * the guest library names 36 entry points from three vendor families - NV_fence
 * (Tegra), AMD_performance_monitor (Adreno/AMD) and a spread of QCOM extensions
 * (driver_control, tiled_rendering, extended_get, alpha_test) - because the
 * 2013 Android build shipped for every mobile GPU of the era.
 *
 * It does not IMPORT them: the dynamic table has no undefined symbol for any of
 * the 36, they appear only as strings. The engine asks for them by name at
 * runtime, behind a glGetString(GL_EXTENSIONS) guard, so a device that does not
 * advertise the extension normally never reaches them.
 *
 * The trap is what happens when the guard is passed and the lookup fails. This
 * engine calls the result of eglGetProcAddress without checking it, so a NULL
 * is a branch to address zero - which is how a missing glInvalidateFramebuffer
 * became a SIGSEGV on the first frame that tried to use it.
 *
 * So these are stubs whose first job is to exist. Where a call returns a count
 * it returns zero (no groups, no counters, no driver controls), which is what
 * an honest "this GPU has no such extension" looks like and keeps any code that
 * did slip past the guard from iterating over things that are not there. Fence
 * ids are handed out non-zero and TestFence reports "done" so a stray wait
 * completes instead of spinning.
 *
 * One group is deliberately not inert - see the framebuffer invalidation
 * stubs below, which forward to the driver when it has them. Answering a real
 * question with a no-op is only free when nobody was going to act on it.
 */
#include <stdint.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "so_util.h"
#include "thunk_gen.h"
#include "trace.h"
#include "gl_bridge.h"

/* GL ABI scalar types, spelled out to avoid pulling a GL header into the
 * loader just for four typedefs. */
typedef int            GLsizei;
typedef int            GLint;
typedef unsigned int   GLuint;
typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef char           GLchar;

#define GL_TRUE  1
#define GL_FALSE 0

extern "C" {

/* ---- NV_fence -------------------------------------------------------- */

ABI_ATTR static void gl_stub_GenFencesNV(GLsizei n, GLuint *fences)
{
    for (GLsizei i = 0; i < n && fences; i++)
        fences[i] = (GLuint)(i + 1);       /* non-zero, plausibly valid */
}
ABI_ATTR static void gl_stub_DeleteFencesNV(GLsizei n, const GLuint *fences)
{
    (void)n; (void)fences;
}
ABI_ATTR static void gl_stub_SetFenceNV(GLuint fence, GLenum condition)
{
    (void)fence; (void)condition;
}
ABI_ATTR static GLboolean gl_stub_TestFenceNV(GLuint fence)
{
    (void)fence;
    return GL_TRUE;                        /* always signalled: no waiting */
}
ABI_ATTR static void gl_stub_FinishFenceNV(GLuint fence)
{
    (void)fence;
}
ABI_ATTR static GLboolean gl_stub_IsFenceNV(GLuint fence)
{
    (void)fence;
    return GL_FALSE;
}
ABI_ATTR static void gl_stub_GetFenceivNV(GLuint fence, GLenum pname,
                                          GLint *params)
{
    (void)fence; (void)pname;
    if (params)
        *params = GL_TRUE;                 /* NV_FENCE_STATUS -> completed */
}

/* ---- AMD_performance_monitor ----------------------------------------- */

ABI_ATTR static void gl_stub_GetPerfMonitorGroupsAMD(GLint *numGroups,
                                                     GLsizei groupsSize,
                                                     GLuint *groups)
{
    (void)groupsSize; (void)groups;
    if (numGroups)
        *numGroups = 0;                    /* no monitor groups on this GPU */
}
ABI_ATTR static void gl_stub_GetPerfMonitorCountersAMD(
        GLuint group, GLint *numCounters, GLint *maxActiveCounters,
        GLsizei counterSize, GLuint *counters)
{
    (void)group; (void)counterSize; (void)counters;
    if (numCounters)       *numCounters = 0;
    if (maxActiveCounters) *maxActiveCounters = 0;
}
ABI_ATTR static void gl_stub_GetPerfMonitorGroupStringAMD(
        GLuint group, GLsizei bufSize, GLsizei *length, GLchar *groupString)
{
    (void)group; (void)bufSize;
    if (length)      *length = 0;
    if (groupString && bufSize > 0) groupString[0] = '\0';
}
ABI_ATTR static void gl_stub_GetPerfMonitorCounterStringAMD(
        GLuint group, GLuint counter, GLsizei bufSize, GLsizei *length,
        GLchar *counterString)
{
    (void)group; (void)counter; (void)bufSize;
    if (length)        *length = 0;
    if (counterString && bufSize > 0) counterString[0] = '\0';
}
ABI_ATTR static void gl_stub_GetPerfMonitorCounterInfoAMD(
        GLuint group, GLuint counter, GLenum pname, void *data)
{
    (void)group; (void)counter; (void)pname; (void)data;
}
ABI_ATTR static void gl_stub_GenPerfMonitorsAMD(GLsizei n, GLuint *monitors)
{
    for (GLsizei i = 0; i < n && monitors; i++)
        monitors[i] = (GLuint)(i + 1);
}
ABI_ATTR static void gl_stub_DeletePerfMonitorsAMD(GLsizei n, GLuint *monitors)
{
    (void)n; (void)monitors;
}
ABI_ATTR static void gl_stub_SelectPerfMonitorCountersAMD(
        GLuint monitor, GLboolean enable, GLuint group, GLint numCounters,
        GLuint *counterList)
{
    (void)monitor; (void)enable; (void)group; (void)numCounters;
    (void)counterList;
}
ABI_ATTR static void gl_stub_BeginPerfMonitorAMD(GLuint monitor)
{
    (void)monitor;
}
ABI_ATTR static void gl_stub_EndPerfMonitorAMD(GLuint monitor)
{
    (void)monitor;
}
ABI_ATTR static void gl_stub_GetPerfMonitorCounterDataAMD(
        GLuint monitor, GLenum pname, GLsizei dataSize, GLuint *data,
        GLint *bytesWritten)
{
    (void)monitor; (void)pname; (void)dataSize; (void)data;
    if (bytesWritten)
        *bytesWritten = 0;
}

/* ---- QCOM_driver_control --------------------------------------------- */

ABI_ATTR static void gl_stub_GetDriverControlsQCOM(GLint *num, GLsizei size,
                                                   GLuint *driverControls)
{
    (void)size; (void)driverControls;
    if (num)
        *num = 0;                          /* no driver controls exposed */
}
ABI_ATTR static void gl_stub_GetDriverControlStringQCOM(
        GLuint driverControl, GLsizei bufSize, GLsizei *length,
        GLchar *driverControlString)
{
    (void)driverControl; (void)bufSize;
    if (length)              *length = 0;
    if (driverControlString && bufSize > 0) driverControlString[0] = '\0';
}
ABI_ATTR static void gl_stub_EnableDriverControlQCOM(GLuint driverControl)
{
    (void)driverControl;
}
ABI_ATTR static void gl_stub_DisableDriverControlQCOM(GLuint driverControl)
{
    (void)driverControl;
}

/*
 * Framebuffer invalidation: forwarded to the driver when it has one, and only
 * dropped when it does not.
 *
 * These exist because Real Racing 3 asks for them through eglGetProcAddress
 * and then calls the result without checking it - a NULL there is a jump to
 * address zero. So an address always has to come back. The mistake was making
 * that address do nothing on every device.
 *
 * On a tile-based GPU this is not a hint that can be swallowed for free. It is
 * how the engine says "this attachment is finished with: do not resolve it
 * back to memory, and do not reload it at the start of the next tile pass".
 * Dropping it means the depth and stencil of every render target are written
 * out and read back every frame at full bandwidth - and on a Mali-G31 sharing
 * memory with the CPU that is about the most expensive thing you can quietly
 * take away. This game renders almost all of its geometry into offscreen
 * targets, so it pays that toll several times a frame.
 *
 * It was written as a no-op because the emulator's Mesa/llvmpipe has no such
 * entry point and the port had only ever run there. Same shape of mistake as
 * the FRAMEBUFFER_FETCH rewrite: a workaround for the machine we develop on,
 * left sitting in the path of the machine we ship to.
 *
 * Resolved through SDL rather than our own GL tables because those are indexed
 * by what the game imports, while this is a question only the driver can
 * answer. Only integers and pointers cross here, so the softfp/hardfp
 * difference the thunks exist for does not arise.
 */
typedef void (*InvalidateFn)(GLenum, GLsizei, const GLenum *);
typedef void (*InvalidateSubFn)(GLenum, GLsizei, const GLenum *,
                                GLint, GLint, GLsizei, GLsizei);

static void *resolve_driver_entry(const char *name)
{
    void *addr = SDL_GL_GetProcAddress(name);
    trace("gl: %s -> %s", name,
          addr ? "forwarding to the driver" : "absent, the call is dropped");
    return addr;
}

ABI_ATTR static void gl_stub_InvalidateFramebuffer(GLenum target,
                                                   GLsizei numAttachments,
                                                   const GLenum *attachments)
{
    static InvalidateFn real      = NULL;
    static bool         looked_up = false;
    if (!looked_up) {
        looked_up = true;
        /* GLES 3 spells it one way and the GLES 2 extension another; this
         * hardware reports GLES 3.2 but the game asks for both names. */
        real = (InvalidateFn)resolve_driver_entry("glInvalidateFramebuffer");
        if (!real)
            real = (InvalidateFn)resolve_driver_entry("glDiscardFramebufferEXT");
    }
    if (real)
        real(target, numAttachments, attachments);
}

ABI_ATTR static void gl_stub_InvalidateSubFramebuffer(GLenum target,
                                                      GLsizei numAttachments,
                                                      const GLenum *attachments,
                                                      GLint x, GLint y,
                                                      GLsizei width,
                                                      GLsizei height)
{
    static InvalidateSubFn real      = NULL;
    static bool            looked_up = false;
    if (!looked_up) {
        looked_up = true;
        real = (InvalidateSubFn)resolve_driver_entry("glInvalidateSubFramebuffer");
    }
    if (real)
        real(target, numAttachments, attachments, x, y, width, height);
}

}  /* extern "C" */

DynLibFunction symtable_gl_stubs[] = {
    THUNK_SPECIFIC("glGenFencesNV",                   gl_stub_GenFencesNV),
    THUNK_SPECIFIC("glDeleteFencesNV",                gl_stub_DeleteFencesNV),
    THUNK_SPECIFIC("glSetFenceNV",                    gl_stub_SetFenceNV),
    THUNK_SPECIFIC("glTestFenceNV",                   gl_stub_TestFenceNV),
    THUNK_SPECIFIC("glFinishFenceNV",                 gl_stub_FinishFenceNV),
    THUNK_SPECIFIC("glIsFenceNV",                     gl_stub_IsFenceNV),
    THUNK_SPECIFIC("glGetFenceivNV",                  gl_stub_GetFenceivNV),

    THUNK_SPECIFIC("glGetPerfMonitorGroupsAMD",       gl_stub_GetPerfMonitorGroupsAMD),
    THUNK_SPECIFIC("glGetPerfMonitorCountersAMD",     gl_stub_GetPerfMonitorCountersAMD),
    THUNK_SPECIFIC("glGetPerfMonitorGroupStringAMD",  gl_stub_GetPerfMonitorGroupStringAMD),
    THUNK_SPECIFIC("glGetPerfMonitorCounterStringAMD",gl_stub_GetPerfMonitorCounterStringAMD),
    THUNK_SPECIFIC("glGetPerfMonitorCounterInfoAMD",  gl_stub_GetPerfMonitorCounterInfoAMD),
    THUNK_SPECIFIC("glGenPerfMonitorsAMD",            gl_stub_GenPerfMonitorsAMD),
    THUNK_SPECIFIC("glDeletePerfMonitorsAMD",         gl_stub_DeletePerfMonitorsAMD),
    THUNK_SPECIFIC("glSelectPerfMonitorCountersAMD",  gl_stub_SelectPerfMonitorCountersAMD),
    THUNK_SPECIFIC("glBeginPerfMonitorAMD",           gl_stub_BeginPerfMonitorAMD),
    THUNK_SPECIFIC("glEndPerfMonitorAMD",             gl_stub_EndPerfMonitorAMD),
    THUNK_SPECIFIC("glGetPerfMonitorCounterDataAMD",  gl_stub_GetPerfMonitorCounterDataAMD),

    THUNK_SPECIFIC("glGetDriverControlsQCOM",         gl_stub_GetDriverControlsQCOM),
    THUNK_SPECIFIC("glGetDriverControlStringQCOM",    gl_stub_GetDriverControlStringQCOM),
    THUNK_SPECIFIC("glEnableDriverControlQCOM",       gl_stub_EnableDriverControlQCOM),
    THUNK_SPECIFIC("glDisableDriverControlQCOM",      gl_stub_DisableDriverControlQCOM),
    THUNK_SPECIFIC("glInvalidateFramebuffer",         gl_stub_InvalidateFramebuffer),
    THUNK_SPECIFIC("glInvalidateSubFramebuffer",      gl_stub_InvalidateSubFramebuffer),
    THUNK_SPECIFIC("glDiscardFramebufferEXT",         gl_stub_InvalidateFramebuffer),

    { NULL, 0 },
};
