#include "bionic_internal.h"
#include "elf_image.h"

namespace wa::dbt {
namespace {

constexpr int kEglSuccess = 0x3000;
constexpr int kEglNotInitialized = 0x3001;
constexpr int kEglBadDisplay = 0x3008;
constexpr int kEglBadParameter = 0x300c;

void Unavailable(wa_host_call_ctx_t&) {}
void InvalidLocation(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = UINT32_MAX;
}
void NoGlContext(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = 0x0502;
} // GL_INVALID_OPERATION

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
    if (ctx.args[0])
        GuestMemory(ctx).WriteWord(ctx.args[0], 0);
    ctx.ret0 = 12; // SL_RESULT_FEATURE_UNSUPPORTED
}

} // namespace

void RegisterGraphics(wa_dbt& dbt) {
    const Builtin egl[] = {
        {"eglGetDisplay", Dispatch<EglDisplay>},
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
        {"eglMakeCurrent", Dispatch<EglUnavailable>},
        {"eglQueryString", Dispatch<EglUnavailable>},
        {"eglQuerySurface", Dispatch<EglUnavailable>},
        {"eglSwapBuffers", Dispatch<EglUnavailable>},
        {"eglTerminate", Dispatch<EglUnavailable>},
    };
    Register(dbt, "libEGL.so", egl);

    constexpr const char* operations[] = {
        "glActiveTexture",
        "glAttachShader",
        "glBindBuffer",
        "glBindTexture",
        "glBlendFunc",
        "glBufferData",
        "glClear",
        "glClearColor",
        "glCompileShader",
        "glCreateProgram",
        "glCreateShader",
        "glDeleteBuffers",
        "glDeleteProgram",
        "glDeleteShader",
        "glDeleteTextures",
        "glDepthMask",
        "glDisable",
        "glDrawArrays",
        "glDrawElements",
        "glEnable",
        "glEnableVertexAttribArray",
        "glFinish",
        "glFlush",
        "glGenBuffers",
        "glGenTextures",
        "glGetIntegerv",
        "glGetProgramInfoLog",
        "glGetProgramiv",
        "glGetShaderInfoLog",
        "glGetShaderiv",
        "glGetString",
        "glLinkProgram",
        "glPixelStorei",
        "glShaderSource",
        "glTexImage2D",
        "glTexParameterf",
        "glTexParameteri",
        "glTexSubImage2D",
        "glUniform1f",
        "glUniform1i",
        "glUniform2f",
        "glUniform3f",
        "glUniform4f",
        "glUniform4fv",
        "glUniformMatrix2fv",
        "glUniformMatrix3fv",
        "glUniformMatrix4fv",
        "glUseProgram",
        "glVertexAttribPointer",
        "glViewport",
    };
    for (const auto* operation : operations)
        CheckGuest(
            RegisterHost(dbt, "libGLESv2.so", operation, Dispatch<Unavailable>, nullptr, true));
    const Builtin queries[] = {
        {"glGetError", Dispatch<NoGlContext>},
        {"glGetUniformLocation", Dispatch<InvalidLocation>},
        {"glGetAttribLocation", Dispatch<InvalidLocation>},
    };
    Register(dbt, "libGLESv2.so", queries);

    const Builtin legacy_cpp[] = {
        {"_Znwj", Dispatch<New>}, {"_Znwm", Dispatch<New>},     {"_Znaj", Dispatch<New>},
        {"_Znam", Dispatch<New>}, {"_ZdlPv", Dispatch<Delete>}, {"_ZdaPv", Dispatch<Delete>},
    };
    Register(dbt, "libstdc++.so", legacy_cpp);
    const Builtin audio[] = {{"slCreateEngine", Dispatch<SlEngine>}};
    Register(dbt, "libOpenSLES.so", audio);
    dbt.virtual_libraries.push_back("libandroid.so");
}

} // namespace wa::dbt
