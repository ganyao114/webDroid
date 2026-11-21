#pragma once

#include <EGL/egl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Optional borrowed window for Android callers. A null window selects an EGL pbuffer.
void demo_set_native_window(EGLNativeWindowType window);
int demo_init(int width, int height);
// Touch coordinates are normalized to [0, 1]. The return value checksums a pixel sample.
int demo_render_frame(float time_sec, float touch_x, float touch_y);
// Generate 48 kHz PCM starting at phase_start radians. Return frames pushed or generated.
int demo_generate_audio(float* out_stereo, int frames, float phase_start);
void demo_shutdown(void);

#ifdef __cplusplus
}
#endif
