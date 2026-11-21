#include "gfx_surface.h"

#include <climits>
#include <cstdio>
#include <limits>

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5_webgl.h>

#include <cstdarg>
#include <cstring>
#include <string>

// Emscripten's WebGL helpers define these macros for their own API.
#undef GLenum
#undef GLint
#undef GLint64
#endif

namespace {

struct Surface {
    wa_gfx_surface_config_t config{};
    uint32_t generation = 0;
    bool initialized = false;
    bool current = false;
#ifdef __EMSCRIPTEN__
    std::string selector;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context = 0;
#endif
};

thread_local Surface surface;

bool valid_size(uint32_t width, uint32_t height) {
    return width && height && width <= INT_MAX && height <= INT_MAX;
}

int32_t current_status() {
    if (!surface.current) {
        return WA_ERR_INVALID;
    }
#ifdef __EMSCRIPTEN__
    if (emscripten_webgl_get_current_context() != surface.context) {
        return WA_ERR_INVALID;
    }
    if (emscripten_is_webgl_context_lost(surface.context)) {
        return WA_ERR_AGAIN;
    }
#endif
    return WA_OK;
}

} // namespace

int32_t wa_gfx_init(const char* canvas_selector, const wa_gfx_surface_config_t* cfg) {
    if (!cfg || !valid_size(cfg->width, cfg->height)) {
        return WA_ERR_INVALID;
    }
    if (surface.initialized) {
        return WA_ERR_BUSY;
    }

    wa_gfx_surface_config_t config = *cfg;
#ifdef __EMSCRIPTEN__
    const char* selector = canvas_selector && *canvas_selector ? canvas_selector : "#canvas";
    if (*selector == '#' && std::strcmp(selector, "#canvas") != 0) {
        ++selector; // Emscripten's Module.canvas lookup takes plain IDs for other targets.
    }
    if (emscripten_set_canvas_element_size(selector, cfg->width, cfg->height) !=
        EMSCRIPTEN_RESULT_SUCCESS) {
        return WA_ERR_NOENT;
    }

    EmscriptenWebGLContextAttributes attributes;
    emscripten_webgl_init_context_attributes(&attributes);
    attributes.majorVersion = 2;
    attributes.alpha = true;
    attributes.depth = cfg->depth_bits != 0;
    attributes.stencil = false;
    attributes.antialias = cfg->samples > 1;
    attributes.preserveDrawingBuffer = true;
    attributes.proxyContextToMainThread = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_DISALLOW;

    const auto context = emscripten_webgl_create_context(selector, &attributes);
    if (!context) {
        return WA_ERR_NOTSUP;
    }
    if (emscripten_webgl_make_context_current(context) != EMSCRIPTEN_RESULT_SUCCESS) {
        emscripten_webgl_destroy_context(context);
        return WA_ERR_INTERNAL;
    }

    int width = 0;
    int height = 0;
    const auto size_status = emscripten_webgl_get_drawing_buffer_size(context, &width, &height);
    if (size_status != EMSCRIPTEN_RESULT_SUCCESS || width <= 0 || height <= 0) {
        emscripten_webgl_destroy_context(context);
        return WA_ERR_INTERNAL;
    }
    GLint depth_bits = 0;
    GLint samples = 0;
    glGetIntegerv(GL_DEPTH_BITS, &depth_bits);
    glGetIntegerv(GL_SAMPLES, &samples);
    config = {static_cast<uint32_t>(width), static_cast<uint32_t>(height),
              static_cast<uint32_t>(depth_bits), static_cast<uint32_t>(samples)};
    surface.context = context;
    surface.selector = selector;
#else
    (void)canvas_selector;
#endif
    surface.config = config;
    ++surface.generation;
    surface.initialized = true;
    surface.current = true;
    return WA_OK;
}

int32_t wa_gfx_resize(uint32_t width, uint32_t height) {
    if (!surface.initialized || !valid_size(width, height)) {
        return WA_ERR_INVALID;
    }
#ifdef __EMSCRIPTEN__
    if (width == surface.config.width && height == surface.config.height) {
        return WA_OK;
    }
    if (emscripten_set_canvas_element_size(surface.selector.c_str(), width, height) !=
        EMSCRIPTEN_RESULT_SUCCESS) {
        return WA_ERR_INTERNAL;
    }
    int actual_width = 0;
    int actual_height = 0;
    if (emscripten_webgl_get_drawing_buffer_size(surface.context, &actual_width, &actual_height) !=
            EMSCRIPTEN_RESULT_SUCCESS ||
        actual_width <= 0 || actual_height <= 0) {
        return WA_ERR_INTERNAL;
    }
    width = actual_width;
    height = actual_height;
#endif
    surface.config.width = width;
    surface.config.height = height;
    return WA_OK;
}

int32_t wa_gfx_swap_buffers(void) {
    const int32_t status = current_status();
#ifdef __EMSCRIPTEN__
    // Chrome presents an OffscreenCanvas when control returns to the worker event loop.
    if (status == WA_OK) {
        glFlush();
    }
#endif
    return status;
}

int32_t wa_gfx_read_pixels_rgba(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                uint8_t* out_rgba) {
    if (!out_rgba || !valid_size(width, height) || x > surface.config.width ||
        y > surface.config.height || width > surface.config.width - x ||
        height > surface.config.height - y ||
        width > std::numeric_limits<size_t>::max() / 4 / height) {
        return WA_ERR_INVALID;
    }
    const int32_t status = current_status();
    if (status != WA_OK) {
        return status;
    }
#ifdef __EMSCRIPTEN__
    GLint pack_buffer = 0;
    GLint read_framebuffer = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    const GLenum pack_settings[] = {GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS,
                                    GL_PACK_SKIP_ROWS};
    GLint saved[4];
    for (size_t i = 0; i < 4; ++i) {
        glGetIntegerv(pack_settings[i], &saved[i]);
        glPixelStorei(pack_settings[i], i == 0 ? 1 : 0);
    }
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, out_rgba);
    const GLenum error = glGetError();
    for (size_t i = 0; i < 4; ++i) {
        glPixelStorei(pack_settings[i], saved[i]);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_framebuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
    return error == GL_NO_ERROR ? WA_OK : WA_ERR_INTERNAL;
#else
    for (size_t i = 0, count = size_t{width} * height; i < count; ++i) {
        out_rgba[4 * i] = 0;
        out_rgba[4 * i + 1] = 0;
        out_rgba[4 * i + 2] = 0;
        out_rgba[4 * i + 3] = 255;
    }
    return WA_OK;
#endif
}

void wa_gfx_shutdown(void) {
    if (!surface.initialized) {
        return;
    }
#ifdef __EMSCRIPTEN__
    if (emscripten_webgl_get_current_context() == surface.context) {
        emscripten_webgl_make_context_current(0);
    }
    emscripten_webgl_destroy_context(surface.context);
    surface.context = 0;
    surface.selector.clear();
#endif
    surface.config = {};
    surface.initialized = false;
    surface.current = false;
    ++surface.generation;
}

void wa_android_log(int32_t prio, const char* tag, const char* msg) {
    tag = tag ? tag : "Android";
    msg = msg ? msg : "";
#ifdef __EMSCRIPTEN__
    const int severity = prio >= 6 ? EM_LOG_ERROR : prio == 5 ? EM_LOG_WARN : 0;
    emscripten_log(EM_LOG_CONSOLE | severity, "[%s] %s", tag, msg);
#else
    std::fprintf(prio >= 5 ? stderr : stdout, "[%s] %s\n", tag, msg);
#endif
}

namespace wa::gfx {

const wa_gfx_surface_config_t* surface_config() {
    return surface.initialized ? &surface.config : nullptr;
}

uint32_t surface_generation() {
    return surface.generation;
}

bool has_current_surface() {
    return surface.current;
}

int32_t make_current(bool current) {
    if (!surface.initialized) {
        return WA_ERR_INVALID;
    }
#ifdef __EMSCRIPTEN__
    if (emscripten_webgl_make_context_current(current ? surface.context : 0) !=
        EMSCRIPTEN_RESULT_SUCCESS) {
        return WA_ERR_INTERNAL;
    }
#endif
    surface.current = current;
    return WA_OK;
}

} // namespace wa::gfx

#ifdef __EMSCRIPTEN__
extern "C" int __android_log_write(int prio, const char* tag, const char* msg) {
    wa_android_log(prio, tag, msg);
    return 1;
}

extern "C" int __android_log_print(int prio, const char* tag, const char* format, ...) {
    char message[4096];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    return length < 0 ? length : __android_log_write(prio, tag, message);
}
#endif
