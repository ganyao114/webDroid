namespace {

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
            } else {
                const auto edge = MapPoint(*m, geometry[0] + geometry[2], geometry[1]);
                gradient.geometry = {start[0], start[1], std::hypot(edge[0] - start[0], edge[1] - start[1]), 0};
            }
        }
        return Peers<GradientData, &State::gradients>(*state).Insert(std::move(gradient));
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
void ReleaseShader(JNIEnv* env, jclass, jlong id) {
    if (auto* state = GetState(env); state && state->gradients) state->gradients->Erase(id);
}
jobject RetainShader(JNIEnv* env, jclass cls, jobject peer, jlong id) {
    RetainPeer<GradientData, &State::gradients>(env, cls, peer, id);
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

uint32_t FilterColor(const PaintData& paint, uint32_t color) {
    const auto handle = static_cast<uint64_t>(paint.color_filter);
    if (!handle) return color;
    const uint32_t mode = (handle >> 32) - 1;
    const auto tint = static_cast<uint32_t>(handle);
    if (mode == 5) { // SRC_IN
        const uint32_t alpha = ((tint >> 24) * (color >> 24) + 127) / 255;
        return tint & 0xffffffu | alpha << 24;
    }
    if (mode == 9 && tint >> 24 == 255) { // Opaque SRC_ATOP
        return tint & 0xffffffu | color & 0xff000000u;
    }
    return color;
}

void RegisterPaintEffects(interp::Vm& vm) {
    if (RegisterIfDeclared(vm, "android/graphics/Color", Method<RgbToHsv>(vm, "nativeRGBToHSV", "(III[F)V")))
        Register(vm, "android/graphics/Color", Method<HsvToColor>(vm, "nativeHSVToColor", "(I[F)I"));
    if (vm.HasClass("Landroid/graphics/LinearGradient;") &&
        RegisterIfDeclared(vm, "android/graphics/LinearGradient",
            Method<CreateLinearGradient>(vm, "nativeCreate", "(JFFFF[J[FIJ)J"))) {
        Register(vm, "android/graphics/Shader",
                 Method<RetainShader>(vm, "nRegisterNativeAllocation",
                                      "(Landroid/graphics/Shader;J)Ljava/lang/Runnable;"));
    }
    if (vm.HasClass("Landroid/graphics/RadialGradient;"))
        RegisterIfDeclared(vm, "android/graphics/RadialGradient",
            Method<CreateRadialGradient>(vm, "nativeCreate", "(JFFFFFF[J[FIJ)J"));
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
