#include "cube_demo.h"
#include "gles_host_bridge.h"
#include "wasmandroid/hal_gfx.h"

#include <GLES3/gl3.h>
#include <emscripten/emscripten.h>

extern "C" {

EMSCRIPTEN_KEEPALIVE int hal_selftest_init(int width, int height) {
    if (width <= 0 || height <= 0) {
        return WA_ERR_INVALID;
    }
    const wa_gfx_surface_config_t config{static_cast<uint32_t>(width),
                                         static_cast<uint32_t>(height), 24, 0};
    const int32_t status = wa_gfx_init("#canvas", &config);
    if (status != WA_OK) {
        return status;
    }
    demo_set_native_window(1); // The EGL shim maps the native window token to Module.canvas.
    const int result = demo_init(width, height);
    if (result != 0) {
        wa_gfx_shutdown();
        return result;
    }
    return WA_OK;
}

EMSCRIPTEN_KEEPALIVE int hal_selftest_depth_bits() {
    GLint bits = 0;
    glGetIntegerv(GL_DEPTH_BITS, &bits);
    return glIsEnabled(GL_DEPTH_TEST) ? bits : 0;
}

EMSCRIPTEN_KEEPALIVE int hal_selftest_audio_init(void* buffer, size_t bytes, uint32_t capacity) {
    const int32_t status = wa_audio_ring_init(buffer, bytes, capacity, WA_AUDIO_SAMPLE_RATE);
    return status == WA_OK ? wa_hal_set_audio_ring(buffer) : status;
}

EMSCRIPTEN_KEEPALIVE void hal_selftest_shutdown() {
    wa_hal_set_audio_ring(nullptr);
    demo_shutdown();
    wa_gfx_shutdown();
}

} // extern "C"
