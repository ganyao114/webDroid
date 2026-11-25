#include "egl_shim.h"
#include "gfx_surface.h"

#include <utility>

#ifdef __EMSCRIPTEN__
#include <EGL/egl.h>
#endif

namespace wa::egl {
namespace {

constexpr Handle display_id = 1;
constexpr Handle config_id = 2;
constexpr Handle surface_id = 3;
constexpr Handle context_id = 4;

constexpr int32_t success = 0x3000;
constexpr int32_t not_initialized = 0x3001;
constexpr int32_t bad_alloc = 0x3003;
constexpr int32_t bad_attribute = 0x3004;
constexpr int32_t bad_config = 0x3005;
constexpr int32_t bad_context = 0x3006;
constexpr int32_t bad_display = 0x3008;
constexpr int32_t bad_match = 0x3009;
constexpr int32_t bad_native_window = 0x300b;
constexpr int32_t bad_surface = 0x300d;

constexpr int32_t buffer_size = 0x3020;
constexpr int32_t alpha_size = 0x3021;
constexpr int32_t blue_size = 0x3022;
constexpr int32_t green_size = 0x3023;
constexpr int32_t red_size = 0x3024;
constexpr int32_t depth_size = 0x3025;
constexpr int32_t stencil_size = 0x3026;
constexpr int32_t config_id_attribute = 0x3028;
constexpr int32_t samples = 0x3031;
constexpr int32_t sample_buffers = 0x3032;
constexpr int32_t surface_type = 0x3033;
constexpr int32_t renderable_type = 0x3040;
constexpr int32_t height_attribute = 0x3056;
constexpr int32_t width_attribute = 0x3057;
constexpr int32_t context_major_version = 0x3098;
constexpr int32_t context_minor_version = 0x30fb;
constexpr int32_t surface_types = 0x0001 | 0x0004; // EGL_PBUFFER_BIT | EGL_WINDOW_BIT
constexpr int32_t es_bits = 0x0004 | 0x0040; // OpenGL ES 2 and 3
constexpr int32_t dont_care = -1;

struct State {
    uint32_t generation = 0;
    int32_t error = success;
    bool initialized = false;
    bool has_surface = false;
    bool has_context = false;
    bool current = false;
    int32_t context_version = 0;
};

State& state() {
    thread_local State state;
    const uint32_t generation = gfx::surface_generation();
    if (state.generation != generation) {
        state = {.generation = generation};
    }
    return state;
}

bool valid_display(Handle display) {
    if (display != display_id) {
        return fail(bad_display);
    }
    if (!state().initialized) {
        return fail(not_initialized);
    }
    return true;
}

bool valid_config(Handle display, Handle config) {
    return valid_display(display) && (config == config_id || fail(bad_config));
}

bool valid_surface(Handle surface) {
    return (surface == surface_id && state().has_surface) || fail(bad_surface);
}

bool valid_context(Handle context) {
    return (context == context_id && state().has_context) || fail(bad_context);
}

} // namespace

bool fail(int32_t error) {
    state().error = error;
    return false;
}

void unsupported_image(Handle display) {
    if (valid_display(display)) fail(bad_parameter);
}

const char* query_string(Handle display, int32_t name) {
    if (display == 0 && name == 0x3055) return ""; // EGL_EXTENSIONS
    if (!valid_display(display)) return nullptr;
    switch (name) {
    case 0x3053: return "WasmAndroid";  // EGL_VENDOR
    case 0x3054: return "1.5";         // EGL_VERSION
    case 0x3055: return "";            // EGL_EXTENSIONS
    case 0x308d: return "OpenGL_ES";   // EGL_CLIENT_APIS
    default: fail(bad_parameter); return nullptr;
    }
}

int32_t get_error() {
    return std::exchange(state().error, success);
}

Handle get_display(uint64_t native_display) {
    if (native_display != 0) {
        fail(bad_parameter);
        return 0;
    }
    return display_id;
}

bool initialize(Handle display, int32_t* major, int32_t* minor) {
    if (display != display_id) {
        return fail(bad_display);
    }
    if (!gfx::surface_config()) {
        return fail(not_initialized);
    }
    state().initialized = true;
    if (major) {
        *major = 1;
    }
    if (minor) {
        *minor = 5;
    }
    return true;
}

bool choose_config(Handle display, Attributes attributes, Handle& match) {
    if (!valid_display(display)) {
        return false;
    }
    const auto& surface = *gfx::surface_config();
    bool matches = true;
    for (size_t i = 0; i < attributes.size(); i += 2) {
        const auto key = attributes[i];
        const auto value = attributes[i + 1];
        if (value == dont_care) {
            continue;
        }
        if (value < 0) {
            return fail(bad_attribute);
        }
        bool attribute_matches;
        switch (key) {
        case buffer_size:
            attribute_matches = value <= 32;
            break;
        case alpha_size:
        case blue_size:
        case green_size:
        case red_size:
            attribute_matches = value <= 8;
            break;
        case depth_size:
            attribute_matches = static_cast<uint32_t>(value) <= surface.depth_bits;
            break;
        case stencil_size:
            attribute_matches = value == 0;
            break;
        case sample_buffers:
            attribute_matches = value <= (surface.samples > 1 ? 1 : 0);
            break;
        case samples:
            attribute_matches = static_cast<uint32_t>(value) <= surface.samples;
            break;
        case config_id_attribute:
            attribute_matches = static_cast<Handle>(value) == config_id;
            break;
        case surface_type:
            attribute_matches = (value & surface_types) == value;
            break;
        case renderable_type:
            attribute_matches = (value & es_bits) == value;
            break;
        default:
            return fail(bad_attribute);
        }
        matches = matches && attribute_matches;
    }
    match = matches ? config_id : 0;
    return true;
}

Handle create_window_surface(Handle display, Handle config, uint64_t window,
                             Attributes attributes) {
    if (!valid_config(display, config)) {
        return 0;
    }
    if (!window) {
        return fail(bad_native_window);
    }
    if (!attributes.empty()) {
        return fail(bad_attribute);
    }
    if (state().has_surface) {
        return fail(bad_alloc);
    }
    state().has_surface = true;
    return surface_id;
}

Handle create_pbuffer_surface(Handle display, Handle config, Attributes attributes) {
    if (!valid_config(display, config)) {
        return 0;
    }
    if (state().has_surface) {
        return fail(bad_alloc);
    }
    const auto& surface = *gfx::surface_config();
    int32_t width = surface.width;
    int32_t height = surface.height;
    for (size_t i = 0; i < attributes.size(); i += 2) {
        switch (attributes[i]) {
        case width_attribute:
            width = attributes[i + 1];
            break;
        case height_attribute:
            height = attributes[i + 1];
            break;
        default:
            return fail(bad_attribute);
        }
    }
    if (width <= 0 || height <= 0) {
        return fail(bad_parameter);
    }
    if (wa_gfx_resize(width, height) != WA_OK) {
        return fail(bad_alloc);
    }
    state().has_surface = true;
    return surface_id;
}

Handle create_context(Handle display, Handle config, Handle share, Attributes attributes) {
    if (!valid_config(display, config)) {
        return 0;
    }
    if (share || state().has_context) {
        return fail(bad_match);
    }
    int32_t major = 1;
    int32_t minor = 0;
    for (size_t i = 0; i < attributes.size(); i += 2) {
        switch (attributes[i]) {
        case context_major_version:
            major = attributes[i + 1];
            break;
        case context_minor_version:
            minor = attributes[i + 1];
            break;
        default:
            return fail(bad_attribute);
        }
    }
    if ((major != 2 && major != 3) || minor != 0) {
        return fail(bad_match);
    }
    state().has_context = true;
    state().context_version = major;
    return context_id;
}

bool make_current(Handle display, Handle draw, Handle read, Handle context) {
    if (!valid_display(display)) {
        return false;
    }
    if (context || draw || read) {
        if (!valid_context(context) || !valid_surface(draw) || !valid_surface(read)) {
            return false;
        }
    }
    if (gfx::make_current(context != 0) != WA_OK) {
        return fail(bad_match);
    }
    state().current = context != 0;
    return true;
}

Handle get_current_display() { return state().current ? display_id : 0; }

Handle get_current_context() { return state().current ? context_id : 0; }

Handle get_current_surface(int32_t which) {
    if (which != 0x3059 && which != 0x305a) { // EGL_DRAW and EGL_READ.
        fail(bad_parameter);
        return 0;
    }
    return state().current ? surface_id : 0;
}

bool swap_buffers(Handle display, Handle surface) {
    if (!valid_display(display) || !valid_surface(surface)) {
        return false;
    }
    return (state().current && wa_gfx_swap_buffers() == WA_OK) || fail(bad_surface);
}

bool query_surface(Handle display, Handle surface, int32_t attribute, int32_t& value) {
    if (!valid_display(display) || !valid_surface(surface)) {
        return false;
    }
    const auto& config = *gfx::surface_config();
    switch (attribute) {
    case width_attribute:
        value = config.width;
        return true;
    case height_attribute:
        value = config.height;
        return true;
    case config_id_attribute:
        value = config_id;
        return true;
    default:
        return fail(bad_attribute);
    }
}

bool query_context(Handle display, Handle context, int32_t attribute, int32_t& value) {
    if (!valid_display(display) || !valid_context(context)) return false;
    switch (attribute) {
    case config_id_attribute: value = config_id; return true;
    case context_major_version: value = state().context_version; return true;
    case 0x3097: value = 0x30a0; return true; // EGL_CONTEXT_CLIENT_TYPE: EGL_OPENGL_ES_API.
    case 0x3086: value = state().current ? 0x3084 : none; return true; // EGL_RENDER_BUFFER.
    default: return fail(bad_attribute);
    }
}

bool release_thread() {
    if (state().current && !make_current(display_id, 0, 0, 0)) return false;
    state().error = success;
    return true;
}

bool destroy_context(Handle display, Handle context) {
    if (!valid_display(display) || !valid_context(context)) {
        return false;
    }
    if (state().current && !make_current(display, 0, 0, 0)) {
        return false;
    }
    state().has_context = false;
    return true;
}

bool destroy_surface(Handle display, Handle surface) {
    if (!valid_display(display) || !valid_surface(surface)) {
        return false;
    }
    if (state().current && !make_current(display, 0, 0, 0)) {
        return false;
    }
    state().has_surface = false;
    return true;
}

bool terminate(Handle display) {
    if (display != display_id) {
        return fail(bad_display);
    }
    if (state().current && !make_current(display, 0, 0, 0)) {
        return false;
    }
    state() = {.generation = gfx::surface_generation()};
    return true;
}

} // namespace wa::egl

#ifdef __EMSCRIPTEN__
namespace {

namespace egl = wa::egl;
static_assert(egl::none == EGL_NONE && egl::bad_parameter == EGL_BAD_PARAMETER);

egl::Handle handle(const void* value) {
    return reinterpret_cast<uintptr_t>(value);
}

void* handle(egl::Handle value) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(value));
}

egl::Attributes attributes(const EGLint* list) {
    size_t length = 0;
    if (list) {
        while (list[length] != EGL_NONE) {
            length += 2;
        }
    }
    return {list, length};
}

} // namespace

extern "C" {

EGLDisplay eglGetDisplay(EGLNativeDisplayType display) {
    return handle(egl::get_display(reinterpret_cast<uintptr_t>(display)));
}

EGLBoolean eglInitialize(EGLDisplay display, EGLint* major, EGLint* minor) {
    return egl::initialize(handle(display), major, minor);
}

EGLBoolean eglChooseConfig(EGLDisplay display, const EGLint* list, EGLConfig* configs,
                           EGLint capacity, EGLint* count) {
    if (!count || capacity < 0) {
        return egl::fail(EGL_BAD_PARAMETER);
    }
    egl::Handle match;
    if (!egl::choose_config(handle(display), attributes(list), match)) {
        return EGL_FALSE;
    }
    *count = match != 0 && (!configs || capacity > 0);
    if (configs && *count) {
        configs[0] = handle(match);
    }
    return EGL_TRUE;
}

EGLSurface eglCreateWindowSurface(EGLDisplay display, EGLConfig config, EGLNativeWindowType window,
                                  const EGLint* list) {
    return handle(
        egl::create_window_surface(handle(display), handle(config), window, attributes(list)));
}

EGLSurface eglCreatePbufferSurface(EGLDisplay display, EGLConfig config, const EGLint* list) {
    return handle(egl::create_pbuffer_surface(handle(display), handle(config), attributes(list)));
}

EGLContext eglCreateContext(EGLDisplay display, EGLConfig config, EGLContext share,
                            const EGLint* list) {
    return handle(
        egl::create_context(handle(display), handle(config), handle(share), attributes(list)));
}

EGLBoolean eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read,
                          EGLContext context) {
    return egl::make_current(handle(display), handle(draw), handle(read), handle(context));
}

EGLBoolean eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
    return egl::swap_buffers(handle(display), handle(surface));
}

EGLBoolean eglQuerySurface(EGLDisplay display, EGLSurface surface, EGLint attribute,
                           EGLint* value) {
    if (!value) {
        return egl::fail(EGL_BAD_PARAMETER);
    }
    return egl::query_surface(handle(display), handle(surface), attribute, *value);
}

EGLBoolean eglDestroyContext(EGLDisplay display, EGLContext context) {
    return egl::destroy_context(handle(display), handle(context));
}

EGLBoolean eglDestroySurface(EGLDisplay display, EGLSurface surface) {
    return egl::destroy_surface(handle(display), handle(surface));
}

EGLBoolean eglTerminate(EGLDisplay display) {
    return egl::terminate(handle(display));
}

EGLint eglGetError() {
    return egl::get_error();
}

} // extern "C"
#endif
