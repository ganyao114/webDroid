#include "bionic_internal.h"
#include "elf_image.h"

#include <utility>

namespace wa::dbt {
namespace {

constexpr int kEglSuccess = 0x3000;
constexpr int kEglNotInitialized = 0x3001;
constexpr int kEglBadDisplay = 0x3008;
constexpr int kEglBadParameter = 0x300c;

void Unavailable(wa_host_call_ctx_t& ctx) {
    ctx.dbt->running->gl_error = 0x0502; // GL_INVALID_OPERATION
}
void InvalidLocation(wa_host_call_ctx_t& ctx) {
    Unavailable(ctx);
    ctx.ret0 = UINT32_MAX;
}
void NoGlContext(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = std::exchange(ctx.dbt->running->gl_error, 0);
}
void WaitFailed(wa_host_call_ctx_t& ctx) {
    Unavailable(ctx);
    ctx.ret0 = 0x911d; // GL_WAIT_FAILED
}

void EglDisplay(wa_host_call_ctx_t& ctx) {
    if (!ctx.args[0])
        ctx.ret0 = 1;
    else
        ctx.dbt->running->egl_error = kEglBadParameter;
}

void EglUnavailable(wa_host_call_ctx_t& ctx) {
    ctx.dbt->running->egl_error = ctx.args[0] == 1 ? kEglNotInitialized : kEglBadDisplay;
}

void EglError(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = ctx.dbt->running->egl_error;
    ctx.dbt->running->egl_error = kEglSuccess;
}

void EglBind(wa_host_call_ctx_t& ctx) {
    if (ctx.args[0] == 0x30a0)
        ctx.ret0 = 1; // EGL_OPENGL_ES_API
    else
        ctx.dbt->running->egl_error = kEglBadParameter;
}

void EglReleaseThread(wa_host_call_ctx_t& ctx) {
    ctx.dbt->running->egl_error = kEglSuccess;
    ctx.ret0 = 1;
}

void EglProc(wa_host_call_ctx_t& ctx) {
    const auto name = GuestMemory(ctx).String(ctx.args[0], 256);
    for (const auto* library : {"libGLESv2.so", "libGLESv3.so", "libEGL.so"}) {
        ctx.ret0 = FindVirtualSymbol(*ctx.dbt, library, name);
        if (ctx.ret0)
            break;
    }
}

void New(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = AllocateGuest(*ctx.dbt, ctx.args[0]);
}
void Delete(wa_host_call_ctx_t& ctx) {
    FreeGuest(*ctx.dbt, ctx.args[0]);
}

void SlEngine(wa_host_call_ctx_t& ctx) {
    if (!ctx.args[0]) {
        ctx.ret0 = 2; // SL_RESULT_PARAMETER_INVALID / XA_RESULT_PARAMETER_INVALID.
        return;
    }
    GuestMemory(ctx).WriteWord(ctx.args[0], 0);
    ctx.ret0 = 12; // SL_RESULT_FEATURE_UNSUPPORTED / XA_RESULT_FEATURE_UNSUPPORTED.
}

void RegisterOpenSlInterfaceIds(wa_dbt& dbt) {
    // OpenSL ES exports pointers to 16-byte interface IDs. Use the UUIDs from
    // AOSP frameworks/wilhelm/src/OpenSLES_IID.cpp in the guest ABI layout.
    struct InterfaceId {
        uint32_t low;
        uint16_t mid, high, sequence;
        uint8_t node[6]{0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b};
    };
    static_assert(sizeof(InterfaceId) == 16);
    constexpr std::pair<const char*, InterfaceId> ids[] = {
        {"SL_IID_BUFFERQUEUE", {0x2bc99cc0, 0xddd4, 0x11db, 0x8d99}},
        {"SL_IID_ENGINE", {0x8d97c260, 0xddd4, 0x11db, 0x958f}},
        {"SL_IID_PLAY", {0xef0bd9c0, 0xddd7, 0x11db, 0xbf49}},
        {"SL_IID_VOLUME", {0x09e8ede0, 0xddde, 0x11db, 0xb4f6}},
    };
    const GuestMemory memory(dbt.mmu, dbt.arch);
    auto slot = AllocateGuest(dbt, std::size(ids) * (sizeof(InterfaceId) + memory.width()));
    if (!slot) throw GuestFault{WA_ERR_NOMEM};
    auto address = slot + std::size(ids) * memory.width();
    for (const auto& [name, id] : ids) {
        memory.WriteWord(slot, address);
        memory.Write(address, id);
        CheckGuest(RegisterData(dbt, "libOpenSLES.so", name, slot, true));
        slot += memory.width();
        address += sizeof(id);
    }
}

void NativeWindowGeometry(wa_host_call_ctx_t& ctx) {
    // This backend exposes EGL surfaces and does not issue ANativeWindow handles.
    ctx.ret0 = WA_ERR_INVALID;
}

} // namespace

void RegisterGraphics(wa_dbt& dbt) {
    const Builtin egl[] = {
        {"eglGetDisplay", Dispatch<EglDisplay>},
        {"eglGetCurrentDisplay", Dispatch<Unavailable>},
        {"eglGetCurrentContext", Dispatch<Unavailable>},
        {"eglGetCurrentSurface", Dispatch<Unavailable>},
        {"eglGetError", Dispatch<EglError>},
        {"eglBindAPI", Dispatch<EglBind>},
        {"eglGetProcAddress", Dispatch<EglProc>},
        {"eglInitialize", Dispatch<EglUnavailable>},
        {"eglChooseConfig", Dispatch<EglUnavailable>},
        {"eglCreateContext", Dispatch<EglUnavailable>},
        {"eglCreateWindowSurface", Dispatch<EglUnavailable>},
        {"eglCreatePbufferSurface", Dispatch<EglUnavailable>},
        {"eglDestroyContext", Dispatch<EglUnavailable>},
        {"eglDestroySurface", Dispatch<EglUnavailable>},
        {"eglCreateImageKHR", Dispatch<EglUnavailable>},
        {"eglDestroyImageKHR", Dispatch<EglUnavailable>},
        {"eglMakeCurrent", Dispatch<EglUnavailable>},
        {"eglQueryString", Dispatch<EglUnavailable>},
        {"eglQuerySurface", Dispatch<EglUnavailable>},
        {"eglQueryContext", Dispatch<EglUnavailable>},
        {"eglReleaseThread", Dispatch<EglReleaseThread>},
        {"eglSwapBuffers", Dispatch<EglUnavailable>},
        {"eglTerminate", Dispatch<EglUnavailable>},
    };
    Register(dbt, "libEGL.so", egl);

    // Core GLES entry points remain visible when the native backend has no context.
    constexpr const char* operations[] = {
        "glActiveTexture",
        "glAttachShader",
        "glBindAttribLocation",
        "glBindBuffer",
        "glBindFramebuffer",
        "glBindRenderbuffer",
        "glBindTexture",
        "glBlendColor",
        "glBlendEquation",
        "glBlendFunc",
        "glBufferData",
        "glBufferSubData",
        "glCheckFramebufferStatus",
        "glClear",
        "glClearColor",
        "glClearStencil",
        "glColorMask",
        "glCompileShader",
        "glCompressedTexImage2D",
        "glCompressedTexSubImage2D",
        "glCopyTexImage2D",
        "glCopyTexSubImage2D",
        "glCreateProgram",
        "glCreateShader",
        "glCullFace",
        "glDeleteBuffers",
        "glDeleteFramebuffers",
        "glDeleteProgram",
        "glDeleteRenderbuffers",
        "glDeleteShader",
        "glDeleteTextures",
        "glDepthMask",
        "glDepthRangef",
        "glDetachShader",
        "glDisable",
        "glDisableVertexAttribArray",
        "glDrawArrays",
        "glDrawElements",
        "glEnable",
        "glEnableVertexAttribArray",
        "glFinish",
        "glFlush",
        "glFramebufferRenderbuffer",
        "glFramebufferTexture2D",
        "glFrontFace",
        "glGenBuffers",
        "glGenFramebuffers",
        "glGenRenderbuffers",
        "glGenTextures",
        "glGenerateMipmap",
        "glGetActiveAttrib",
        "glGetActiveUniform",
        "glGetAttachedShaders",
        "glGetBooleanv",
        "glGetBufferParameteriv",
        "glGetFloatv",
        "glGetFramebufferAttachmentParameteriv",
        "glGetIntegerv",
        "glGetProgramInfoLog",
        "glGetProgramiv",
        "glGetRenderbufferParameteriv",
        "glGetShaderInfoLog",
        "glGetShaderPrecisionFormat",
        "glGetShaderSource",
        "glGetShaderiv",
        "glGetString",
        "glGetTexParameterfv",
        "glGetTexParameteriv",
        "glGetUniformfv",
        "glGetUniformiv",
        "glGetVertexAttribPointerv",
        "glGetVertexAttribfv",
        "glGetVertexAttribiv",
        "glHint",
        "glIsBuffer",
        "glIsEnabled",
        "glIsFramebuffer",
        "glIsProgram",
        "glIsRenderbuffer",
        "glIsShader",
        "glIsTexture",
        "glLineWidth",
        "glLinkProgram",
        "glPixelStorei",
        "glPolygonOffset",
        "glReadPixels",
        "glReleaseShaderCompiler",
        "glRenderbufferStorage",
        "glSampleCoverage",
        "glScissor",
        "glShaderSource",
        "glStencilFunc",
        "glStencilFuncSeparate",
        "glStencilMask",
        "glStencilMaskSeparate",
        "glStencilOp",
        "glStencilOpSeparate",
        "glTexImage2D",
        "glTexParameterf",
        "glTexParameterfv",
        "glTexParameteri",
        "glTexParameteriv",
        "glTexSubImage2D",
        "glUniform1f",
        "glUniform1fv",
        "glUniform1i",
        "glUniform1iv",
        "glUniform2f",
        "glUniform2fv",
        "glUniform2i",
        "glUniform2iv",
        "glUniform3f",
        "glUniform3fv",
        "glUniform3i",
        "glUniform3iv",
        "glUniform4f",
        "glUniform4fv",
        "glUniform4i",
        "glUniform4iv",
        "glUniformMatrix2fv",
        "glUniformMatrix3fv",
        "glUniformMatrix4fv",
        "glUseProgram",
        "glValidateProgram",
        "glVertexAttrib1f",
        "glVertexAttrib2f",
        "glVertexAttrib2fv",
        "glVertexAttrib3f",
        "glVertexAttrib3fv",
        "glVertexAttrib4f",
        "glVertexAttrib4fv",
        "glVertexAttribPointer",
        "glViewport",
    };
    const Builtin queries[] = {
        {"glGetError", Dispatch<NoGlContext>},
        {"glGetUniformLocation", Dispatch<InvalidLocation>},
        {"glGetAttribLocation", Dispatch<InvalidLocation>},
        {"glEGLImageTargetTexture2DOES", Dispatch<Unavailable>},
    };
    for (const auto* library : {"libGLESv2.so", "libGLESv3.so"}) {
        for (const auto* operation : operations)
            CheckGuest(RegisterHost(dbt, library, operation, Dispatch<Unavailable>, nullptr, true));
        Register(dbt, library, queries);
    }
    const char* gles3_operations[] = {
        "glBeginQuery", "glBeginTransformFeedback", "glBindBufferBase", "glBindBufferRange", "glBindSampler",
        "glBindTransformFeedback", "glBindVertexArray", "glBlendEquationSeparate", "glBlendFuncSeparate",
        "glBlitFramebuffer", "glClearBufferfi", "glClearBufferfv", "glClearBufferiv", "glClearBufferuiv",
        "glClearDepthf", "glCompressedTexImage3D", "glCompressedTexSubImage3D", "glCopyBufferSubData",
        "glCopyTexSubImage3D", "glDeleteQueries", "glDeleteSamplers", "glDeleteSync",
        "glDeleteTransformFeedbacks", "glDeleteVertexArrays", "glDepthFunc", "glDispatchCompute",
        "glDrawArraysInstanced", "glDrawBuffers", "glDrawElementsInstanced", "glDrawRangeElements",
        "glEndQuery", "glEndTransformFeedback", "glFenceSync", "glFlushMappedBufferRange",
        "glFramebufferTextureLayer", "glGenQueries", "glGenSamplers", "glGenTransformFeedbacks", "glGenVertexArrays",
        "glGetActiveUniformBlockName", "glGetActiveUniformBlockiv", "glGetActiveUniformsiv", "glGetInteger64i_v",
        "glGetInteger64v", "glGetIntegeri_v", "glGetInternalformativ", "glGetProgramBinary", "glGetQueryObjectuiv",
        "glGetQueryiv", "glGetSamplerParameterfv", "glGetSamplerParameteriv", "glGetStringi", "glGetSynciv",
        "glGetTransformFeedbackVarying", "glGetUniformIndices", "glInvalidateFramebuffer",
        "glInvalidateSubFramebuffer", "glIsQuery", "glIsSampler", "glIsSync", "glIsTransformFeedback",
        "glIsVertexArray", "glMapBufferRange", "glMemoryBarrierByRegion", "glPauseTransformFeedback",
        "glProgramBinary", "glProgramParameteri", "glReadBuffer", "glRenderbufferStorageMultisample",
        "glResumeTransformFeedback", "glSamplerParameterf", "glSamplerParameteri", "glSamplerParameteriv",
        "glTexImage3D", "glTexStorage2D", "glTexStorage3D", "glTexSubImage3D", "glTransformFeedbackVaryings",
        "glUniform1ui", "glUniform1uiv", "glUniform2ui", "glUniform2uiv", "glUniform3ui", "glUniform3uiv",
        "glUniform4ui", "glUniform4uiv", "glUniformBlockBinding", "glUniformMatrix2x3fv", "glUniformMatrix2x4fv",
        "glUniformMatrix3x2fv", "glUniformMatrix3x4fv", "glUniformMatrix4x2fv", "glUniformMatrix4x3fv",
        "glUnmapBuffer", "glVertexAttribDivisor", "glVertexAttribI4i", "glVertexAttribI4iv", "glVertexAttribI4ui",
        "glVertexAttribI4uiv", "glVertexAttribIPointer", "glWaitSync",
    };
    for (const auto* operation : gles3_operations)
        CheckGuest(RegisterHost(dbt, "libGLESv3.so", operation, Dispatch<Unavailable>, nullptr, true));
    const Builtin gles3_queries[] = {
        {"glClientWaitSync", Dispatch<WaitFailed>},
        {"glGetFragDataLocation", Dispatch<InvalidLocation>},
        {"glGetUniformBlockIndex", Dispatch<InvalidLocation>},
    };
    Register(dbt, "libGLESv3.so", gles3_queries);

    const Builtin legacy_cpp[] = {
        {"_Znwj", Dispatch<New>}, {"_Znwm", Dispatch<New>},     {"_Znaj", Dispatch<New>},
        {"_Znam", Dispatch<New>}, {"_ZdlPv", Dispatch<Delete>}, {"_ZdaPv", Dispatch<Delete>},
    };
    Register(dbt, "libstdc++.so", legacy_cpp);
    const Builtin audio[] = {{"slCreateEngine", Dispatch<SlEngine>}};
    Register(dbt, "libOpenSLES.so", audio);
    RegisterOpenSlInterfaceIds(dbt);
    const Builtin open_max[] = {{"xaCreateEngine", Dispatch<SlEngine>}};
    Register(dbt, "libOpenMAXAL.so", open_max);
    const Builtin window[] = {{"ANativeWindow_setBuffersGeometry", Dispatch<NativeWindowGeometry>}};
    Register(dbt, "libandroid.so", window);
}

} // namespace wa::dbt
