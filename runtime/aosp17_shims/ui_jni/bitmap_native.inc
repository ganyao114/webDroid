jint BitmapData::PutColor(uint32_t color) const {
    if (config == 1) return color & 0xff000000u;
    if (config == 3) return ConvertPixel(color, 3);
    return has_alpha ? color : color | 0xff000000u;
}

namespace {

constexpr size_t kMaxEncodedSize = 64 * 1024 * 1024;

auto ValidConfig(jint config) {
    return config == 1 || (config >= 3 && config <= 8 && config != 7);
}

jint BytesPerPixel(jint config) {
    if (config == 1) return 1;
    if (config == 3 || config == 4) return 2;
    if (config == 6) return 8;
    return 4;
}

BitmapData NewBitmapData(jint width, jint height, jint config, bool mutable_pixels,
                         bool alpha = true, bool premultiplied = true) {
    BitmapData bitmap;
    bitmap.width = width;
    bitmap.height = height;
    bitmap.config = config == 4 ? 5 : config;
    bitmap.has_alpha = config == 3 ? false : alpha;
    bitmap.immutable = !mutable_pixels;
    bitmap.premultiplied = premultiplied;
    bitmap.pixels.resize(static_cast<size_t>(width) * height, bitmap.PutColor(0));
    return bitmap;
}

jobject WrapBitmap(JNIEnv* env, BitmapData bitmap, jint density = -1, jbyteArray chunk = nullptr) {
    auto* state = GetState(env);
    const auto type = env->FindClass("android/graphics/Bitmap");
    if (!state || !type) return nullptr;
    const auto ctor = env->GetMethodID(type, "<init>", "(JIIIZ[BLandroid/graphics/NinePatch$InsetStruct;)V");
    if (!ctor) return nullptr;
    const jint width = bitmap.width, height = bitmap.height;
    const jboolean premul = bitmap.premultiplied;
    auto& store = Peers<BitmapData, &State::bitmaps>(*state);
    const jlong handle = store.Insert(std::move(bitmap));
    jvalue args[7]{};
    args[0].j = handle;
    args[1].i = width;
    args[2].i = height;
    args[3].i = density;
    args[4].z = premul;
    args[5].l = chunk;
    auto object = env->NewObjectA(type, ctor, args);
    if (!object) store.Erase(handle);
    return object;
}

bool RowsFit(jint length, jint offset, jint stride, jint width, jint height) {
    const int64_t last = offset + int64_t(height - 1) * stride;
    return height >= 0 && width >= 0 && std::min<int64_t>(offset, last) >= 0 &&
           std::max<int64_t>(offset, last) + width <= length;
}
bool BitmapRectFits(const BitmapData& bitmap, jint x, jint y, jint width, jint height) {
    return !bitmap.recycled && x >= 0 && y >= 0 && width >= 0 && height >= 0 &&
           width <= bitmap.width && height <= bitmap.height &&
           x <= bitmap.width - width && y <= bitmap.height - height;
}
void CopyPixels(JNIEnv* env, BitmapData& bitmap, jintArray array, jint offset, jint stride,
                jint x, jint y, jint width, jint height, bool input) {
    if (!width || !height) return;
    if (!array || !BitmapRectFits(bitmap, x, y, width, height) ||
        !RowsFit(env->GetArrayLength(array), offset, stride, width, height)) {
        Throw(env, "java/lang/ArrayIndexOutOfBoundsException", "Bitmap pixel array range");
        return;
    }
    for (jint row = 0; row < height; ++row) {
        auto* pixels = bitmap.pixels.data() + static_cast<size_t>(row + y) * bitmap.width + x;
        const jint index = offset + static_cast<int64_t>(row) * stride;
        if (input) {
            env->GetIntArrayRegion(array, index, width, pixels);
            if (env->ExceptionCheck()) return;
            for (jint i = 0; i < width; ++i) pixels[i] = bitmap.PutColor(pixels[i]);
        } else {
            env->SetIntArrayRegion(array, index, width, pixels);
            if (env->ExceptionCheck()) return;
        }
    }
    if (input) bitmap.Changed();
}

jobject CreateBitmap(JNIEnv* env, jclass, jintArray colors, jint offset, jint stride,
                     jint width, jint height, jint config, jboolean mutable_pixels, jlong) {
    return CallSafely(env, [&]() -> jobject {
        if (!ValidSize(width, height) || !ValidConfig(config)) {
            Throw(env, "java/lang/IllegalArgumentException", "Unsupported software bitmap size or config");
            return nullptr;
        }
        auto bitmap = NewBitmapData(width, height, config, mutable_pixels);
        if (colors) CopyPixels(env, bitmap, colors, offset, stride, 0, 0, width, height, true);
        return env->ExceptionCheck() ? nullptr : WrapBitmap(env, std::move(bitmap));
    });
}
jobject CopyBitmap(JNIEnv* env, jclass, jlong src, jint config, jboolean mutable_pixels) {
    return CallSafely(env, [&]() -> jobject {
        const auto* bitmap = Bitmap(env, src);
        if (!bitmap || bitmap->recycled || !ValidConfig(config)) return nullptr;
        auto copy = *bitmap;
        copy.config = config == 4 ? 5 : config;
        copy.immutable = !mutable_pixels;
        copy.has_alpha = copy.has_alpha && config != 3;
        copy.generation = 1;
        for (jint& color : copy.pixels) color = copy.PutColor(color);
        return WrapBitmap(env, std::move(copy));
    });
}
jobject CopyInternalBitmap(JNIEnv* env, jclass type, jlong id) {
    const auto* bitmap = Bitmap(env, id);
    return bitmap ? CopyBitmap(env, type, id, bitmap->config, false) : nullptr;
}
jint GetBitmapPixel(JNIEnv* env, jclass, jlong id, jint x, jint y) {
    const auto* bitmap = Bitmap(env, id);
    if (!bitmap || !BitmapRectFits(*bitmap, x, y, 1, 1)) return 0;
    return bitmap->pixels[y * bitmap->width + x];
}
jlong GetBitmapColor(JNIEnv* env, jclass type, jlong id, jint x, jint y) {
    return static_cast<jlong>(uint64_t(uint32_t(GetBitmapPixel(env, type, id, x, y))) << 32);
}
void PutBitmapPixel(JNIEnv* env, jclass, jlong id, jint x, jint y, jint color) {
    if (auto* bitmap = Bitmap(env, id); bitmap && BitmapRectFits(*bitmap, x, y, 1, 1)) {
        bitmap->pixels[y * bitmap->width + x] = bitmap->PutColor(color);
        bitmap->Changed();
    }
}
template <bool Write>
void BitmapPixels(JNIEnv* env, jclass, jlong id, jintArray pixels, jint offset, jint stride,
                  jint x, jint y, jint width, jint height) {
    if (auto* bitmap = Bitmap(env, id)) CopyPixels(env, *bitmap, pixels, offset, stride, x, y, width, height, Write);
}
void BitmapRecycle(JNIEnv* env, jclass, jlong id) {
    if (auto* bitmap = Bitmap(env, id)) {
        bitmap->recycled = true;
        bitmap->Changed();
        std::vector<jint>{}.swap(bitmap->pixels);
    }
}
void EraseBitmap(JNIEnv* env, jclass, jlong id, jint color) {
    if (auto* bitmap = Bitmap(env, id)) {
        std::ranges::fill(bitmap->pixels, bitmap->PutColor(color));
        bitmap->Changed();
    }
}
void EraseBitmapLong(JNIEnv* env, jclass type, jlong id, jlong, jlong color) {
    EraseBitmap(env, type, id, UnpackColor(color));
}
jint BitmapRowBytes(JNIEnv* env, jclass, jlong id) {
    const auto* bitmap = Bitmap(env, id);
    return bitmap ? bitmap->width * BytesPerPixel(bitmap->config) : 0;
}
jint BitmapAllocBytes(JNIEnv* env, jclass, jlong id) {
    const auto* bitmap = Bitmap(env, id);
    return bitmap ? static_cast<jint>(bitmap->pixels.capacity() * BytesPerPixel(bitmap->config)) : 0;
}
void ReconfigureBitmap(JNIEnv* env, jclass, jlong id, jint width, jint height, jint config,
                       jboolean premultiplied) {
    CallSafely(env, [&] {
        auto* bitmap = Bitmap(env, id);
        if (!bitmap) return;
        if (!ValidSize(width, height) || !ValidConfig(config) || bitmap->immutable) {
            Throw(env, "java/lang/IllegalArgumentException", "Invalid bitmap reconfiguration");
            return;
        }
        const auto count = static_cast<size_t>(width) * height;
        if (count * BytesPerPixel(config) > bitmap->pixels.capacity() * BytesPerPixel(bitmap->config)) {
            Throw(env, "java/lang/IllegalArgumentException", "Bitmap allocation is too small");
            return;
        }
        bitmap->pixels.resize(count);
        bitmap->width = width;
        bitmap->height = height;
        bitmap->config = config == 4 ? 5 : config;
        bitmap->premultiplied = premultiplied;
        bitmap->has_alpha = config != 3;
        for (jint& color : bitmap->pixels) color = bitmap->PutColor(color);
        bitmap->Changed();
    });
}
template <auto Member, class T>
T BitmapGet(JNIEnv* env, jclass, jlong id) {
    const auto* bitmap = Bitmap(env, id);
    return bitmap ? static_cast<T>(bitmap->*Member) : T{};
}
template <auto Member, class T>
void BitmapSet(JNIEnv* env, jclass, jlong id, T value) {
    if (auto* bitmap = Bitmap(env, id)) bitmap->*Member = value;
}
void SetImmutable(JNIEnv* env, jclass, jlong id) {
    if (auto* bitmap = Bitmap(env, id)) bitmap->immutable = true;
}
void SetBitmapAlpha(JNIEnv* env, jclass, jlong id, jboolean alpha, jboolean premultiplied) {
    if (auto* bitmap = Bitmap(env, id)) {
        bitmap->has_alpha = bitmap->config == 3 ? false : alpha;
        bitmap->premultiplied = premultiplied;
        if (!bitmap->has_alpha)
            for (jint& color : bitmap->pixels) color = bitmap->PutColor(color);
        bitmap->Changed();
    }
}
jboolean BitmapPremultiplied(JNIEnv* env, jclass, jlong id) {
    const auto* bitmap = Bitmap(env, id);
    return bitmap && bitmap->has_alpha && bitmap->config != 1 && bitmap->premultiplied;
}
jboolean SameBitmaps(JNIEnv* env, jclass, jlong a, jlong b) {
    const auto* x = Bitmap(env, a);
    const auto* y = Bitmap(env, b);
    return x && y && x->width == y->width && x->height == y->height &&
           x->config == y->config && x->pixels == y->pixels;
}
jobject ExtractBitmapAlpha(JNIEnv* env, jclass, jlong id, jlong paint, jintArray offset) {
    return CallSafely(env, [&]() -> jobject {
        const auto* bitmap = Bitmap(env, id);
        const auto* p = paint ? Paint(env, paint) : nullptr;
        if (!bitmap || bitmap->recycled || env->ExceptionCheck()) return nullptr;
        const uint32_t opacity = p ? p->color >> 24 : 255u;
        auto result = NewBitmapData(bitmap->width, bitmap->height, 1, true);
        for (size_t at = 0; at < bitmap->pixels.size(); ++at) {
            const uint32_t alpha = static_cast<uint32_t>(bitmap->pixels[at]) >> 24;
            result.pixels[at] = ((alpha * opacity + 127) / 255) << 24;
        }
        if (offset) {
            const jint zero[] = {0, 0};
            env->SetIntArrayRegion(offset, 0, 2, zero);
        }
        return env->ExceptionCheck() ? nullptr : WrapBitmap(env, std::move(result));
    });
}
jboolean CompressBitmap(JNIEnv* env, jclass, jlong id, jint format, jint, jobject stream, jbyteArray) {
    return CallSafely(env, [&]() -> jboolean {
        const auto* bitmap = Bitmap(env, id);
        if (!bitmap || bitmap->recycled || format != 1 || !stream) return false;
        using interp::software_raster::AppendNetworkInt;
        using interp::software_raster::AppendPngChunk;
        const size_t row_bytes = static_cast<size_t>(bitmap->width) * 4 + 1;
        std::vector<uint8_t> raw(row_bytes * bitmap->height);
        for (jint y = 0; y < bitmap->height; ++y) {
            auto* row = raw.data() + row_bytes * y + 1;
            for (jint x = 0; x < bitmap->width; ++x) {
                const uint32_t pixel = bitmap->pixels[y * bitmap->width + x];
                *row++ = pixel >> 16;
                *row++ = pixel >> 8;
                *row++ = pixel;
                *row++ = pixel >> 24;
            }
        }
        uLongf size = compressBound(raw.size());
        std::vector<uint8_t> compressed(size);
        if (compress2(compressed.data(), &size, raw.data(), raw.size(), Z_DEFAULT_COMPRESSION) != Z_OK)
            return false;
        compressed.resize(size);
        std::vector<uint8_t> png{137, 80, 78, 71, 13, 10, 26, 10};
        std::vector<uint8_t> ihdr;
        AppendNetworkInt(ihdr, bitmap->width);
        AppendNetworkInt(ihdr, bitmap->height);
        ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
        AppendPngChunk(png, "IHDR", ihdr);
        AppendPngChunk(png, "IDAT", compressed);
        AppendPngChunk(png, "IEND", {});
        const auto type = env->GetObjectClass(stream);
        const auto write = env->GetMethodID(type, "write", "([BII)V");
        if (!write) return false;
        auto bytes = env->NewByteArray(png.size());
        if (!bytes) return false;
        env->SetByteArrayRegion(bytes, 0, png.size(), reinterpret_cast<const jbyte*>(png.data()));
        if (!env->ExceptionCheck()) env->CallVoidMethod(stream, write, bytes, 0, jint(png.size()));
        return !env->ExceptionCheck();
    });
}

jboolean BitmapFalse(JNIEnv*, jclass, jlong) { return false; }
jboolean BitmapSrgb(JNIEnv* env, jclass, jlong id) {
    const auto* bitmap = Bitmap(env, id);
    return bitmap && bitmap->config != 1;
}
void BitmapPrepare(JNIEnv*, jclass, jlong) {
    // Raster pixels are already CPU-accessible. The scene imports them on draw.
}
jlong BitmapFinalizer(JNIEnv*, jclass) { return 0; }
void SetBitmapGainmap(JNIEnv* env, jclass, jlong, jlong gainmap) {
    if (gainmap) Throw(env, "java/lang/UnsupportedOperationException", "HDR gainmaps require hardware graphics");
}

struct DecodeOptions {
    JNIEnv* env;
    jobject object;
    jclass type = object ? env->GetObjectClass(object) : nullptr;
    jfieldID Field(const char* name, const char* signature) const {
        return type ? env->GetFieldID(type, name, signature) : nullptr;
    }
    bool Bool(const char* name, bool fallback = false) const {
        const auto id = Field(name, "Z");
        return id ? env->GetBooleanField(object, id) != 0 : fallback;
    }
    jint Int(const char* name, jint fallback = 0) const {
        const auto id = Field(name, "I");
        return id ? env->GetIntField(object, id) : fallback;
    }
    void PutInt(const char* name, jint value) const {
        if (const auto id = Field(name, "I")) env->SetIntField(object, id, value);
    }
    void PutObject(const char* name, const char* signature, jobject value) const {
        if (const auto id = Field(name, signature)) env->SetObjectField(object, id, value);
    }
    jobject GetObject(const char* name, const char* signature) const {
        const auto id = Field(name, signature);
        return id ? env->GetObjectField(object, id) : nullptr;
    }
};

std::span<const uint8_t> NinePatchChunk(std::span<const uint8_t> bytes) {
    if (bytes.size() < 8 || bytes[0] != 137 || bytes[1] != 'P' || bytes[2] != 'N' || bytes[3] != 'G')
        return {};
    bytes = bytes.subspan(8);
    while (bytes.size() >= 12) {
        const uint32_t size = Read32(bytes.data());
        if (size > bytes.size() - 12) break;
        if (std::memcmp(bytes.data() + 4, "npTc", 4) == 0 && size >= 32)
            return bytes.subspan(8, size);
        bytes = bytes.subspan(size + 12);
    }
    return {};
}

jobject DecodeImage(JNIEnv* env, std::span<const uint8_t> bytes, jobject options, jobject padding,
                    jlong reused) {
    const DecodeOptions opts{env, options};
    opts.PutInt("outWidth", -1);
    opts.PutInt("outHeight", -1);
    opts.PutObject("outMimeType", "Ljava/lang/String;", nullptr);
    if (padding) SetRect(env, padding, {-1, -1, -1, -1});
    int width, height, channels;
    if (bytes.empty() || bytes.size() > kMaxEncodedSize ||
        !stbi_info_from_memory(bytes.data(), bytes.size(), &width, &height, &channels) ||
        !ValidSize(width, height))
        return nullptr;
    const auto sample = std::max(1, opts.Int("inSampleSize"));
    const jint density = opts.Int("inDensity"), target = opts.Int("inTargetDensity");
    const auto chunk = NinePatchChunk(bytes);
    const bool scaled = (opts.Bool("inScaled", true) || !chunk.empty()) && density > 0 && target > 0 &&
                        density != target && density != opts.Int("inScreenDensity");
    const double scale = scaled ? double(target) / density : 1.0;
    const auto dimension = [&](int size) { return std::clamp(std::round(std::max(1, size / sample) * scale), 1.0, double(kMaxDimension + 1)); };
    const jint out_w = dimension(width), out_h = dimension(height);
    if (!ValidSize(out_w, out_h)) return nullptr;
    const bool bounds = opts.Bool("inJustDecodeBounds");
    opts.PutInt("outWidth", bounds ? width : out_w);
    opts.PutInt("outHeight", bounds ? height : out_h);
    if (options) {
        const bool png = bytes.size() > 8 && bytes[0] == 137 && bytes[1] == 'P';
        opts.PutObject("outMimeType", "Ljava/lang/String;", env->NewStringUTF(png ? "image/png" : "image/jpeg"));
    }
    if (bounds || env->ExceptionCheck()) return nullptr;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> rgba(
        stbi_load_from_memory(bytes.data(), bytes.size(), &width, &height, &channels, 4), &stbi_image_free);
    if (!rgba) {
        opts.PutInt("outWidth", -1);
        opts.PutInt("outHeight", -1);
        return nullptr;
    }
    jint config = 5;
    if (const auto preferred = opts.GetObject("inPreferredConfig", "Landroid/graphics/Bitmap$Config;")) {
        const auto cls = env->GetObjectClass(preferred);
        if (const auto id = env->GetFieldID(cls, "nativeInt", "I")) {
            const auto candidate = env->GetIntField(preferred, id);
            if (ValidConfig(candidate)) config = candidate;
        }
    }
    auto bitmap = NewBitmapData(out_w, out_h, config, opts.Bool("inMutable") || reused,
                                channels == 2 || channels == 4, opts.Bool("inPremultiplied", true));
    for (jint y = 0; y < out_h; ++y) {
        for (jint x = 0; x < out_w; ++x) {
            const auto* p = rgba.get() + 4 * (static_cast<size_t>(y * height / out_h) * width + x * width / out_w);
            const uint32_t color = uint32_t(p[3]) << 24 | uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
            bitmap.pixels[y * out_w + x] = bitmap.PutColor(color);
        }
    }
    if (reused) {
        auto* old = Bitmap(env, reused);
        const auto object = opts.GetObject("inBitmap", "Landroid/graphics/Bitmap;");
        if (!old || !object || old->recycled || old->immutable ||
            old->pixels.capacity() < bitmap.pixels.size()) return nullptr;
        bitmap.generation = old->generation;
        bitmap.Changed();
        *old = std::move(bitmap);
        const auto method = env->GetMethodID(env->GetObjectClass(object), "reinit", "(IIZ)V");
        if (method) env->CallVoidMethod(object, method, out_w, out_h, jint(old->premultiplied));
        return env->ExceptionCheck() ? nullptr : object;
    }
    jbyteArray patch = nullptr;
    if (!chunk.empty()) {
        patch = env->NewByteArray(chunk.size());
        if (!patch) return nullptr;
        env->SetByteArrayRegion(patch, 0, chunk.size(), reinterpret_cast<const jbyte*>(chunk.data()));
        if (padding) SetRect(env, padding, {float(Read32(chunk.data() + 12)), float(Read32(chunk.data() + 20)),
                                           float(Read32(chunk.data() + 16)), float(Read32(chunk.data() + 24))});
    }
    return env->ExceptionCheck() ? nullptr : WrapBitmap(env, std::move(bitmap), -1, patch);
}

jobject DecodeByteArray(JNIEnv* env, jclass, jbyteArray array, jint offset, jint length, jobject options,
                        jlong bitmap, jlong) {
    return CallSafely(env, [&]() -> jobject {
        if (length < 0 || static_cast<size_t>(length) > kMaxEncodedSize) return nullptr;
        std::vector<uint8_t> bytes(length);
        env->GetByteArrayRegion(array, offset, length, reinterpret_cast<jbyte*>(bytes.data()));
        return env->ExceptionCheck() ? nullptr : DecodeImage(env, bytes, options, nullptr, bitmap);
    });
}

jobject DecodeStream(JNIEnv* env, jclass, jobject stream, jbyteArray scratch, jobject padding,
                     jobject options, jlong bitmap, jlong) {
    return CallSafely(env, [&]() -> jobject {
        if (!stream || !scratch) return nullptr;
        const auto cls = env->GetObjectClass(stream);
        const auto read = env->GetMethodID(cls, "read", "([BII)I");
        const jint capacity = env->GetArrayLength(scratch);
        if (!read || capacity <= 0) return nullptr;
        std::vector<uint8_t> bytes;
        for (;;) {
            const auto count = env->CallIntMethod(stream, read, scratch, 0, capacity);
            if (env->ExceptionCheck()) return nullptr;
            if (count == -1) break;
            if (count <= 0 || count > capacity || bytes.size() + count > kMaxEncodedSize) return nullptr;
            const size_t at = bytes.size();
            bytes.resize(at + count);
            env->GetByteArrayRegion(scratch, 0, count, reinterpret_cast<jbyte*>(bytes.data() + at));
            if (env->ExceptionCheck()) return nullptr;
        }
        return DecodeImage(env, bytes, options, padding, bitmap);
    });
}

int FileDescriptor(JNIEnv* env, jobject fd) {
    if (!fd) return -1;
    const auto type = env->GetObjectClass(fd);
    const auto id = env->GetFieldID(type, "descriptor", "I");
    return id ? env->GetIntField(fd, id) : -1;
}
jboolean Seekable(JNIEnv* env, jclass, jobject fd) {
    return lseek(FileDescriptor(env, fd), 0, SEEK_CUR) >= 0;
}
jobject DecodeFileDescriptor(JNIEnv* env, jclass, jobject fd, jobject padding, jobject options,
                             jlong bitmap, jlong) {
    return CallSafely(env, [&]() -> jobject {
        const int file = FileDescriptor(env, fd);
        auto offset = lseek(file, 0, SEEK_CUR);
        if (file < 0 || offset < 0) return nullptr;
        std::vector<uint8_t> image;
        std::array<uint8_t, 16 * 1024> buffer;
        for (;;) {
            const auto count = pread(file, buffer.data(), buffer.size(), offset);
            if (count < 0 && errno == EINTR) continue;
            if (!count) break;
            if (count < 0 || image.size() + count > kMaxEncodedSize) return nullptr;
            image.insert(image.end(), buffer.begin(), buffer.begin() + count);
            offset += count;
        }
        return DecodeImage(env, image, options, padding, bitmap);
    });
}
jobject DecodeFile(JNIEnv* env, jclass, jstring name, jobject options, jlong bitmap, jlong) {
    return CallSafely(env, [&]() -> jobject {
        const std::string path = Utf8(env, name);
        if (env->ExceptionCheck() || path.find('\0') != std::string::npos) return nullptr;
        struct File {
            int fd;
            ~File() { if (fd >= 0) close(fd); }
        } file{open(path.c_str(), O_RDONLY | O_CLOEXEC)};
        if (file.fd < 0) return nullptr;
        std::vector<uint8_t> image;
        std::array<uint8_t, 16 * 1024> buffer;
        for (;;) {
            const auto count = read(file.fd, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) continue;
            if (!count) break;
            if (count < 0 || image.size() + count > kMaxEncodedSize) return nullptr;
            image.insert(image.end(), buffer.begin(), buffer.begin() + count);
        }
        return DecodeImage(env, image, options, nullptr, bitmap);
    });
}
jobject DecodeAsset(JNIEnv* env, jclass, jlong handle, jobject padding, jobject options,
                    jlong bitmap, jlong) {
    return CallSafely(env, [&]() -> jobject {
        auto* asset = reinterpret_cast<android::Asset*>(static_cast<uintptr_t>(handle));
        if (!asset || asset->getLength() <= 0 || asset->getLength() > kMaxEncodedSize) return nullptr;
        const auto* bytes = static_cast<const uint8_t*>(asset->getBuffer(false));
        return bytes ? DecodeImage(env, {bytes, static_cast<size_t>(asset->getLength())},
                                   options, padding, bitmap) : nullptr;
    });
}

jboolean IsNinePatch(JNIEnv* env, jclass, jbyteArray chunk) {
    if (!chunk || env->GetArrayLength(chunk) < 32) return false;
    jbyte header[4]{};
    env->GetByteArrayRegion(chunk, 0, 4, header);
    const auto x = static_cast<uint8_t>(header[1]), y = static_cast<uint8_t>(header[2]),
               colors = static_cast<uint8_t>(header[3]);
    return x && y && !(x & 1) && !(y & 1) && env->GetArrayLength(chunk) >= 32 + 4 * (x + y + colors);
}

void RegisterModernBitmaps(interp::Vm& vm) {
    constexpr const char* bitmap = "android/graphics/Bitmap";
    Register(vm, bitmap,
             Method<RetainPeer<BitmapData, &State::bitmaps>>(vm, "nRegisterNativeAllocation", "(Landroid/graphics/Bitmap;J)V"),
             Method<CopyBitmap>(vm, "nativeCopy", "(JIZ)Landroid/graphics/Bitmap;"),
             Method<CopyInternalBitmap>(vm, "nativeCopyPreserveInternalConfig", "(J)Landroid/graphics/Bitmap;"),
             Method<BitmapFinalizer>(vm, "nativeGetNativeFinalizer", "()J"),
             Method<BitmapFinalizer>(vm, "nativeGetNativeNoop", "()J"),
             Method<BitmapRecycle>(vm, "nativeRecycle", "(J)V"),
             Method<ReconfigureBitmap>(vm, "nativeReconfigure", "(JIIIZ)V"),
             Method<EraseBitmap>(vm, "nativeErase", "(JI)V"),
             Method<EraseBitmapLong>(vm, "nativeErase", "(JJJ)V"),
             Method<GetBitmapPixel>(vm, "nativeGetPixel", "(JII)I"),
             Method<GetBitmapColor>(vm, "nativeGetColor", "(JII)J"),
             Method<PutBitmapPixel>(vm, "nativeSetPixel", "(JIII)V"),
             Method<BitmapPixels<false>>(vm, "nativeGetPixels", "(J[IIIIIII)V"),
             Method<BitmapPixels<true>>(vm, "nativeSetPixels", "(J[IIIIIII)V"),
             Method<BitmapRowBytes>(vm, "nativeRowBytes", "(J)I"),
             Method<BitmapAllocBytes>(vm, "nativeGetAllocationByteCount", "(J)I"),
             Method<BitmapGet<&BitmapData::config, jint>>(vm, "nativeConfig", "(J)I"),
             Method<BitmapGet<&BitmapData::generation, jint>>(vm, "nativeGenerationId", "(J)I"),
             Method<BitmapGet<&BitmapData::has_alpha, jboolean>>(vm, "nativeHasAlpha", "(J)Z"),
             Method<SetBitmapAlpha>(vm, "nativeSetHasAlpha", "(JZZ)V"),
             Method<BitmapPremultiplied>(vm, "nativeIsPremultiplied", "(J)Z"),
             Method<BitmapSet<&BitmapData::premultiplied, jboolean>>(vm, "nativeSetPremultiplied", "(JZ)V"),
             Method<BitmapGet<&BitmapData::mipmap, jboolean>>(vm, "nativeHasMipMap", "(J)Z"),
             Method<BitmapSet<&BitmapData::mipmap, jboolean>>(vm, "nativeSetHasMipMap", "(JZ)V"),
             Method<BitmapGet<&BitmapData::immutable, jboolean>>(vm, "nativeIsImmutable", "(J)Z"),
             Method<SetImmutable>(vm, "nativeSetImmutable", "(J)V"),
             Method<BitmapGet<&BitmapData::source, jlong>>(vm, "nativeGetSourceId", "(J)J"),
             Method<BitmapSet<&BitmapData::source, jlong>>(vm, "nativeSetSourceId", "(JJ)V"),
             Method<BitmapFalse>(vm, "nativeIsBackedByAshmem", "(J)Z"),
             Method<BitmapFalse>(vm, "nativeHasGainmap", "(J)Z"),
             Method<SetBitmapGainmap>(vm, "nativeSetGainmap", "(JJ)V"),
             Method<BitmapSrgb>(vm, "nativeIsSRGB", "(J)Z"),
             Method<BitmapFalse>(vm, "nativeIsSRGBLinear", "(J)Z"),
             Method<BitmapPrepare>(vm, "nativePrepareToDraw", "(J)V"),
             Method<SameBitmaps>(vm, "nativeSameAs", "(JJ)Z"),
             Method<ExtractBitmapAlpha>(vm, "nativeExtractAlpha", "(JJ[I)Landroid/graphics/Bitmap;"),
             Method<CompressBitmap>(vm, "nativeCompress", "(JIILjava/io/OutputStream;[B)Z"));
    Register(vm, "android/graphics/BitmapFactory",
             Method<DecodeByteArray>(vm, "nativeDecodeByteArray", "([BIILandroid/graphics/BitmapFactory$Options;JJ)Landroid/graphics/Bitmap;"),
             Method<DecodeStream>(vm, "nativeDecodeStream", "(Ljava/io/InputStream;[BLandroid/graphics/Rect;Landroid/graphics/BitmapFactory$Options;JJ)Landroid/graphics/Bitmap;"),
             Method<DecodeFileDescriptor>(vm, "nativeDecodeFileDescriptor", "(Ljava/io/FileDescriptor;Landroid/graphics/Rect;Landroid/graphics/BitmapFactory$Options;JJ)Landroid/graphics/Bitmap;"),
             Method<DecodeAsset>(vm, "nativeDecodeAsset", "(JLandroid/graphics/Rect;Landroid/graphics/BitmapFactory$Options;JJ)Landroid/graphics/Bitmap;"),
             Method<DecodeFile>(vm, "nDecodeFile", "(Ljava/lang/String;Landroid/graphics/BitmapFactory$Options;JJ)Landroid/graphics/Bitmap;"),
             Method<Seekable>(vm, "nativeIsSeekable", "(Ljava/io/FileDescriptor;)Z"));
    if (vm.HasClass("Landroid/graphics/NinePatch;"))
        Register(vm, "android/graphics/NinePatch", Method<IsNinePatch>(vm, "isNinePatchChunk", "([B)Z"));
}

} // namespace
