struct CanvasPropertyData {
    std::variant<jfloat, PaintData> value;
    size_t references = 0;
};

namespace {

template <class T>
jlong SaveCanvasProperty(JNIEnv* env, const T& value) {
    auto* state = GetState(env);
    return state ? Peers<CanvasPropertyData, &State::canvas_properties>(*state).Insert({value}) : 0;
}
jlong NewFloatProperty(JNIEnv* env, jclass, jfloat value) {
    return CallSafely(env, [&] { return SaveCanvasProperty(env, value); });
}
jlong NewPaintProperty(JNIEnv* env, jclass, jlong paint) {
    return CallSafely(env, [&]() -> jlong {
        const auto* data = Paint(env, paint);
        return data ? SaveCanvasProperty(env, *data) : 0;
    });
}
template <bool Increment>
void RefCanvasProperty(JNIEnv* env, jclass, jlong id) {
    auto* state = GetState(env);
    if (!state || !state->canvas_properties) return;
    const auto& store = state->canvas_properties;
    auto* property = store->Find(id);
    if (!property) return;
    if constexpr (Increment) {
        ++property->references;
    } else if (property->references && --property->references == 0) {
        store->Erase(id);
    }
}

jlong NewFontFamilyBuilder(JNIEnv* env, jclass, jstring locales, jint variant) {
    return CallSafely(env, [&]() -> jlong {
        auto* state = GetState(env);
        if (!state) return 0;
        FontFamilyData family{locales ? Utf8(env, locales) : std::string{}, variant};
        if (env->ExceptionCheck()) return 0;
        return Peers<FontFamilyData, &State::font_families>(*state).Insert(std::move(family));
    });
}
void AddFontAxis(JNIEnv* env, jclass, jlong id, jint tag, jfloat value) {
    CallSafely(env, [&] {
        if (auto* family = Peer<FontFamilyData, &State::font_families>(env, id);
            family && std::isfinite(value))
            family->axes.emplace_back(tag, value);
    });
}

jboolean AddFamilyFont(JNIEnv* env, jclass, jlong id, jobject buffer, jint ttc,
                        jint weight, jint italic) {
    return CallSafely(env, [&]() -> jboolean {
        auto* family = Peer<FontFamilyData, &State::font_families>(env, id);
        if (!family) return false;
        const auto axes = std::exchange(family->axes, {});
        if (!buffer || ttc < 0) return false;
        const auto type = env->GetObjectClass(buffer);
        const auto capacity = env->GetMethodID(type, "capacity", "()I");
        const auto get = env->GetMethodID(type, "get", "(I)B");
        if (!capacity || !get || env->CallIntMethod(buffer, capacity) < 12 || env->ExceptionCheck())
            return false;
        const auto magic_at = [&](jint offset) {
            uint32_t bits = 0;
            for (jint b = 0; b < 4 && !env->ExceptionCheck(); ++b)
                bits = bits << 8 | uint8_t(env->CallByteMethod(buffer, get, offset + b));
            return bits;
        };
        uint32_t magic = magic_at(0);
        if (magic == 0x74746366u) { // ttcf
            const auto count = magic_at(8);
            const auto size = env->CallIntMethod(buffer, capacity);
            if (uint32_t(ttc) >= count || ttc >= (size - 12) / 4) return false;
            const auto offset = magic_at(12 + ttc * 4);
            if (offset > static_cast<uint32_t>(size - 12)) return false;
            magic = magic_at(offset);
        } else if (ttc != 0) {
            return false;
        }
        if (env->ExceptionCheck() || (magic != 0x00010000u && magic != 0x4f54544fu && magic != 0x74727565u))
            return false;
        for (const auto& [tag, value] : axes) {
            if (tag == 0x77676874) weight = std::clamp(value, 1.f, 1000.f);
            if (tag == 0x6974616c) italic = value >= 0.5f;
        }
        family->faces.push_back(EncodeTypeface(WA_FONT_ROBOTO, weight > 0 ? weight : 400, italic > 0));
        return true;
    });
}
jlong FinalizeFamilyBuilder(JNIEnv* env, jclass, jlong id) {
    return CallSafely(env, [&]() -> jlong {
        const auto* family = Peer<FontFamilyData, &State::font_families>(env, id);
        if (!family || family->faces.empty()) return 0;
        return Peers<FontFamilyData, &State::font_families>(*GetState(env)).Insert(*family);
    });
}
jlong FontFamilyFinalizer(JNIEnv*, jclass) { return kFinalizeFontFamily; }

void ReleaseAbsentEffect(JNIEnv*, jclass, jlong) {
    // DrawFilter and PathEffect have no allocating subclasses in this framework build.
}

void RegisterGraphicsCompatibility(interp::Vm& vm) {
    if (vm.HasClass("Landroid/graphics/CanvasProperty;"))
        Register(vm, "android/graphics/CanvasProperty",
                 Method<NewFloatProperty>(vm, "nCreateFloat", "(F)J"),
                 Method<NewPaintProperty>(vm, "nCreatePaint", "(J)J"));
    if (vm.HasClass("Lcom/android/internal/util/VirtualRefBasePtr;"))
        Register(vm, "com/android/internal/util/VirtualRefBasePtr",
                 Method<RefCanvasProperty<true>>(vm, "nIncStrong", "(J)V"),
                 Method<RefCanvasProperty<false>>(vm, "nDecStrong", "(J)V"));
    if (vm.HasClass("Landroid/graphics/FontFamily;"))
        Register(vm, "android/graphics/FontFamily",
                 Method<NewFontFamilyBuilder>(vm, "nInitBuilder", "(Ljava/lang/String;I)J"),
                 Method<FinalizeFamilyBuilder>(vm, "nCreateFamily", "(J)J"),
                 Method<FontFamilyFinalizer>(vm, "nGetBuilderReleaseFunc", "()J"),
                 Method<FontFamilyFinalizer>(vm, "nGetFamilyReleaseFunc", "()J"),
                 Method<AddFamilyFont>(vm, "nAddFont", "(JLjava/nio/ByteBuffer;III)Z"),
                 Method<AddFamilyFont>(vm, "nAddFontWeightStyle", "(JLjava/nio/ByteBuffer;III)Z"),
                 Method<AddFontAxis>(vm, "nAddAxisValue", "(JIF)V"));
    for (const char* type : {"android/graphics/DrawFilter", "android/graphics/PathEffect"}) {
        if (vm.HasClass("L" + std::string(type) + ";"))
            RegisterIfDeclared(vm, type, Method<ReleaseAbsentEffect>(vm, "nativeDestructor", "(J)V"));
    }
}

} // namespace
