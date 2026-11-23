namespace {

using ShaderPeer = std::shared_ptr<const ShaderData>;

jlong StoreShader(State& state, ShaderData shader) {
    return Peers<ShaderPeer, &State::shaders>(state).Insert(
        std::make_shared<const ShaderData>(std::move(shader)));
}

ShaderPeer GetShader(JNIEnv* env, jlong handle) {
    if (handle == 0) return {};
    const auto* peer = Peer<ShaderPeer, &State::shaders>(env, handle);
    return peer ? *peer : nullptr;
}

jlong SetPaintShader(JNIEnv* env, jclass, jlong paint, jlong handle) {
    auto* target = Paint(env, paint);
    if (!target) return 0;
    auto shader = GetShader(env, handle);
    if (env->ExceptionCheck()) return 0;
    target->shader = std::move(shader);
    return std::exchange(target->shader_handle, handle);
}

jlong CreateBitmapShader(JNIEnv* env, jclass, jlong matrix, jlong bitmap, jint tile_x, jint tile_y,
                         jint max_aniso, jboolean filter, jboolean, jlong) {
    return CallSafely(env, [&]() -> jlong {
        const auto* image = Peer<std::shared_ptr<BitmapData>, &State::bitmaps>(env, bitmap);
        const auto* local = matrix ? MatrixValues(env, matrix) : &kIdentity;
        if (!image || !local || (*image)->recycled) return 0;
        return StoreShader(*GetState(env),
                           {BitmapShaderData{*image, tile_x, tile_y, filter || max_aniso > 0},
                            Inverse(*local)});
    });
}

jlong CreateComposeShader(JNIEnv* env, jclass, jlong matrix, jlong a, jlong b, jint mode) {
    return CallSafely(env, [&]() -> jlong {
        auto destination = GetShader(env, a);
        auto source = GetShader(env, b);
        const auto* local = matrix ? MatrixValues(env, matrix) : &kIdentity;
        if (!destination || !source || !local) return 0;
        return StoreShader(*GetState(env),
                           {ComposeShaderData{std::move(destination), std::move(source), mode},
                            Inverse(*local)});
    });
}

jlong StoreGradient(JNIEnv* env, jlong matrix, jint kind, jint tile, std::array<float, 4> geometry,
                    jlongArray colors, jfloatArray positions) {
    return CallSafely(env, [&]() -> jlong {
        auto* state = GetState(env);
        if (!state || !colors) return 0;
        const auto count = env->GetArrayLength(colors);
        if (count < 2) return 0;
        GradientData gradient{kind, tile, geometry, std::vector<jfloat>(count), std::vector<jint>(count)};
        if (positions) {
            env->GetFloatArrayRegion(positions, 0, count, gradient.positions.data());
        } else {
            for (jint i = 0; i < count; ++i) gradient.positions[i] = float(i) / (count - 1);
        }
        std::vector<jlong> values(count);
        env->GetLongArrayRegion(colors, 0, count, values.data());
        if (env->ExceptionCheck()) return 0;
        std::ranges::transform(values, gradient.colors.begin(), [](jlong c) { return UnpackColor(c); });
        if (matrix) {
            const auto* m = MatrixValues(env, matrix);
            if (!m) return 0;
            const auto start = MapPoint(*m, geometry[0], geometry[1]);
            if (kind == 1) {
                const auto end = MapPoint(*m, geometry[2], geometry[3]);
                gradient.geometry = {start[0], start[1], end[0], end[1]};
            } else if (kind == 2) {
                const auto edge = MapPoint(*m, geometry[0] + geometry[2], geometry[1]);
                gradient.geometry = {start[0], start[1], std::hypot(edge[0] - start[0], edge[1] - start[1]), 0};
            } else {
                gradient.geometry = {start[0], start[1], 0, 0};
            }
        }
        return StoreShader(*state, {std::move(gradient)});
    });
}

jlong CreateLinearGradient(JNIEnv* env, jobject, jlong matrix, jfloat x, jfloat y, jfloat ex,
                           jfloat ey, jlongArray colors, jfloatArray positions, jint tile, jlong) {
    return StoreGradient(env, matrix, 1, tile, {x, y, ex, ey}, colors, positions);
}
jlong CreateRadialGradient(JNIEnv* env, jclass, jlong matrix, jfloat, jfloat, jfloat, jfloat x,
                           jfloat y, jfloat radius, jlongArray colors, jfloatArray positions, jint tile, jlong) {
    return StoreGradient(env, matrix, 2, tile, {x, y, radius, 0}, colors, positions);
}
jlong CreateSweepGradient(JNIEnv* env, jclass, jlong matrix, jfloat x, jfloat y,
                          jlongArray colors, jfloatArray positions, jlong) {
    return StoreGradient(env, matrix, 3, 0, {x, y, 0, 0}, colors, positions);
}
void ReleaseShader(JNIEnv* env, jclass, jlong id) {
    if (auto* state = GetState(env); state && state->shaders) state->shaders->Erase(id);
}
jobject RetainShader(JNIEnv* env, jclass cls, jobject peer, jlong id) {
    RetainPeer<ShaderPeer, &State::shaders>(env, cls, peer, id);
    if (env->ExceptionCheck()) return nullptr;
    const auto type = env->FindClass("android/graphics/Canvas$ShaderRelease");
    if (!type) return nullptr;
    const auto ctor = env->GetMethodID(type, "<init>", "(J)V");
    return ctor ? env->NewObject(type, ctor, id) : nullptr;
}

jlong CreateBlurMask(JNIEnv*, jclass, jfloat radius, jint style) {
    if (!std::isfinite(radius) || radius <= 0) return 0;
    return jlong(uint32_t(style + 1)) << 32 | std::bit_cast<uint32_t>(radius);
}
void ReleaseMask(JNIEnv*, jclass, jlong) {
    // Blur mask filters carry their parameters in the handle value.
}
jlong CreateBlendFilter(JNIEnv*, jclass, jint color, jint mode) {
    return jlong(uint32_t(mode + 1)) << 32 | uint32_t(color);
}

void RgbToHsv(JNIEnv* env, jclass, jint red, jint green, jint blue, jfloatArray hsv) {
    const float r = std::clamp(red, 0, 255) / 255.f;
    const float g = std::clamp(green, 0, 255) / 255.f;
    const float b = std::clamp(blue, 0, 255) / 255.f;
    const float value = std::max({r, g, b});
    const float delta = value - std::min({r, g, b});
    float hue = 0;
    if (delta > 0) {
        if (value == r) hue = (g - b) / delta;
        else if (value == g) hue = 2 + (b - r) / delta;
        else hue = 4 + (r - g) / delta;
    }
    const jfloat result[] = {(hue < 0 ? hue + 6 : hue) * 60.f, value > 0 ? delta / value : 0.f, value};
    env->SetFloatArrayRegion(hsv, 0, 3, result);
}

jint HsvToColor(JNIEnv* env, jclass, jint alpha, jfloatArray hsv) {
    jfloat input[3];
    env->GetFloatArrayRegion(hsv, 0, 3, input);
    if (env->ExceptionCheck()) return 0;
    const float hue = input[0] >= 0 && input[0] < 360 ? input[0] / 60.f : 0;
    const float saturation = std::isnan(input[1]) ? 0 : std::clamp(input[1], 0.f, 1.f);
    const float value = std::isnan(input[2]) ? 0 : std::clamp(input[2], 0.f, 1.f);
    const int index = static_cast<int>(hue);
    const float fraction = hue - index;
    const std::array levels{value, value * (1 - saturation), value * (1 - saturation * fraction),
                            value * (1 - saturation * (1 - fraction))};
    constexpr int components[6][3] = {{0, 3, 1}, {2, 0, 1}, {1, 0, 3}, {1, 2, 0}, {3, 1, 0}, {0, 1, 2}};
    const auto& rgb = components[index];
    const auto channel = [&](int component) { return uint32_t(std::lround(levels[component] * 255.f)); };
    return static_cast<jint>(uint32_t(alpha) << 24 | channel(rgb[0]) << 16 | channel(rgb[1]) << 8 | channel(rgb[2]));
}

using Rgb = std::array<float, 3>;

float Luminance(const Rgb& bgr) {
    return 0.11f * bgr[0] + 0.59f * bgr[1] + 0.3f * bgr[2];
}
float Saturation(const Rgb& bgr) {
    const auto [dark, light] = std::ranges::minmax(bgr);
    return light - dark;
}
Rgb WithSaturation(Rgb bgr, float target) {
    const auto [dark, light] = std::ranges::minmax(bgr);
    if (dark == light) return {};
    for (auto& channel : bgr) channel = (channel - dark) * target / (light - dark);
    return bgr;
}
Rgb WithLuminance(Rgb bgr, float target) {
    const auto delta = target - Luminance(bgr);
    for (auto& channel : bgr) channel += delta;
    const auto [dark, light] = std::ranges::minmax(bgr);
    if (dark < 0) {
        for (auto& channel : bgr) channel = target + (channel - target) * target / (target - dark);
    }
    if (light > 1) {
        for (auto& channel : bgr) channel = target + (channel - target) * (1 - target) / (light - target);
    }
    return bgr;
}
Rgb BlendHsl(Rgb src, Rgb dst, jint mode) {
    switch (mode) {
    case 25: return WithLuminance(WithSaturation(src, Saturation(dst)), Luminance(dst));
    case 26: return WithLuminance(WithSaturation(dst, Saturation(src)), Luminance(dst));
    case 27: return WithLuminance(src, Luminance(dst));
    default: return WithLuminance(dst, Luminance(src));
    }
}

uint32_t BlendColor(uint32_t tint, uint32_t color, jint mode) {
    if (mode == 5) { // SRC_IN
        const uint32_t alpha = ((tint >> 24) * (color >> 24) + 127) / 255;
        return tint & 0xffffffu | alpha << 24;
    }
    const float sa = (tint >> 24) / 255.f, da = (color >> 24) / 255.f;
    float src = 1.f, dst = 1.f;
    switch (mode) {
    case 0: return 0; // CLEAR
    case 1: return tint;
    case 2: return color;
    case 3: return interp::software_raster::SourceOver(tint, color);
    case 4: return interp::software_raster::SourceOver(color, tint);
    case 6: src = 0; dst = sa; break;
    case 7: src = 1 - da; dst = 0; break;
    case 8: src = 0; dst = 1 - sa; break;
    case 9: src = da; dst = 1 - sa; break;
    case 10: src = 1 - da; dst = sa; break;
    case 11: src = 1 - da; dst = 1 - sa; break;
    default: break;
    }
    const float alpha = mode == 13 ? sa * da : mode >= 14 ? sa + da - sa * da
                                                                : std::min(1.f, src * sa + dst * da);
    if (!(alpha > 0)) return 0;
    Rgb hsl{};
    if (mode >= 25 && mode <= 28) {
        const auto channels = [](uint32_t pixel) -> Rgb {
            return {(pixel & 255) / 255.f, ((pixel >> 8) & 255) / 255.f, ((pixel >> 16) & 255) / 255.f};
        };
        hsl = BlendHsl(channels(tint), channels(color), mode);
    }
    uint32_t result = static_cast<uint32_t>(std::lround(255 * alpha)) << 24;
    for (unsigned shift : {0u, 8u, 16u}) {
        const float s = ((tint >> shift) & 255) / 255.f, d = ((color >> shift) & 255) / 255.f;
        float value = s * src * sa + d * dst * da;
        if (mode == 13) {
            value = s * d * alpha;
        } else if (mode >= 14) {
            float blend = s;
            switch (mode) {
            case 14: blend = s + d - s * d; break;
            case 15: blend = d < .5f ? 2 * s * d : 1 - 2 * (1 - s) * (1 - d); break;
            case 16: blend = std::min(s, d); break;
            case 17: blend = std::max(s, d); break;
            case 18: blend = d == 0 ? 0 : s == 1 ? 1 : std::min(1.f, d / (1 - s)); break;
            case 19: blend = d == 1 ? 1 : s == 0 ? 0 : 1 - std::min(1.f, (1 - d) / s); break;
            case 20: blend = s < .5f ? 2 * s * d : 1 - 2 * (1 - s) * (1 - d); break;
            case 21: {
                const float curve = d <= .25f ? ((16 * d - 12) * d + 4) * d : std::sqrt(d);
                blend = s <= .5f ? d - (1 - 2 * s) * d * (1 - d) : d + (2 * s - 1) * (curve - d);
                break;
            }
            case 22: blend = std::abs(d - s); break;
            case 23: blend = s + d - 2 * s * d; break;
            case 24: blend = s * d; break;
            case 25: case 26: case 27: case 28: blend = hsl[shift / 8]; break;
            }
            value = s * sa * (1 - da) + d * da * (1 - sa) + blend * sa * da;
        }
        result |= static_cast<uint32_t>(std::lround(255 * std::clamp(value / alpha, 0.f, 1.f))) << shift;
    }
    return result;
}

uint32_t FilterColor(const PaintData& paint, uint32_t color) {
    const auto handle = static_cast<uint64_t>(paint.color_filter);
    return handle ? BlendColor(static_cast<uint32_t>(handle), color, (handle >> 32) - 1) : color;
}

void RetainColorFilter(JNIEnv*, jclass, jobject, jlong) {
    // Blend filters encode the mode and color in the handle itself.
}

void RegisterPaintEffects(interp::Vm& vm) {
    if (vm.HasClass("Landroid/graphics/ColorFilter;"))
        RegisterIfDeclared(vm, "android/graphics/ColorFilter",
            Method<RetainColorFilter>(vm, "nRegisterNativeAllocation", "(Landroid/graphics/ColorFilter;J)V"));
    if (RegisterIfDeclared(vm, "android/graphics/Color", Method<RgbToHsv>(vm, "nativeRGBToHSV", "(III[F)V")))
        Register(vm, "android/graphics/Color", Method<HsvToColor>(vm, "nativeHSVToColor", "(I[F)I"));
    if (vm.HasClass("Landroid/graphics/Shader;")) {
        RegisterIfDeclared(vm, "android/graphics/Shader",
            Method<RetainShader>(vm, "nRegisterNativeAllocation",
                                 "(Landroid/graphics/Shader;J)Ljava/lang/Runnable;"));
        RegisterIfDeclared(vm, "android/graphics/Shader",
            Method<ManagedFinalizer>(vm, "nativeGetFinalizer", "()J"));
    }
    if (vm.HasClass("Landroid/graphics/BitmapShader;"))
        RegisterIfDeclared(vm, "android/graphics/BitmapShader",
            Method<CreateBitmapShader>(vm, "nativeCreate", "(JJIIIZZJ)J"));
    if (vm.HasClass("Landroid/graphics/ComposeShader;"))
        RegisterIfDeclared(vm, "android/graphics/ComposeShader",
            Method<CreateComposeShader>(vm, "nativeCreate", "(JJJI)J"));
    if (vm.HasClass("Landroid/graphics/LinearGradient;"))
        RegisterIfDeclared(vm, "android/graphics/LinearGradient",
            Method<CreateLinearGradient>(vm, "nativeCreate", "(JFFFF[J[FIJ)J"));
    if (vm.HasClass("Landroid/graphics/RadialGradient;"))
        RegisterIfDeclared(vm, "android/graphics/RadialGradient",
            Method<CreateRadialGradient>(vm, "nativeCreate", "(JFFFFFF[J[FIJ)J"));
    if (vm.HasClass("Landroid/graphics/SweepGradient;"))
        RegisterIfDeclared(vm, "android/graphics/SweepGradient",
            Method<CreateSweepGradient>(vm, "nativeCreate", "(JFF[J[FJ)J"));
    if (vm.HasClass("Landroid/graphics/BlurMaskFilter;") &&
        RegisterIfDeclared(vm, "android/graphics/BlurMaskFilter",
            Method<CreateBlurMask>(vm, "nativeConstructor", "(FI)J"))) {
        RegisterIfDeclared(vm, "android/graphics/MaskFilter",
            Method<ReleaseMask>(vm, "nativeDestructor", "(J)V"));
    }
    if (vm.HasClass("Landroid/graphics/PorterDuffColorFilter;"))
        RegisterIfDeclared(vm, "android/graphics/PorterDuffColorFilter",
            Method<CreateBlendFilter>(vm, "native_CreateBlendModeFilter", "(II)J"));
    if (vm.HasClass("Landroid/graphics/BlendModeColorFilter;"))
        RegisterIfDeclared(vm, "android/graphics/BlendModeColorFilter",
            Method<CreateBlendFilter>(vm, "native_CreateBlendModeFilter", "(II)J"));
}

} // namespace
