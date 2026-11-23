template <class Text>
jlong ShapeTextRun(JNIEnv* env, jclass, Text input, jint start, jint count,
                   jint context_start, jint context_count, jboolean rtl, jlong paint) {
    return CallSafely(env, [&]() -> jlong {
        const auto* p = Paint(env, paint);
        auto* state = GetState(env);
        if (!p || !state || context_start < 0 || start < context_start || count < 0 ||
            context_count < 0 || int64_t(start) + count > int64_t(context_start) + context_count)
            return 0;
        auto text = [&] {
            if constexpr (std::is_same_v<Text, jstring>)
                return Chars(env, input, start, start + count);
            else
                return Chars(env, input, start, count);
        }();
        if (env->ExceptionCheck()) return 0;
        GlyphRunData result;
        result.metrics = Measure(*state, *p, text);
        result.face = p->typeface ? p->typeface : state->default_typeface;
        result.variation = p->variation;
        result.fake_bold = p->flags & 0x20;
        for (jint i = 0; static_cast<size_t>(i) < text.size(); i = NextCodePoint(text, i))
            result.glyphs.push_back({static_cast<jint>(CodePoint(text, i)), 0});
        if (rtl) std::ranges::reverse(result.glyphs);
        float cursor = 0;
        for (auto& glyph : result.glyphs) {
            glyph.x = cursor;
            std::array<char16_t, 2> chars{};
            size_t length = 1;
            if (glyph.codepoint <= 0xffff) {
                chars[0] = glyph.codepoint;
            } else {
                const auto offset = glyph.codepoint - 0x10000;
                chars = {char16_t(0xd800 + (offset >> 10)), char16_t(0xdc00 + (offset & 0x3ff))};
                length = 2;
            }
            cursor += Measure(*state, *p, {chars.data(), length}).advance;
        }
        return wa::ui_jni::detail::Peers<GlyphRunData, &State::glyph_runs>(*state).Insert(std::move(result));
    });
}

const GlyphRunData* GlyphRun(JNIEnv* env, jlong id) {
    return Peer<GlyphRunData, &State::glyph_runs>(env, id);
}
const GlyphRunData::Glyph* GlyphAt(JNIEnv* env, jlong id, jint index) {
    const auto* run = GlyphRun(env, id);
    if (!run) return nullptr;
    if (index < 0 || static_cast<size_t>(index) >= run->glyphs.size()) {
        Throw(env, "java/lang/IndexOutOfBoundsException", "Positioned glyph index");
        return nullptr;
    }
    return &run->glyphs[index];
}
template <auto Member, class T>
T GlyphValue(JNIEnv* env, jclass, jlong id, jint index) {
    const auto* glyph = GlyphAt(env, id, index);
    return glyph ? glyph->*Member : T{};
}
template <auto Member>
jfloat GlyphMetric(JNIEnv* env, jclass, jlong id) {
    const auto* run = GlyphRun(env, id);
    if (!run) return 0;
    const auto value = run->metrics.*Member;
    return Member == &wa_text_metrics_t::ascent ? -value : value;
}
jint GlyphCount(JNIEnv* env, jclass, jlong id) {
    const auto* run = GlyphRun(env, id);
    return run ? run->glyphs.size() : 0;
}
jint GlyphFontCount(JNIEnv* env, jclass, jlong id) { return GlyphCount(env, nullptr, id) != 0; }
jlong GlyphFont(JNIEnv* env, jclass, jlong id, jint) {
    const auto* run = GlyphRun(env, id);
    return run ? run->face : 0;
}
jint GlyphFontId(JNIEnv*, jclass, jlong, jint) { return 0; }
jfloat GlyphY(JNIEnv*, jclass, jlong, jint) { return 0; }
jboolean GlyphFakeBold(JNIEnv* env, jclass, jlong id, jint) {
    const auto* run = GlyphRun(env, id);
    return run && run->fake_bold;
}
jboolean GlyphFakeItalic(JNIEnv*, jclass, jlong, jint) { return false; }
jfloat GlyphWeightOverride(JNIEnv* env, jclass, jlong id, jint) {
    const auto* run = GlyphRun(env, id);
    return run && run->variation.weight ? float(run->variation.weight) : -1.f;
}
jfloat GlyphItalicOverride(JNIEnv* env, jclass, jlong id, jint) {
    const auto* run = GlyphRun(env, id);
    return run && run->variation.italic ? float(*run->variation.italic) : -1.f;
}
jlong GlyphRunFinalizer(JNIEnv*, jclass) { return kFinalizeGlyphRun; }

void RegisterGlyphRuns(interp::Vm& vm) {
    if (vm.HasClass("Landroid/graphics/text/TextRunShaper;"))
        Register(vm, "android/graphics/text/TextRunShaper",
                 Method<ShapeTextRun<jstring>>(vm, "nativeShapeTextRun", "(Ljava/lang/String;IIIIZJ)J"),
                 Method<ShapeTextRun<jcharArray>>(vm, "nativeShapeTextRun", "([CIIIIZJ)J"));
    if (vm.HasClass("Landroid/graphics/text/PositionedGlyphs;"))
        Register(vm, "android/graphics/text/PositionedGlyphs",
                 Method<GlyphCount>(vm, "nGetGlyphCount", "(J)I"),
                 Method<GlyphMetric<&wa_text_metrics_t::advance>>(vm, "nGetTotalAdvance", "(J)F"),
                 Method<GlyphMetric<&wa_text_metrics_t::ascent>>(vm, "nGetAscent", "(J)F"),
                 Method<GlyphMetric<&wa_text_metrics_t::descent>>(vm, "nGetDescent", "(J)F"),
                 Method<GlyphValue<&GlyphRunData::Glyph::codepoint, jint>>(vm, "nGetGlyphId", "(JI)I"),
                 Method<GlyphValue<&GlyphRunData::Glyph::x, jfloat>>(vm, "nGetX", "(JI)F"),
                 Method<GlyphY>(vm, "nGetY", "(JI)F"),
                 Method<GlyphFont>(vm, "nGetFont", "(JI)J"),
                 Method<GlyphFont>(vm, "nGetFontRef", "(JI)J"),
                 Method<GlyphFontId>(vm, "nGetFontId", "(JI)I"),
                 Method<GlyphFontCount>(vm, "nGetFontCount", "(J)I"),
                 Method<GlyphFakeBold>(vm, "nGetFakeBold", "(JI)Z"),
                 Method<GlyphFakeItalic>(vm, "nGetFakeItalic", "(JI)Z"),
                 Method<GlyphWeightOverride>(vm, "nGetWeightOverride", "(JI)F"),
                 Method<GlyphItalicOverride>(vm, "nGetItalicOverride", "(JI)F"),
                 Method<GlyphRunFinalizer>(vm, "nReleaseFunc", "()J"));
}
