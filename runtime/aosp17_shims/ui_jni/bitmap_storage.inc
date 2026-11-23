// The software store holds unpremultiplied ARGB. Buffer copies expose the
// packed Android pixel representation, including premultiplication.
uint16_t FloatToHalf(float component) {
    const auto bits = std::bit_cast<uint32_t>(component);
    const int exponent = int((bits >> 23) & 255) - 112;
    if (exponent <= 0) {
        return static_cast<uint16_t>(std::lround(std::ldexp(component, 24)));
    }
    return static_cast<uint16_t>((exponent << 10) + ((bits & 0x7fffff) + 0x1000) / 0x2000);
}

float BitmapHalfToFloat(uint32_t component) {
    const int exponent = (component >> 10) & 31;
    if (component & 0x8000) return 0.f;
    if (exponent == 31) return (component & 1023) ? 0.f : 1.f;
    const float fraction = (component & 1023) / 1024.f;
    return exponent ? std::ldexp(1 + fraction, exponent - 15) : std::ldexp(fraction, -14);
}

uint64_t PackBitmapPixel(const BitmapData& bitmap, uint32_t color) {
    const uint32_t a = bitmap.has_alpha ? color >> 24 : 255;
    const auto component = [&](unsigned shift) {
        const uint32_t c = (color >> shift) & 255;
        return bitmap.premultiplied ? (c * a + 127) / 255 : c;
    };
    const auto r = component(16), g = component(8), b = component(0);
    switch (bitmap.config) {
    case 1: return a;
    case 3: return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
    case 6:
        return uint64_t(FloatToHalf(r / 255.f)) | uint64_t(FloatToHalf(g / 255.f)) << 16 |
               uint64_t(FloatToHalf(b / 255.f)) << 32 | uint64_t(FloatToHalf(a / 255.f)) << 48;
    case 8:
        return (r * 1023 + 127) / 255 | ((g * 1023 + 127) / 255 << 10) |
               ((b * 1023 + 127) / 255 << 20) | ((a * 3 + 127) / 255 << 30);
    default: return r | g << 8 | b << 16 | a << 24;
    }
}

jint UnpackBitmapPixel(const BitmapData& bitmap, uint64_t packed) {
    const auto output = [&](float r, float g, float b, float alpha) {
        if (!bitmap.has_alpha) alpha = 1;
        const float scale = bitmap.premultiplied && alpha > 0 ? 1 / alpha : 1;
        const auto byte = [](float value) { return uint32_t(std::lround(std::clamp(value, 0.f, 1.f) * 255)); };
        return bitmap.PutColor(byte(alpha) << 24 | byte(r * scale) << 16 | byte(g * scale) << 8 | byte(b * scale));
    };
    switch (bitmap.config) {
    case 1: return static_cast<jint>((packed & 255) << 24);
    case 3: {
        const auto r = (packed >> 11) & 31, g = (packed >> 5) & 63, b = packed & 31;
        return bitmap.PutColor(0xff000000u | uint32_t((r << 3) | (r >> 2)) << 16 |
                               uint32_t((g << 2) | (g >> 4)) << 8 | uint32_t((b << 3) | (b >> 2)));
    }
    case 6:
        return output(BitmapHalfToFloat(packed & 0xffff), BitmapHalfToFloat((packed >> 16) & 0xffff),
                      BitmapHalfToFloat((packed >> 32) & 0xffff), BitmapHalfToFloat(packed >> 48));
    case 8:
        return output((packed & 1023) / 1023.f, ((packed >> 10) & 1023) / 1023.f,
                      ((packed >> 20) & 1023) / 1023.f, (packed >> 30) / 3.f);
    default:
        return output((packed & 255) / 255.f, ((packed >> 8) & 255) / 255.f,
                      ((packed >> 16) & 255) / 255.f, (packed >> 24) / 255.f);
    }
}

template <bool FromBuffer>
void CopyBitmapBuffer(JNIEnv* env, jclass, jlong id, jobject buffer) {
    CallSafely(env, [&] {
        auto* bitmap = Bitmap(env, id);
        const auto type = env->GetObjectClass(buffer);
        if (!bitmap || !type || bitmap->recycled) return;
        const auto* cls = &interp::JniBridge::From(env).AsClass(type);
        jint element_bytes = 0;
        const char* element = nullptr;
        const char* buffer_return = nullptr;
        for (; cls; cls = cls->super) {
            if (cls->descriptor == "Ljava/nio/ByteBuffer;") {
                element_bytes = 1; element = "(I)B"; buffer_return = "(IB)Ljava/nio/ByteBuffer;"; break;
            }
            if (cls->descriptor == "Ljava/nio/ShortBuffer;") {
                element_bytes = 2; element = "(I)S"; buffer_return = "(IS)Ljava/nio/ShortBuffer;"; break;
            }
            if (cls->descriptor == "Ljava/nio/IntBuffer;") {
                element_bytes = 4; element = "(I)I"; buffer_return = "(II)Ljava/nio/IntBuffer;"; break;
            }
        }
        if (!element_bytes || (FromBuffer && bitmap->immutable)) {
            Throw(env, "java/lang/IllegalArgumentException", "Incompatible bitmap buffer");
            return;
        }
        const auto position = env->GetMethodID(type, "position", "()I");
        const auto remaining = env->GetMethodID(type, "remaining", "()I");
        const auto transfer = env->GetMethodID(type, FromBuffer ? "get" : "put", FromBuffer ? element : buffer_return);
        if (!position || !remaining || !transfer) return;
        const auto first = env->CallIntMethod(buffer, position);
        const auto count = env->CallIntMethod(buffer, remaining);
        if (env->ExceptionCheck()) return;
        const size_t stride = BytesPerPixel(bitmap->config);
        const size_t byte_count = bitmap->pixels.size() * stride;
        if (count < 0 || byte_count > size_t(count) * element_bytes) {
            Throw(env, "java/lang/RuntimeException", "Buffer is too small for pixels");
            return;
        }
        bool little_endian = true;
        if (element_bytes > 1) {
            const auto order = env->GetMethodID(type, "order", "()Ljava/nio/ByteOrder;");
            const auto order_type = env->FindClass("java/nio/ByteOrder");
            if (!order || !order_type) return;
            const auto little = env->GetStaticFieldID(order_type, "LITTLE_ENDIAN", "Ljava/nio/ByteOrder;");
            if (!little) return;
            little_endian = env->IsSameObject(env->CallObjectMethod(buffer, order),
                                              env->GetStaticObjectField(order_type, little));
            if (env->ExceptionCheck()) return;
        }

        std::vector<uint8_t> bytes(byte_count);
        if constexpr (!FromBuffer) {
            for (size_t i = 0; i < bitmap->pixels.size(); ++i) {
                auto packed = PackBitmapPixel(*bitmap, bitmap->pixels[i]);
                for (size_t b = 0; b < stride; ++b, packed >>= 8) bytes[i * stride + b] = packed;
            }
        }
        // A partial final element is possible for ALPHA_8 and RGB_565.
        for (size_t i = 0; i < byte_count; i += element_bytes) {
            const auto index = first + static_cast<jint>(i / element_bytes);
            const auto length = std::min<size_t>(element_bytes, byte_count - i);
            jvalue args[2]{};
            args[0].i = index;
            const auto read = [&](jmethodID get) -> uint32_t {
                if (element_bytes == 1) return uint8_t(env->CallByteMethodA(buffer, get, args));
                if (element_bytes == 2) return uint16_t(env->CallShortMethodA(buffer, get, args));
                return uint32_t(env->CallIntMethodA(buffer, get, args));
            };
            const auto bit = [&](size_t b) { return 8 * (little_endian ? b : element_bytes - b - 1); };
            if constexpr (FromBuffer) {
                const auto packed = read(transfer);
                for (size_t b = 0; b < length; ++b) bytes[i + b] = packed >> bit(b);
            } else {
                uint32_t packed = 0;
                if (length < size_t(element_bytes)) {
                    const auto get = env->GetMethodID(type, "get", element);
                    if (!get) return;
                    packed = read(get);
                }
                for (size_t b = 0; b < length; ++b) {
                    const auto shift = bit(b);
                    packed = (packed & ~(255u << shift)) | uint32_t(bytes[i + b]) << shift;
                }
                if (element_bytes == 1) args[1].b = packed;
                else if (element_bytes == 2) args[1].s = packed;
                else args[1].i = packed;
                env->DeleteLocalRef(env->CallObjectMethodA(buffer, transfer, args));
            }
            if (env->ExceptionCheck()) return;
        }
        if constexpr (FromBuffer) {
            for (size_t i = 0; i < bitmap->pixels.size(); ++i) {
                uint64_t packed = 0;
                for (size_t b = 0; b < stride; ++b) packed |= uint64_t(bytes[i * stride + b]) << (8 * b);
                bitmap->pixels[i] = UnpackBitmapPixel(*bitmap, packed);
            }
            bitmap->Changed();
        }
    });
}

jboolean WriteBitmapParcel(JNIEnv* env, jclass, jlong id, jint density, jobject parcel) {
    return CallSafely(env, [&]() -> jboolean {
        const auto* bitmap = Bitmap(env, id);
        const auto type = env->GetObjectClass(parcel);
        if (!bitmap || bitmap->recycled || !type) return false;
        const auto ints = env->GetMethodID(type, "writeIntArray", "([I)V");
        const auto wide = env->GetMethodID(type, "writeLong", "(J)V");
        if (!ints || !wide) return false;
        const jint flags = bitmap->has_alpha | (bitmap->premultiplied << 1) |
                           (bitmap->immutable << 2) | (bitmap->mipmap << 3);
        const std::array header{bitmap->width, bitmap->height, bitmap->config, density, flags};
        const auto info = env->NewIntArray(header.size());
        const auto data = env->NewIntArray(bitmap->pixels.size());
        if (!info || !data) return false;
        env->SetIntArrayRegion(info, 0, header.size(), header.data());
        env->SetIntArrayRegion(data, 0, bitmap->pixels.size(), bitmap->pixels.data());
        if (env->ExceptionCheck()) return false;
        env->CallVoidMethod(parcel, ints, info);
        if (!env->ExceptionCheck()) env->CallVoidMethod(parcel, wide, bitmap->color_space);
        if (!env->ExceptionCheck()) env->CallVoidMethod(parcel, ints, data);
        return !env->ExceptionCheck();
    });
}

jobject ReadBitmapParcel(JNIEnv* env, jclass, jobject parcel) {
    return CallSafely(env, [&]() -> jobject {
        const auto type = env->GetObjectClass(parcel);
        const auto ints = type ? env->GetMethodID(type, "createIntArray", "()[I") : nullptr;
        const auto wide = type ? env->GetMethodID(type, "readLong", "()J") : nullptr;
        if (!ints || !wide) return nullptr;
        const auto info = static_cast<jintArray>(env->CallObjectMethod(parcel, ints));
        std::array<jint, 5> header{};
        if (info && env->GetArrayLength(info) == header.size())
            env->GetIntArrayRegion(info, 0, header.size(), header.data());
        if (env->ExceptionCheck()) return nullptr;
        const auto [width, height, config, density, flags] = header;
        if (!ValidSize(width, height) || !ValidConfig(config)) {
            Throw(env, "android/os/BadParcelableException", "Invalid bitmap parcel header");
            return nullptr;
        }
        const auto color_space = env->CallLongMethod(parcel, wide);
        const auto data = env->ExceptionCheck() ? nullptr : static_cast<jintArray>(env->CallObjectMethod(parcel, ints));
        if (env->ExceptionCheck()) return nullptr;
        if (!data || env->GetArrayLength(data) != width * height) {
            Throw(env, "android/os/BadParcelableException", "Invalid bitmap parcel pixels");
            return nullptr;
        }
        auto bitmap = NewBitmapData(width, height, config, !(flags & 4), flags & 1, flags & 2);
        bitmap.mipmap = flags & 8;
        bitmap.color_space = color_space;
        env->GetIntArrayRegion(data, 0, width * height, bitmap.pixels.data());
        return env->ExceptionCheck() ? nullptr : WrapBitmap(env, std::move(bitmap), density);
    });
}

jobject CreateSharedBitmap(JNIEnv* env, jclass type, jint width, jint height, jint config, jlong color) {
    return CreateBitmap(env, type, nullptr, 0, 0, width, height, config, true, color);
}
jobject CopySharedBitmap(JNIEnv* env, jclass type, jlong id, jint config) {
    return CopyBitmap(env, type, id, config, false);
}
jint BitmapSharedFd(JNIEnv*, jclass, jlong) { return -1; }
// Software bitmaps have no hardware buffer or HDR gainmap attached.
jobject BitmapPlatformBuffer(JNIEnv*, jclass, jlong) { return nullptr; }
jobject WrapPlatformBuffer(JNIEnv*, jclass, jobject, jlong) { return nullptr; }

constexpr jlong kLastNamedColorSpaceHandle = 65;

jobject NewRgbColorSpace(JNIEnv* env, const ColorSpaceData& data) {
    const auto transfer_type = env->FindClass("android/graphics/ColorSpace$Rgb$TransferParameters");
    const auto rgb_type = env->FindClass("android/graphics/ColorSpace$Rgb");
    if (!transfer_type || !rgb_type) return nullptr;
    const auto transfer_ctor = env->GetMethodID(transfer_type, "<init>", "(DDDDDDD)V");
    const auto rgb_ctor = env->GetMethodID(rgb_type, "<init>",
        "(Ljava/lang/String;[FLandroid/graphics/ColorSpace$Rgb$TransferParameters;)V");
    if (!transfer_ctor || !rgb_ctor) return nullptr;
    std::array<jvalue, 7> args{};
    for (size_t i = 0; i < args.size(); ++i) args[i].d = data.transfer[i];
    const auto transfer = env->NewObjectA(transfer_type, transfer_ctor, args.data());
    if (!transfer) return nullptr;
    const auto matrix = env->NewFloatArray(data.to_xyz.size());
    if (!matrix) return nullptr;
    env->SetFloatArrayRegion(matrix, 0, data.to_xyz.size(), data.to_xyz.data());
    const auto name = env->NewStringUTF("Bitmap RGB");
    const jvalue rgb_args[]{{.l = name}, {.l = matrix}, {.l = transfer}};
    return env->ExceptionCheck() ? nullptr : env->NewObjectA(rgb_type, rgb_ctor, rgb_args);
}

jobject BitmapColorSpace(JNIEnv* env, jclass, jlong id) {
    const auto* bitmap = Bitmap(env, id);
    if (!bitmap || bitmap->config == 1) return nullptr;
    if (bitmap->color_space > kLastNamedColorSpaceHandle) {
        const auto& store = GetState(env)->color_spaces;
        if (const auto* data = store ? store->Find(bitmap->color_space - kLastNamedColorSpaceHandle) : nullptr)
            return NewRgbColorSpace(env, *data);
    }
    const auto cls = env->FindClass("android/graphics/ColorSpace");
    const auto get = cls ? env->GetStaticMethodID(cls, "get", "(I)Landroid/graphics/ColorSpace;") : nullptr;
    const jint named = bitmap->color_space >= 2 && bitmap->color_space <= kLastNamedColorSpaceHandle
        ? bitmap->color_space - 2 : 0;
    return get ? env->CallStaticObjectMethod(cls, get, named) : nullptr;
}
void SetBitmapColorSpace(JNIEnv* env, jclass, jlong id, jlong color) {
    if (auto* bitmap = Bitmap(env, id)) bitmap->color_space = color;
}
jlong CreateRgbColorSpace(JNIEnv* env, jclass, jfloat a, jfloat b, jfloat c, jfloat d, jfloat e,
                          jfloat f, jfloat g, jfloatArray xyz) {
    return CallSafely(env, [&]() -> jlong {
        auto* state = GetState(env);
        ColorSpaceData data{{a, b, c, d, e, f, g}, {}};
        env->GetFloatArrayRegion(xyz, 0, data.to_xyz.size(), data.to_xyz.data());
        // Handles 1..65 are reserved by the framework's named-color-space encoding.
        return state && !env->ExceptionCheck() ?
            kLastNamedColorSpaceHandle + Peers<ColorSpaceData, &State::color_spaces>(*state).Insert(data) : 0;
    });
}
