#include "native_graphics.h"
#include "region_data.h"
#include "software_raster.h"

#include <androidfw/Asset.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

// Bundled stb_image from the Emscripten 7.0.0 toolchain.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STB_IMAGE_STATIC
#include "stb_image.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace wa::ui_jni::detail {
namespace {

constexpr jint kMaxDimension = 4096;

bool ValidSize(jint width, jint height) {
    return width > 0 && width <= kMaxDimension && height > 0 && height <= kMaxDimension;
}

jintArray Allocate(JNIEnv* env, jclass, jint width, jint height) {
    if (!ValidSize(width, height)) {
        Throw(env, "java/lang/IllegalArgumentException",
              "Bitmap dimensions must be between 1 and 4096");
        return nullptr;
    }
    return env->NewIntArray(width * height);
}

jint ReadPixel(JNIEnv* env, jclass, jintArray pixels, jint index, jlong identity, jint generation) {
    State* state = GetState(env);
    if (!state)
        return 0;
    if (state->sf && index >= 0) {
        for (const auto& image : state->images) {
            if (image.identity != identity || image.generation != generation)
                continue;
            auto* store = wa_sf_bitmaps(state->sf);
            if (const auto* view = wa_bitmap_acquire(store, image.id)) {
                const auto offset = static_cast<uint32_t>(index) * 4;
                if (offset < view->byte_size) {
                    const auto* pixel = wa_bitmap_pixels(view) + offset;
                    uint32_t argb = uint32_t{pixel[3]} << 24 | uint32_t{pixel[0]} << 16 |
                                    uint32_t{pixel[1]} << 8 | pixel[2];
                    wa_bitmap_release(store, view);
                    return static_cast<jint>(argb);
                }
                wa_bitmap_release(store, view);
            }
            break;
        }
    }
    jint value = 0;
    env->GetIntArrayRegion(pixels, index, 1, &value);
    return value;
}

void Fill(JNIEnv* env, jclass, jintArray array, jint start, jint count, jint color) {
    const jint size = env->GetArrayLength(array);
    if (start < 0 || count < 0 || start > size - count) {
        Throw(env, "java/lang/ArrayIndexOutOfBoundsException", "Bitmap fill outside bounds");
        return;
    }
    if (count == 0)
        return;
    std::array<jint, 256> colors;
    colors.fill(color);
    for (jint offset = 0; offset < count && !env->ExceptionCheck(); offset += colors.size()) {
        env->SetIntArrayRegion(array, start + offset, std::min<jint>(count - offset, colors.size()),
                               colors.data());
    }
}

uint32_t ConvertPixel(uint32_t argb, jint format) {
    switch (format) {
    case 1:
        return argb | 0xff000000u;
    case 2:
        return argb & 0xff000000u;
    case 3: {
        const auto r = (argb >> 19) & 31u, g = (argb >> 10) & 63u, b = (argb >> 3) & 31u;
        return 0xff000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) |
               (b << 3 | b >> 2);
    }
    default:
        return argb;
    }
}

void CopyRows(JNIEnv* env, jclass, jintArray source, jint source_offset, jint source_stride,
              jintArray destination, jint destination_offset, jint destination_stride, jint width,
              jint height, jint format) {
    CallSafely(env, [&] {
        if (!ValidSize(width, height)) {
            Throw(env, "java/lang/IllegalArgumentException", "Invalid bitmap copy dimensions");
            return;
        }
        const auto valid = [width, height](jint length, jint offset, jint stride) {
            const int64_t first = offset;
            const int64_t last = first + static_cast<int64_t>(height - 1) * stride;
            return std::min(first, last) >= 0 && std::max(first, last) + width <= length;
        };
        if (!valid(env->GetArrayLength(source), source_offset, source_stride) ||
            !valid(env->GetArrayLength(destination), destination_offset, destination_stride)) {
            Throw(env, "java/lang/ArrayIndexOutOfBoundsException",
                  "Bitmap rows exceed the pixel array");
            return;
        }
        std::vector<jint> row(width);
        for (jint y = 0; y < height && !env->ExceptionCheck(); ++y) {
            const auto from = source_offset + static_cast<int64_t>(y) * source_stride;
            const auto to = destination_offset + static_cast<int64_t>(y) * destination_stride;
            env->GetIntArrayRegion(source, from, width, row.data());
            if (env->ExceptionCheck())
                return;
            if (format != 0) {
                for (jint& color : row)
                    color = static_cast<jint>(ConvertPixel(color, format));
            }
            env->SetIntArrayRegion(destination, to, width, row.data());
        }
    });
}

uint32_t Interpolate(uint32_t p00, uint32_t p01, uint32_t p10, uint32_t p11, float tx, float ty) {
    const float weights[] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
    const uint32_t pixels[] = {p00, p01, p10, p11};
    float rgba[4]{};
    for (int i = 0; i < 4; ++i) {
        const float alpha = (pixels[i] >> 24) * weights[i];
        rgba[3] += alpha;
        for (int c = 0; c < 3; ++c)
            rgba[c] += ((pixels[i] >> (8 * c)) & 255u) * alpha;
    }
    if (rgba[3] == 0)
        return 0;
    uint32_t result = static_cast<uint32_t>(rgba[3] + 0.5f) << 24;
    for (int c = 0; c < 3; ++c) {
        result |= static_cast<uint32_t>(rgba[c] / rgba[3] + 0.5f) << (8 * c);
    }
    return result;
}

void Scale(JNIEnv* env, jclass, jintArray image, jint width, jint height, jintArray output,
           jint out_width, jint out_height, jboolean filter) {
    CallSafely(env, [&] {
        if (!ValidSize(width, height) || !ValidSize(out_width, out_height) ||
            env->GetArrayLength(image) != width * height ||
            env->GetArrayLength(output) != out_width * out_height) {
            Throw(env, "java/lang/IllegalArgumentException", "Invalid bitmap dimensions");
            return;
        }
        std::vector<jint> source(width * height);
        env->GetIntArrayRegion(image, 0, source.size(), source.data());
        if (env->ExceptionCheck())
            return;
        std::vector<jint> row(out_width);
        for (jint y = 0; y < out_height; ++y) {
            float sy = (y + 0.5f) * height / out_height;
            sy = std::clamp(sy - (filter ? 0.5f : 0.f), 0.f, static_cast<float>(height - 1));
            const jint y0 = static_cast<jint>(sy), y1 = std::min(y0 + 1, height - 1);
            for (jint x = 0; x < out_width; ++x) {
                float sx = (x + 0.5f) * width / out_width;
                sx = std::clamp(sx - (filter ? 0.5f : 0.f), 0.f, static_cast<float>(width - 1));
                const jint x0 = static_cast<jint>(sx), x1 = std::min(x0 + 1, width - 1);
                row[x] = filter ? static_cast<jint>(
                                      Interpolate(source[y0 * width + x0], source[y0 * width + x1],
                                                  source[y1 * width + x0], source[y1 * width + x1],
                                                  sx - x0, sy - y0))
                                : source[y0 * width + x0];
            }
            env->SetIntArrayRegion(output, y * out_width, out_width, row.data());
            if (env->ExceptionCheck())
                return;
        }
    });
}

uint32_t Read32(const uint8_t* data) {
    return uint32_t{data[0]} << 24 | uint32_t{data[1]} << 16 | uint32_t{data[2]} << 8 | data[3];
}

uint16_t Read16(const uint8_t* data) {
    return uint16_t{data[0]} << 8 | data[1];
}

int Paeth(int a, int b, int c) {
    int p = a + b - c;
    int da = std::abs(p - a), db = std::abs(p - b), dc = std::abs(p - c);
    if (da <= db && da <= dc)
        return a;
    return db <= dc ? b : c;
}

class Png {
  public:
    bool Read(std::span<const uint8_t> input, bool bounds_only) {
        constexpr std::array<uint8_t, 8> signature{137, 'P', 'N', 'G', 13, 10, 26, 10};
        if (input.size() < signature.size() || !std::ranges::equal(input.first(8), signature))
            return false;
        input = input.subspan(8);
        bool have_header = false;
        while (input.size() >= 12) {
            const uint32_t length = Read32(input.data());
            if (length > input.size() - 12)
                return false;
            std::string_view tag(reinterpret_cast<const char*>(input.data() + 4), 4);
            const auto chunk = input.subspan(8, length);
            const uint32_t expected_crc = Read32(chunk.data() + chunk.size());
            if (crc32(0, input.data() + 4, length + 4) != expected_crc)
                return false;
            if (!have_header && tag != "IHDR")
                return false;
            if (tag == "IHDR") {
                if (have_header || !Header(chunk))
                    return false;
                have_header = true;
                if (bounds_only)
                    return true;
            } else if (tag == "PLTE") {
                if (chunk.empty() || chunk.size() % 3 || chunk.size() > 256 * 3)
                    return false;
                palette_.assign(chunk.begin(), chunk.end());
            } else if (tag == "tRNS") {
                if (chunk.size() > 256)
                    return false;
                transparency_.assign(chunk.begin(), chunk.end());
            } else if (tag == "IDAT") {
                compressed_.insert(compressed_.end(), chunk.begin(), chunk.end());
            } else if (tag == "IEND") {
                return chunk.empty() && Decode();
            } else if ((tag[0] & 0x20) == 0) {
                return false; // Unknown critical chunk.
            }
            input = input.subspan(length + 12);
        }
        return false;
    }

    jint width = 0, height = 0;
    std::vector<jint> pixels;

  private:
    bool Header(std::span<const uint8_t> data) {
        if (data.size() != 13)
            return false;
        uint32_t w = Read32(data.data()), h = Read32(data.data() + 4);
        if (w == 0 || h == 0 || w > kMaxDimension || h > kMaxDimension || data[10] != 0 ||
            data[11] != 0 || data[12] != 0)
            return false;
        width = static_cast<jint>(w);
        height = static_cast<jint>(h);
        depth_ = data[8];
        type_ = data[9];
        switch (type_) {
        case 0:
            channels_ = 1;
            return depth_ == 1 || depth_ == 2 || depth_ == 4 || depth_ == 8 || depth_ == 16;
        case 2:
            channels_ = 3;
            return depth_ == 8 || depth_ == 16;
        case 3:
            channels_ = 1;
            return depth_ == 1 || depth_ == 2 || depth_ == 4 || depth_ == 8;
        case 4:
            channels_ = 2;
            return depth_ == 8 || depth_ == 16;
        case 6:
            channels_ = 4;
            return depth_ == 8 || depth_ == 16;
        default:
            return false;
        }
    }

    uint32_t Sample(const uint8_t* row, size_t index) const {
        if (depth_ == 16)
            return Read16(row + 2 * index);
        if (depth_ == 8)
            return row[index];
        const size_t bits = index * depth_;
        return (row[bits / 8] >> (8 - depth_ - bits % 8)) & ((1u << depth_) - 1);
    }

    uint32_t Color8(uint32_t sample) const {
        if (depth_ == 16)
            return sample >> 8;
        return sample * 255u / ((1u << depth_) - 1);
    }

    bool Decode() {
        if (compressed_.empty() || (type_ == 3 && palette_.empty()))
            return false;
        const size_t row_bytes = (static_cast<size_t>(width) * channels_ * depth_ + 7) / 8;
        const size_t bpp = std::max<size_t>(1, channels_ * depth_ / 8);
        std::vector<uint8_t> encoded((row_bytes + 1) * height);
        uLongf expected = encoded.size();
        if (uncompress(encoded.data(), &expected, compressed_.data(), compressed_.size()) != Z_OK ||
            expected != encoded.size())
            return false;

        pixels.resize(static_cast<size_t>(width) * height);
        const uint8_t* previous = nullptr;
        for (jint y = 0; y < height; ++y) {
            uint8_t* row = encoded.data() + (row_bytes + 1) * y + 1;
            uint8_t filter = row[-1];
            if (filter > 4)
                return false;
            for (size_t i = 0; i < row_bytes; ++i) {
                int a = i < bpp ? 0 : row[i - bpp];
                int b = previous ? previous[i] : 0;
                int c = previous && i >= bpp ? previous[i - bpp] : 0;
                switch (filter) {
                case 1:
                    row[i] += a;
                    break;
                case 2:
                    row[i] += b;
                    break;
                case 3:
                    row[i] += (a + b) / 2;
                    break;
                case 4:
                    row[i] += Paeth(a, b, c);
                    break;
                }
            }
            for (jint x = 0; x < width; ++x) {
                uint32_t r, g, b, alpha = 255;
                const size_t at = x * channels_;
                if (type_ == 3) {
                    uint32_t index = Sample(row, at);
                    if (index * 3 >= palette_.size())
                        return false;
                    r = palette_[index * 3];
                    g = palette_[index * 3 + 1];
                    b = palette_[index * 3 + 2];
                    if (index < transparency_.size())
                        alpha = transparency_[index];
                } else if (type_ == 0 || type_ == 4) {
                    const auto sample = Sample(row, at);
                    r = g = b = Color8(sample);
                    if (type_ == 4)
                        alpha = Color8(Sample(row, at + 1));
                    else if (transparency_.size() == 2 && sample == Read16(transparency_.data()))
                        alpha = 0;
                } else {
                    const auto rs = Sample(row, at), gs = Sample(row, at + 1),
                               bs = Sample(row, at + 2);
                    r = Color8(rs);
                    g = Color8(gs);
                    b = Color8(bs);
                    if (type_ == 6)
                        alpha = Color8(Sample(row, at + 3));
                    else if (transparency_.size() == 6 && rs == Read16(transparency_.data()) &&
                             gs == Read16(transparency_.data() + 2) &&
                             bs == Read16(transparency_.data() + 4))
                        alpha = 0;
                }
                pixels[y * width + x] = static_cast<jint>(alpha << 24 | r << 16 | g << 8 | b);
            }
            previous = row;
        }
        return true;
    }

    uint8_t type_ = 0, depth_ = 0, channels_ = 0;
    std::vector<uint8_t> palette_, transparency_, compressed_;
};

jintArray DecodePng(JNIEnv* env, jclass, jbyteArray bytes, jint offset, jint length, jintArray size,
                    jboolean bounds_only) {
    return CallSafely(env, [&]() -> jintArray {
        if (length < 8 || length > 64 * 1024 * 1024)
            return nullptr;
        std::vector<uint8_t> input(length);
        env->GetByteArrayRegion(bytes, offset, length, reinterpret_cast<jbyte*>(input.data()));
        if (env->ExceptionCheck())
            return nullptr;
        Png png;
        if (!png.Read(input, bounds_only))
            return nullptr;
        const jint dimensions[] = {png.width, png.height};
        env->SetIntArrayRegion(size, 0, 2, dimensions);
        if (bounds_only || env->ExceptionCheck())
            return nullptr;
        jintArray result = env->NewIntArray(png.pixels.size());
        if (result)
            env->SetIntArrayRegion(result, 0, png.pixels.size(), png.pixels.data());
        return result;
    });
}

} // namespace

#include "bitmap_native.inc"

void RegisterBitmaps(interp::Vm& vm) {
    if (RegisterIfDeclared(vm, "android/graphics/Bitmap",
        Method<CreateBitmap>(vm, "nativeCreate", "([IIIIIIZJ)Landroid/graphics/Bitmap;"))) {
        RegisterModernBitmaps(vm);
        return;
    }
    Register(vm, "android/graphics/Bitmap", Method<Allocate>(vm, "nativeAllocate", "(II)[I"),
             Method<ReadPixel>(vm, "nativePixel", "([IIJI)I"),
             Method<CopyRows>(vm, "nativeCopyRows", "([III[IIIIII)V"),
             Method<Fill>(vm, "nativeFill", "([IIII)V"),
             Method<Scale>(vm, "nativeScale", "([III[IIIZ)V"));
    Register(vm, "android/graphics/BitmapFactory",
             Method<DecodePng>(vm, "nativeDecodePng", "([BII[IZ)[I"));
}

} // namespace wa::ui_jni::detail
