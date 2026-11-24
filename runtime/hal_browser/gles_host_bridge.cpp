#include "gles_host_bridge.h"

#include "bionic_internal.h"
#include "elf_image.h"
#include "egl_shim.h"
#include "gfx_surface.h"
#include "guest_call.h"

#include <array>
#include <tuple>
#include <type_traits>

#if defined(__EMSCRIPTEN__) || defined(WA_HAL_NATIVE_GLES3)
#include <GLES3/gl3.h>
#endif

namespace {

using wa::GuestCall;
namespace egl = wa::egl;

template <void (*Function)(GuestCall&), bool needs_surface = false>
int32_t invoke(wa_host_call_ctx_t* context, void*) {
    if (!context || (context->arch != WA_ARCH_ARM32 && context->arch != WA_ARCH_ARM64)) {
        return WA_ERR_INVALID;
    }
    GuestCall call(*context);
    context->ret0 = 0;
    context->ret1 = 0;
    context->fret0 = 0;
    context->has_float_ret = 0;
    if constexpr (needs_surface) {
        if (!wa::gfx::has_current_surface()) {
            return WA_ERR_INVALID;
        }
    }
    Function(call);
    return call.status();
}

template <typename T> T scalar(GuestCall& call, size_t& word, size_t& fp) {
    if constexpr (std::is_same_v<T, float>) {
        return call.arm32() ? std::bit_cast<float>(static_cast<uint32_t>(call.word(word++)))
                            : static_cast<float>(call.fp(fp++));
    } else {
        return static_cast<T>(call.word(word++));
    }
}

template <auto Function> struct Scalars;

template <typename Result, typename... Args, Result (*Function)(Args...)> struct Scalars<Function> {
    static void run(GuestCall& call) {
        size_t word = 0;
        size_t fp = 0;
        // Braced initialization consumes arguments in order, including spilled ARM32 floats.
        const std::tuple<Args...> args{scalar<Args>(call, word, fp)...};
        if (!call.ok()) {
            return;
        }
        if constexpr (std::is_void_v<Result>) {
            std::apply(Function, args);
        } else {
            call.result(std::apply(Function, args));
        }
    }
};

struct AttributeList {
    std::array<int32_t, 2 * egl::max_attribute_pairs> values;
    size_t size = 0;

    bool load(GuestCall& call, uint64_t address) {
        if (!address) {
            return true;
        }
        while (call.ok()) {
            int32_t key;
            if (!call.read(address, key)) {
                return false;
            }
            if (key == egl::none) {
                return true;
            }
            if (size == values.size()) {
                return call.fail(WA_ERR_OVERFLOW);
            }
            values[size++] = key;
            if (!call.advance(address, sizeof(key)) || !call.read(address, values[size++]) ||
                !call.advance(address, sizeof(key))) {
                return false;
            }
        }
        return false;
    }

    egl::Attributes span() const { return {values.data(), size}; }
};

egl::Handle egl_handle(GuestCall& call, size_t index) {
    const uint64_t value = call.word(index);
    return value <= UINT32_MAX ? static_cast<egl::Handle>(value) : UINT32_MAX;
}

void egl_initialize(GuestCall& call) {
    int32_t major;
    int32_t minor;
    if (!egl::initialize(egl_handle(call, 0), &major, &minor)) {
        return;
    }
    const uint64_t major_out = call.word(1);
    const uint64_t minor_out = call.word(2);
    if ((!major_out || call.write(major_out, major)) &&
        (!minor_out || call.write(minor_out, minor))) {
        call.result(1);
    }
}

void egl_choose_config(GuestCall& call) {
    const uint64_t configs = call.word(2);
    const int32_t capacity = call.i32(3);
    const uint64_t count_out = call.word(4);
    if (!call.ok()) {
        return;
    }
    if (!count_out || capacity < 0) {
        egl::fail(egl::bad_parameter);
        return;
    }
    AttributeList attrs;
    egl::Handle match = 0;
    if (attrs.load(call, call.word(1)) &&
        egl::choose_config(egl_handle(call, 0), attrs.span(), match)) {
        const int32_t count = match && (!configs || capacity > 0);
        if ((!configs || !count || call.put_pointer(configs, match)) &&
            call.write(count_out, count)) {
            call.result(1);
        }
    }
}

void egl_create_window_surface(GuestCall& call) {
    AttributeList attrs;
    if (attrs.load(call, call.word(3))) {
        call.result(egl::create_window_surface(egl_handle(call, 0), egl_handle(call, 1),
                                               call.word(2), attrs.span()));
    }
}

void egl_create_pbuffer_surface(GuestCall& call) {
    AttributeList attrs;
    if (attrs.load(call, call.word(2))) {
        call.result(
            egl::create_pbuffer_surface(egl_handle(call, 0), egl_handle(call, 1), attrs.span()));
    }
}

void egl_create_context(GuestCall& call) {
    AttributeList attrs;
    if (attrs.load(call, call.word(3))) {
        call.result(egl::create_context(egl_handle(call, 0), egl_handle(call, 1),
                                        egl_handle(call, 2), attrs.span()));
    }
}

void egl_query_surface(GuestCall& call) {
    const uint64_t output = call.word(3);
    if (!output) {
        egl::fail(egl::bad_parameter);
        return;
    }
    int32_t value;
    if (egl::query_surface(egl_handle(call, 0), egl_handle(call, 1), call.i32(2), value) &&
        call.write(output, value)) {
        call.result(1);
    }
}

void egl_bind_api(GuestCall& call) {
    if (call.word(0) == 0x30a0) call.result(1);
    else egl::fail(egl::bad_parameter);
}

void egl_query_string(GuestCall& call) {
    if (const auto* value = egl::query_string(egl_handle(call, 0), call.i32(1)))
        call.result(wa::dbt::InternString(*call.context().dbt, value));
}

void egl_get_proc_address(GuestCall& call) {
    std::string name;
    if (!call.string(call.word(0), name, 256)) return;
    const auto& dbt = *call.context().dbt;
    for (const auto* library : {"libGLESv2.so", "libGLESv3.so", "libEGL.so"}) {
        if (const auto address = wa::dbt::FindVirtualSymbol(dbt, library, name)) {
            call.result(address);
            return;
        }
    }
}

void audio_push(GuestCall& call) {
    const uint64_t address = call.word(0);
    const int32_t frames = call.i32(1);
    if (frames < 0 || (frames && !address)) {
        call.result(WA_ERR_INVALID);
        return;
    }
    constexpr size_t samples_per_frame = 2;
    const size_t samples = static_cast<size_t>(frames) * samples_per_frame;
    const auto pcm = call.buffer<float>(samples);
    if (call.ok() && call.copy_in(address, pcm.get(), samples * sizeof(float))) {
        call.result(wa_ndk_audio_push_stereo_f32(pcm.get(), frames));
    }
}

#if defined(__EMSCRIPTEN__) || defined(WA_HAL_NATIVE_GLES3)

constexpr size_t shader_string_limit = 16 * 1024 * 1024;

void shader_source(GuestCall& call) {
    const GLuint shader = call.word(0);
    const GLsizei count = call.i32(1);
    uint64_t strings = call.word(2);
    uint64_t lengths = call.word(3);
    if (count <= 0) {
        glShaderSource(shader, count, nullptr, nullptr);
        return;
    }
    const auto pointers = call.buffer<const GLchar*>(count);
    const auto sources = call.buffer<std::string>(count);
    const auto explicit_lengths = call.buffer<GLint>(lengths ? count : 0);
    for (GLsizei i = 0; i < count && call.ok(); ++i) {
        const uint64_t address = call.pointer(strings);
        GLint length = -1;
        if (lengths && !call.read(lengths, length)) {
            return;
        }
        if (length < 0) {
            if (!call.string(address, sources[i], shader_string_limit)) {
                return;
            }
        } else {
            if (static_cast<size_t>(length) > shader_string_limit) {
                call.fail(WA_ERR_OVERFLOW);
                return;
            }
            sources[i].resize(length);
            if (!call.copy_in(address, sources[i].data(), length)) {
                return;
            }
        }
        pointers[i] = sources[i].c_str();
        if (explicit_lengths) {
            explicit_lengths[i] = length;
        }
        if (i + 1 < count && (!call.advance(strings, call.pointer_size()) ||
                              (lengths && !call.advance(lengths, sizeof(GLint))))) {
            return;
        }
    }
    if (call.ok()) {
        glShaderSource(shader, count, pointers.get(), explicit_lengths.get());
    }
}

template <auto Query> void query_object(GuestCall& call) {
    GLint value = 0;
    Query(call.word(0), call.word(1), &value);
    call.write(call.word(2), value);
}

template <auto Query> void info_log(GuestCall& call) {
    const GLuint object = call.word(0);
    const GLsizei capacity = call.i32(1);
    const uint64_t length_out = call.word(2);
    const uint64_t message_out = call.word(3);
    if (capacity < 0 || (capacity && !message_out)) {
        call.fail(WA_ERR_INVALID);
        return;
    }
    const auto message = call.buffer<GLchar>(capacity);
    GLsizei length = 0;
    if (call.ok()) {
        if (message) {
            message[0] = '\0';
        }
        Query(object, capacity, &length, message.get());
        if (message_out && capacity) {
            call.copy_out(message_out, message.get(), static_cast<size_t>(length) + 1);
        }
        if (length_out) {
            call.write(length_out, length);
        }
    }
}

template <auto Generate> void gen_objects(GuestCall& call) {
    const GLsizei count = call.i32(0);
    const uint64_t output = call.word(1);
    if (count <= 0) {
        Generate(count, nullptr);
        return;
    }
    if (!output) {
        call.fail(WA_ERR_FAULT);
        return;
    }
    const auto objects = call.buffer<GLuint>(count);
    if (call.ok()) {
        Generate(count, objects.get());
        call.copy_out(output, objects.get(), static_cast<size_t>(count) * sizeof(GLuint));
    }
}

template <auto Delete> void delete_objects(GuestCall& call) {
    const GLsizei count = call.i32(0);
    if (count <= 0) {
        Delete(count, nullptr);
        return;
    }
    const auto objects = call.buffer<GLuint>(count);
    if (call.ok() && call.copy_in(call.word(1), objects.get(), size_t(count) * sizeof(GLuint))) {
        Delete(count, objects.get());
    }
}

void buffer_data(GuestCall& call) {
    const GLenum target = call.word(0);
    const int64_t size = call.signed_word(1);
    const uint64_t address = call.word(2);
    const GLenum usage = call.word(3);
    if (size < 0 || size > std::numeric_limits<GLsizeiptr>::max()) {
        call.fail(size < 0 ? WA_ERR_INVALID : WA_ERR_OVERFLOW);
        return;
    }
    const auto data = call.buffer<uint8_t>(address ? size : 0);
    if (call.ok() && (!address || call.copy_in(address, data.get(), size))) {
        glBufferData(target, size, data.get(), usage);
    }
}

const void* buffer_offset(GuestCall& call, uint64_t value) {
    if (value > UINTPTR_MAX) {
        call.fail(WA_ERR_OVERFLOW);
        return nullptr;
    }
    return reinterpret_cast<const void*>(static_cast<uintptr_t>(value));
}

void vertex_attrib_pointer(GuestCall& call) {
    const GLuint index = call.word(0);
    const GLint size = call.i32(1);
    const GLenum type = call.word(2);
    const GLboolean normalized = call.word(3);
    const GLsizei stride = call.i32(4);
    const void* offset = buffer_offset(call, call.word(5));
    if (!call.ok()) {
        return;
    }
    GLint buffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
    if (buffer) {
        glVertexAttribPointer(index, size, type, normalized, stride, offset);
    } else {
        call.fail(WA_ERR_NOTSUP);
    }
}

void draw_elements(GuestCall& call) {
    const auto* offset = buffer_offset(call, call.word(3));
    if (!call.ok()) {
        return;
    }
    GLint buffer = 0;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &buffer);
    if (buffer) {
        glDrawElements(call.word(0), call.i32(1), call.word(2), offset);
    } else {
        call.fail(WA_ERR_NOTSUP);
    }
}

template<auto Find> void get_location(GuestCall& call) {
    std::string name;
    if (call.string(call.word(1), name, shader_string_limit)) {
        call.result(Find(call.word(0), name.c_str()));
    }
}

template <size_t Width, auto Uniform> void uniform_matrix(GuestCall& call) {
    const GLint location = call.i32(0);
    const GLsizei count = call.i32(1);
    const GLboolean transpose = call.word(2);
    const uint64_t address = call.word(3);
    if (count < 0 || static_cast<size_t>(count) > SIZE_MAX / (Width * sizeof(GLfloat))) {
        call.fail(count < 0 ? WA_ERR_INVALID : WA_ERR_OVERFLOW);
        return;
    }
    const size_t elements = size_t(count) * Width;
    const auto values = call.buffer<GLfloat>(elements);
    if (call.ok() && call.copy_in(address, values.get(), elements * sizeof(GLfloat))) {
        Uniform(location, count, transpose, values.get());
    }
}

void uniform_vector(GuestCall& call) {
    const auto location = call.i32(0), count = call.i32(1);
    const auto address = call.word(2);
    if (count < 0 || static_cast<size_t>(count) > SIZE_MAX / (4 * sizeof(GLfloat))) {
        call.fail(count < 0 ? WA_ERR_INVALID : WA_ERR_OVERFLOW);
        return;
    }
    const size_t elements = size_t(count) * 4;
    const auto values = call.buffer<GLfloat>(elements);
    if (call.ok() && call.copy_in(address, values.get(), elements * sizeof(GLfloat)))
        glUniform4fv(location, count, values.get());
}

void gl_get_string(GuestCall& call) {
    const auto* value = glGetString(call.word(0));
    if (value) call.result(wa::dbt::InternString(*call.context().dbt,
                                                 reinterpret_cast<const char*>(value)));
}

void gl_get_integer(GuestCall& call) {
    const auto name = static_cast<GLenum>(call.word(0));
    GLint count = 1;
    switch (name) {
    case GL_VIEWPORT: case GL_SCISSOR_BOX: case GL_COLOR_WRITEMASK:
    case GL_COLOR_CLEAR_VALUE: case GL_BLEND_COLOR: count = 4; break;
    case GL_MAX_VIEWPORT_DIMS: case GL_DEPTH_RANGE:
    case GL_ALIASED_POINT_SIZE_RANGE: case GL_ALIASED_LINE_WIDTH_RANGE: count = 2; break;
    case GL_COMPRESSED_TEXTURE_FORMATS:
        glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &count);
        break;
    case GL_SHADER_BINARY_FORMATS:
        glGetIntegerv(GL_NUM_SHADER_BINARY_FORMATS, &count);
        break;
    case GL_PROGRAM_BINARY_FORMATS:
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &count);
        break;
    }
    if (count <= 0 || count > 4096) return;
    const auto values = call.buffer<GLint>(count);
    if (!call.ok()) return;
    std::fill_n(values.get(), count, 0);
    glGetIntegerv(name, values.get());
    call.copy_out(call.word(1), values.get(), count * sizeof(GLint));
}

size_t pixel_size(GLenum format, GLenum type) {
    size_t component;
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        return 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
    case GL_UNSIGNED_INT_24_8:
        return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        return 8;
    case GL_BYTE:
    case GL_UNSIGNED_BYTE:
        component = 1;
        break;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
    case GL_HALF_FLOAT:
        component = 2;
        break;
    case GL_FLOAT:
    case GL_INT:
    case GL_UNSIGNED_INT:
        component = 4;
        break;
    default:
        return 0;
    }
    switch (format) {
    case GL_RED:
    case GL_RED_INTEGER:
    case GL_ALPHA:
    case GL_LUMINANCE:
    case GL_DEPTH_COMPONENT:
        return component;
    case GL_RG:
    case GL_RG_INTEGER:
    case GL_LUMINANCE_ALPHA:
        return component * 2;
    case GL_RGB:
    case GL_RGB_INTEGER:
        return component * 3;
    case GL_RGBA:
    case GL_RGBA_INTEGER:
        return component * 4;
    default:
        return 0;
    }
}

struct PixelLayout {
    size_t bytes;
    size_t start;
    size_t stride;
    size_t row_bytes;
};

PixelLayout pixel_layout(GuestCall& call, GLsizei width, GLsizei height, size_t pixel, bool pack) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    if (!pixel) {
        call.fail(WA_ERR_NOTSUP);
        return {};
    }
    GLint alignment = 4;
    GLint row_length = 0;
    GLint skip_pixels = 0;
    GLint skip_rows = 0;
    glGetIntegerv(pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT, &alignment);
    glGetIntegerv(pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH, &row_length);
    glGetIntegerv(pack ? GL_PACK_SKIP_PIXELS : GL_UNPACK_SKIP_PIXELS, &skip_pixels);
    glGetIntegerv(pack ? GL_PACK_SKIP_ROWS : GL_UNPACK_SKIP_ROWS, &skip_rows);
    if (alignment <= 0 || row_length < 0 || skip_pixels < 0 || skip_rows < 0) {
        call.fail(WA_ERR_INTERNAL);
        return {};
    }
    const uint64_t row = uint64_t(row_length ? row_length : width) * pixel;
    const uint64_t stride = (row + alignment - 1) & ~uint64_t(alignment - 1);
    const uint64_t last_row_bytes = (uint64_t(skip_pixels) + width) * pixel;
    const uint64_t preceding_rows = uint64_t(skip_rows) + height - 1;
    if (last_row_bytes > SIZE_MAX || preceding_rows > (SIZE_MAX - last_row_bytes) / stride) {
        call.fail(WA_ERR_OVERFLOW);
        return {};
    }
    return {static_cast<size_t>(preceding_rows * stride + last_row_bytes),
            static_cast<size_t>(skip_rows * stride + skip_pixels * pixel),
            static_cast<size_t>(stride), static_cast<size_t>(width * pixel)};
}

void tex_image(GuestCall& call) {
    const GLenum target = call.word(0);
    const GLint level = call.i32(1);
    const GLint internal_format = call.i32(2);
    const GLsizei width = call.i32(3);
    const GLsizei height = call.i32(4);
    const GLint border = call.i32(5);
    const GLenum format = call.word(6);
    const GLenum type = call.word(7);
    const uint64_t address = call.word(8);
    if (!call.ok()) {
        return;
    }
    GLint unpack_buffer = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
    if (unpack_buffer) {
        const auto* offset = buffer_offset(call, address);
        if (call.ok()) {
            glTexImage2D(target, level, internal_format, width, height, border, format, type,
                         offset);
        }
        return;
    }
    const size_t bytes =
        address ? pixel_layout(call, width, height, pixel_size(format, type), false).bytes : 0;
    const auto pixels = call.buffer<uint8_t>(bytes);
    if (call.ok() && call.copy_in(address, pixels.get(), bytes)) {
        glTexImage2D(target, level, internal_format, width, height, border, format, type,
                     pixels.get());
    }
}

void tex_sub_image(GuestCall& call) {
    const auto target = static_cast<GLenum>(call.word(0));
    const auto level = call.i32(1), x = call.i32(2), y = call.i32(3);
    const auto width = call.i32(4), height = call.i32(5);
    const auto format = static_cast<GLenum>(call.word(6)), type = static_cast<GLenum>(call.word(7));
    const auto address = call.word(8);
    if (!call.ok()) return;
    GLint buffer = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer);
    if (buffer) {
        const auto* offset = buffer_offset(call, address);
        if (call.ok()) glTexSubImage2D(target, level, x, y, width, height, format, type, offset);
        return;
    }
    const auto size = pixel_layout(call, width, height, pixel_size(format, type), false).bytes;
    const auto pixels = call.buffer<uint8_t>(size);
    if (call.ok() && call.copy_in(address, pixels.get(), size))
        glTexSubImage2D(target, level, x, y, width, height, format, type, pixels.get());
}

void read_pixels(GuestCall& call) {
    const GLint x = call.i32(0);
    const GLint y = call.i32(1);
    const GLsizei width = call.i32(2);
    const GLsizei height = call.i32(3);
    const GLenum format = call.word(4);
    const GLenum type = call.word(5);
    uint64_t address = call.word(6);
    if (!call.ok()) {
        return;
    }
    GLint pack_buffer = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    if (pack_buffer) {
        auto* offset = const_cast<void*>(buffer_offset(call, address));
        if (call.ok()) {
            glReadPixels(x, y, width, height, format, type, offset);
        }
        return;
    }
    if (!address && width > 0 && height > 0) {
        call.fail(WA_ERR_FAULT);
        return;
    }
    const auto layout = pixel_layout(call, width, height, pixel_size(format, type), true);
    const auto pixels = call.buffer<uint8_t>(layout.bytes);
    if (!layout.bytes || !call.ok() || !call.advance(address, layout.start)) {
        return;
    }
    // glReadPixels leaves portions outside the framebuffer unchanged. Initialize staging
    // to zero so a failed or clipped read cannot leak unrelated host heap contents.
    std::memset(pixels.get(), 0, layout.bytes);
    glReadPixels(x, y, width, height, format, type, pixels.get());
    const auto* first_row = pixels.get() + layout.start;
    if (layout.stride == layout.row_bytes) {
        call.copy_out(address, first_row, layout.bytes - layout.start);
        return;
    }
    for (GLsizei row = 0; row < height && call.ok(); ++row) {
        if (!call.copy_out(address, first_row + row * layout.stride, layout.row_bytes)) {
            return;
        }
        if (row + 1 < height && !call.advance(address, layout.stride)) {
            return;
        }
    }
}

#else

void unsupported_gles(GuestCall& call) {
    call.fail(WA_ERR_NOTSUP);
}

#endif

struct Binding {
    const char* name;
    wa_host_symbol_fn function;
};

#if defined(__EMSCRIPTEN__) || defined(WA_HAL_NATIVE_GLES3)
#define GL_SCALAR(name) {#name, invoke<Scalars<name>::run, true>}
#define GL_COPY(name, ...) {#name, invoke<__VA_ARGS__, true>}
#else
#define GL_SCALAR(name) {#name, invoke<unsupported_gles>}
#define GL_COPY(name, ...) GL_SCALAR(name)
#endif

constexpr Binding gl_bindings[] = {
    GL_SCALAR(glViewport),
    GL_SCALAR(glClearColor),
    GL_SCALAR(glClear),
    GL_SCALAR(glBlendFunc),
    GL_SCALAR(glDepthMask),
    GL_SCALAR(glFlush),
    GL_SCALAR(glFinish),
    GL_COPY(glGetString, gl_get_string),
    GL_COPY(glGetIntegerv, gl_get_integer),
    GL_SCALAR(glEnable),
    GL_SCALAR(glDisable),
    GL_SCALAR(glDepthFunc),
    GL_SCALAR(glCullFace),
    GL_SCALAR(glCreateShader),
    GL_COPY(glShaderSource, shader_source),
    GL_SCALAR(glCompileShader),
    GL_COPY(glGetShaderiv, query_object<glGetShaderiv>),
    GL_COPY(glGetShaderInfoLog, info_log<glGetShaderInfoLog>),
    GL_SCALAR(glDeleteShader),
    GL_SCALAR(glCreateProgram),
    GL_SCALAR(glAttachShader),
    GL_SCALAR(glLinkProgram),
    GL_COPY(glGetProgramiv, query_object<glGetProgramiv>),
    GL_COPY(glGetProgramInfoLog, info_log<glGetProgramInfoLog>),
    GL_SCALAR(glUseProgram),
    GL_SCALAR(glDeleteProgram),
    GL_COPY(glGenBuffers, gen_objects<glGenBuffers>),
    GL_SCALAR(glBindBuffer),
    GL_COPY(glBufferData, buffer_data),
    GL_COPY(glDeleteBuffers, delete_objects<glDeleteBuffers>),
    GL_COPY(glGenVertexArrays, gen_objects<glGenVertexArrays>),
    GL_SCALAR(glBindVertexArray),
    GL_COPY(glDeleteVertexArrays, delete_objects<glDeleteVertexArrays>),
    GL_SCALAR(glEnableVertexAttribArray),
    GL_COPY(glVertexAttribPointer, vertex_attrib_pointer),
    GL_COPY(glGetUniformLocation, get_location<glGetUniformLocation>),
    GL_COPY(glGetAttribLocation, get_location<glGetAttribLocation>),
    GL_COPY(glUniformMatrix2fv, uniform_matrix<4, glUniformMatrix2fv>),
    GL_COPY(glUniformMatrix3fv, uniform_matrix<9, glUniformMatrix3fv>),
    GL_COPY(glUniformMatrix4fv, uniform_matrix<16, glUniformMatrix4fv>),
    GL_COPY(glUniform4fv, uniform_vector),
    GL_SCALAR(glUniform1f),
    GL_SCALAR(glUniform1i),
    GL_SCALAR(glUniform2f),
    GL_SCALAR(glUniform3f),
    GL_SCALAR(glUniform4f),
    GL_SCALAR(glDrawArrays),
    GL_COPY(glDrawElements, draw_elements),
    GL_COPY(glGenTextures, gen_objects<glGenTextures>),
    GL_SCALAR(glBindTexture),
    GL_COPY(glTexImage2D, tex_image),
    GL_COPY(glTexSubImage2D, tex_sub_image),
    GL_SCALAR(glTexParameterf),
    GL_SCALAR(glTexParameteri),
    GL_SCALAR(glActiveTexture),
    GL_SCALAR(glPixelStorei),
    GL_COPY(glReadPixels, read_pixels),
    GL_COPY(glDeleteTextures, delete_objects<glDeleteTextures>),
    GL_SCALAR(glGetError),
};

#undef GL_SCALAR
#undef GL_COPY

constexpr Binding egl_bindings[] = {
    {"eglBindAPI", invoke<egl_bind_api>},
    {"eglGetProcAddress", invoke<egl_get_proc_address>},
    {"eglQueryString", invoke<egl_query_string>},
    {"eglGetDisplay", invoke<Scalars<egl::get_display>::run>},
    {"eglInitialize", invoke<egl_initialize>},
    {"eglChooseConfig", invoke<egl_choose_config>},
    {"eglCreateWindowSurface", invoke<egl_create_window_surface>},
    {"eglCreatePbufferSurface", invoke<egl_create_pbuffer_surface>},
    {"eglCreateContext", invoke<egl_create_context>},
    {"eglMakeCurrent", invoke<Scalars<egl::make_current>::run>},
    {"eglSwapBuffers", invoke<Scalars<egl::swap_buffers>::run>},
    {"eglQuerySurface", invoke<egl_query_surface>},
    {"eglDestroyContext", invoke<Scalars<egl::destroy_context>::run>},
    {"eglDestroySurface", invoke<Scalars<egl::destroy_surface>::run>},
    {"eglTerminate", invoke<Scalars<egl::terminate>::run>},
    {"eglGetError", invoke<Scalars<egl::get_error>::run>},
};

constexpr Binding log_bindings[] = {
    {"__android_log_print", invoke<wa::android_log_print>},
    {"__android_log_write", invoke<wa::android_log_write>},
};

constexpr Binding audio_bindings[] = {{"wa_ndk_audio_push_stereo_f32", invoke<audio_push>}};

#if defined(__EMSCRIPTEN__) || defined(WA_HAL_NATIVE_GLES3)
int32_t gles2(wa_host_call_ctx_t* context, void* user) {
    const auto& binding = *static_cast<const Binding*>(user);
    if (wa::gfx::has_current_surface()) return binding.function(context, nullptr);
    const std::string_view name(binding.name);
    if (name == "glGetError") context->ret0 = 0x0502;
    else if (name == "glGetUniformLocation" || name == "glGetAttribLocation") context->ret0 = UINT32_MAX;
    return WA_OK;
}
#endif

} // namespace

int32_t wa_hal_register_graphics_symbols(wa_dbt_t* dbt) {
    for (const auto& binding : egl_bindings) {
        const auto status = wa_dbt_register_symbol(dbt, "libEGL.so", binding.name, binding.function, nullptr);
        if (status != WA_OK) return status;
    }
#if defined(__EMSCRIPTEN__) || defined(WA_HAL_NATIVE_GLES3)
    for (const auto& binding : gl_bindings) {
        const auto status = wa_dbt_register_symbol(dbt, "libGLESv2.so", binding.name, gles2,
                                                   const_cast<Binding*>(&binding));
        if (status != WA_OK) return status;
    }
#endif
    return WA_OK;
}

int32_t wa_hal_register_ndk_symbols(wa_dbt_t* dbt) {
    if (!dbt) {
        return WA_ERR_INVALID;
    }
    const struct {
        const char* soname;
        std::span<const Binding> bindings;
    } libraries[] = {
        {"libEGL.so", egl_bindings},
        {"libGLESv3.so", gl_bindings},
        {"liblog.so", log_bindings},
        {nullptr, audio_bindings},
    };

    for (const auto& library : libraries) {
        for (const auto& binding : library.bindings) {
            const int32_t status = wa_dbt_register_symbol(dbt, library.soname, binding.name,
                                                          binding.function, nullptr);
            if (status != WA_OK) {
                return status;
            }
        }
    }
    return WA_OK;
}
