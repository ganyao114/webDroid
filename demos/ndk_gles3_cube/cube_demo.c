#include "cube_demo.h"

#include <GLES3/gl3.h>
#include <android/log.h>
#include <arm_neon.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define DEMO_API __attribute__((visibility("default")))
#define LOG_TAG "NDKGLES3Cube"

// The browser linker supplies this optional extension. The library can also run under
// Android's platform loader, where the caller receives the generated PCM in out_stereo.
extern int wa_ndk_audio_push_stereo_f32(const float* out_stereo, int frames) __attribute__((weak));

typedef struct {
    float position[3];
    float normal[3];
    float uv[2];
} Vertex;

static const Vertex vertices[] = {
    {{-1, -1, +1}, {0, 0, +1}, {0, 0}}, {{+1, -1, +1}, {0, 0, +1}, {1, 0}},
    {{+1, +1, +1}, {0, 0, +1}, {1, 1}}, {{-1, +1, +1}, {0, 0, +1}, {0, 1}},
    {{+1, -1, -1}, {0, 0, -1}, {0, 0}}, {{-1, -1, -1}, {0, 0, -1}, {1, 0}},
    {{-1, +1, -1}, {0, 0, -1}, {1, 1}}, {{+1, +1, -1}, {0, 0, -1}, {0, 1}},
    {{+1, -1, +1}, {+1, 0, 0}, {0, 0}}, {{+1, -1, -1}, {+1, 0, 0}, {1, 0}},
    {{+1, +1, -1}, {+1, 0, 0}, {1, 1}}, {{+1, +1, +1}, {+1, 0, 0}, {0, 1}},
    {{-1, -1, -1}, {-1, 0, 0}, {0, 0}}, {{-1, -1, +1}, {-1, 0, 0}, {1, 0}},
    {{-1, +1, +1}, {-1, 0, 0}, {1, 1}}, {{-1, +1, -1}, {-1, 0, 0}, {0, 1}},
    {{-1, +1, +1}, {0, +1, 0}, {0, 0}}, {{+1, +1, +1}, {0, +1, 0}, {1, 0}},
    {{+1, +1, -1}, {0, +1, 0}, {1, 1}}, {{-1, +1, -1}, {0, +1, 0}, {0, 1}},
    {{-1, -1, -1}, {0, -1, 0}, {0, 0}}, {{+1, -1, -1}, {0, -1, 0}, {1, 0}},
    {{+1, -1, +1}, {0, -1, 0}, {1, 1}}, {{-1, -1, +1}, {0, -1, 0}, {0, 1}},
};

static const GLushort indices[] = {
    0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
    12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
};

static const char vertex_shader[] = "#version 300 es\n"
                                    "layout(location = 0) in vec3 a_position;\n"
                                    "layout(location = 1) in vec3 a_normal;\n"
                                    "layout(location = 2) in vec2 a_uv;\n"
                                    "uniform mat4 u_mvp;\n"
                                    "uniform mat4 u_model;\n"
                                    "out vec3 v_normal;\n"
                                    "out vec3 v_tint;\n"
                                    "out vec2 v_uv;\n"
                                    "void main() {\n"
                                    "    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
                                    "    v_normal = mat3(u_model) * a_normal;\n"
                                    "    v_tint = vec3(0.72) + 0.28 * abs(a_normal);\n"
                                    "    v_uv = a_uv;\n"
                                    "}\n";

static const char fragment_shader[] =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec3 v_normal;\n"
    "in vec3 v_tint;\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_checker;\n"
    "out vec4 out_color;\n"
    "void main() {\n"
    "    vec3 light = normalize(vec3(0.65, 0.85, 1.0));\n"
    "    float diffuse = max(dot(normalize(v_normal), light), 0.0);\n"
    "    vec3 checker = texture(u_checker, v_uv).rgb;\n"
    "    out_color = vec4(checker * v_tint * (0.30 + 0.70 * diffuse), 1.0);\n"
    "}\n";

static EGLNativeWindowType native_window;
static EGLDisplay display = EGL_NO_DISPLAY;
static EGLSurface surface = EGL_NO_SURFACE;
static EGLContext context = EGL_NO_CONTEXT;
static GLuint program;
static GLuint vao;
static GLuint vbo;
static GLuint ebo;
static GLuint checker;
static GLint mvp_location;
static GLint model_location;

static GLuint compile_shader(GLenum type, const char* text) {
    const GLuint shader = glCreateShader(type);
    if (!shader) {
        return 0;
    }
    glShaderSource(shader, 1, &text, NULL);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char message[1024] = {0};
        glGetShaderInfoLog(shader, sizeof(message), NULL, message);
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "Shader compilation failed: %s", message);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program(void) {
    const GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_shader);
    if (!vertex) {
        return 0;
    }
    const GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_shader);
    const GLuint result = fragment ? glCreateProgram() : 0;
    GLint linked = GL_FALSE;
    if (result) {
        glAttachShader(result, vertex);
        glAttachShader(result, fragment);
        glLinkProgram(result);
        glGetProgramiv(result, GL_LINK_STATUS, &linked);
        if (!linked) {
            char message[1024] = {0};
            glGetProgramInfoLog(result, sizeof(message), NULL, message);
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "Program linking failed: %s", message);
            glDeleteProgram(result);
        }
    }
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return linked ? result : 0;
}

static void upload_checkerboard(void) {
    enum { SIDE = 64 };
    uint8_t rgba[SIDE][SIDE][4];
    for (int y = 0; y < SIDE; ++y) {
        for (int x = 0; x < SIDE; ++x) {
            const int light = ((x / 8) ^ (y / 8)) & 1;
            rgba[y][x][0] = light ? 255 : 32;
            rgba[y][x][1] = light ? 160 : 145;
            rgba[y][x][2] = light ? 45 : 255;
            rgba[y][x][3] = 255;
        }
    }

    glGenTextures(1, &checker);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, checker);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SIDE, SIDE, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

DEMO_API void demo_set_native_window(EGLNativeWindowType window) {
    native_window = window;
}

DEMO_API void demo_shutdown(void) {
    if (display == EGL_NO_DISPLAY) {
        return;
    }
    if (program) {
        glDeleteProgram(program);
        glDeleteTextures(1, &checker);
        glDeleteBuffers(1, &ebo);
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        program = checker = ebo = vbo = vao = 0;
    }
    if (context != EGL_NO_CONTEXT) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        context = EGL_NO_CONTEXT;
    }
    if (surface != EGL_NO_SURFACE) {
        eglDestroySurface(display, surface);
        surface = EGL_NO_SURFACE;
    }
    eglTerminate(display);
    display = EGL_NO_DISPLAY;
}

DEMO_API int demo_init(int width, int height) {
    if (width <= 0 || height <= 0 || display != EGL_NO_DISPLAY) {
        return -1;
    }
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE,
        native_window ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_ALPHA_SIZE,
        8,
        EGL_DEPTH_SIZE,
        16,
        EGL_NONE,
    };
    const EGLint surface_attributes[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL) ||
        !eglChooseConfig(display, config_attributes, &config, 1, &count) || count == 0) {
        goto egl_error;
    }
    surface = native_window ? eglCreateWindowSurface(display, config, native_window, NULL)
                            : eglCreatePbufferSurface(display, config, surface_attributes);
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(display, surface, surface, context)) {
        goto egl_error;
    }

    program = link_program();
    if (!program) {
        demo_shutdown();
        return -2;
    }
    glUseProgram(program);
    mvp_location = glGetUniformLocation(program, "u_mvp");
    model_location = glGetUniformLocation(program, "u_model");
    glUniform1i(glGetUniformLocation(program, "u_checker"), 0);

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void*)offsetof(Vertex, position));
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void*)offsetof(Vertex, normal));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    upload_checkerboard();

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "GLES setup failed with error 0x%x", error);
        demo_shutdown();
        return -3;
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Initialized GLES3 cube at %dx%d", width,
                        height);
    return 0;

egl_error:
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "EGL setup failed with error 0x%x",
                        eglGetError());
    demo_shutdown();
    return -1;
}

// Column-major matrix product. Each NEON vector represents a column of the result.
static void mat_multiply(float out[16], const float left[16], const float right[16]) {
    const float32x4_t l0 = vld1q_f32(left);
    const float32x4_t l1 = vld1q_f32(left + 4);
    const float32x4_t l2 = vld1q_f32(left + 8);
    const float32x4_t l3 = vld1q_f32(left + 12);
    for (int column = 0; column < 4; ++column) {
        const float* r = right + 4 * column;
        float32x4_t value = vmulq_f32(l0, vdupq_n_f32(r[0]));
        value = vmlaq_f32(value, l1, vdupq_n_f32(r[1]));
        value = vmlaq_f32(value, l2, vdupq_n_f32(r[2]));
        value = vmlaq_f32(value, l3, vdupq_n_f32(r[3]));
        vst1q_f32(out + 4 * column, value);
    }
}

DEMO_API int demo_render_frame(float time_sec, float touch_x, float touch_y) {
    EGLint width;
    EGLint height;
    if (!program || !eglQuerySurface(display, surface, EGL_WIDTH, &width) ||
        !eglQuerySurface(display, surface, EGL_HEIGHT, &height) || width <= 0 || height <= 0) {
        return -1;
    }

    const float pitch = 0.36f + time_sec * 0.67f + (touch_y - 0.5f) * 1.4f;
    const float yaw = 0.49f + time_sec * 1.25f + (touch_x - 0.5f) * 1.8f;
    const float cx = cosf(pitch);
    const float sx = sinf(pitch);
    const float cy = cosf(yaw);
    const float sy = sinf(yaw);
    const float rotate_x[] = {1, 0, 0, 0, 0, cx, sx, 0, 0, -sx, cx, 0, 0, 0, 0, 1};
    const float rotate_y[] = {cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1};
    const float scale = 1.0f / tanf(0.50f);
    const float near = 0.1f;
    const float far = 50.0f;
    const float projection[] = {
        scale * (float)height / (float)width,
        0,
        0,
        0,
        0,
        scale,
        0,
        0,
        0,
        0,
        -(far + near) / (far - near),
        -1,
        0,
        0,
        -(2 * far * near) / (far - near),
        0,
    };
    float model[16];
    float mvp[16];
    mat_multiply(model, rotate_y, rotate_x);
    model[14] = -4.6f;
    mat_multiply(mvp, projection, model);

    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(program);
    glBindVertexArray(vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, checker);
    glUniformMatrix4fv(mvp_location, 1, GL_FALSE, mvp);
    glUniformMatrix4fv(model_location, 1, GL_FALSE, model);
    glDrawElements(GL_TRIANGLES, sizeof(indices) / sizeof(indices[0]), GL_UNSIGNED_SHORT, NULL);

    uint8_t sample[8 * 8 * 4];
    const GLsizei sample_width = width < 8 ? width : 8;
    const GLsizei sample_height = height < 8 ? height : 8;
    glReadPixels((width - sample_width) / 2, (height - sample_height) / 2, sample_width,
                 sample_height, GL_RGBA, GL_UNSIGNED_BYTE, sample);
    if (glGetError() != GL_NO_ERROR || !eglSwapBuffers(display, surface)) {
        return -2;
    }
    uint32_t checksum = 2166136261u;
    for (int i = 0; i < sample_width * sample_height * 4; ++i) {
        checksum = (checksum ^ sample[i]) * 16777619u;
    }
    return (int)(checksum & 0x7fffffffu);
}

DEMO_API int demo_generate_audio(float* out_stereo, int frames, float phase_start) {
    if (frames < 0 || (frames && !out_stereo)) {
        return -1;
    }
    const float step = 6.28318530718f * 220.0f / 48000.0f;
    for (int frame = 0; frame < frames; ++frame) {
        const float phase = phase_start + (float)frame * step;
        const float root = sinf(phase);
        const float third = sinf(phase * 1.25992105f);
        const float fifth = sinf(phase * 1.49830708f);
        out_stereo[2 * (size_t)frame] = 0.20f * root + 0.09f * third + 0.06f * fifth;
        out_stereo[2 * (size_t)frame + 1] = 0.15f * root + 0.06f * third + 0.10f * fifth;
    }
    if (wa_ndk_audio_push_stereo_f32) {
        return wa_ndk_audio_push_stereo_f32(out_stereo, frames);
    }
    return frames;
}
