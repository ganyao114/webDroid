namespace {

struct UnicodeRange {
    char32_t start;
    jbyte value;
};
struct UnicodeMirror {
    jchar from, to;
};
#include "unicode_properties.inc"

jbyte CharacterProperty(std::span<const UnicodeRange> ranges, char32_t codepoint) {
    return (std::ranges::upper_bound(ranges, codepoint, {}, &UnicodeRange::start) - 1)->value;
}

bool CharacterSlice(JNIEnv* env, jcharArray chars, jint start, jint count) {
    if (!chars) {
        Throw(env, "java/lang/NullPointerException", "Character array");
        return false;
    }
    if (start >= 0 && count >= 0 && start <= env->GetArrayLength(chars) - count)
        return true;
    Throw(env, "java/lang/ArrayIndexOutOfBoundsException", "Character array range");
    return false;
}

void CharacterProperties(JNIEnv* env, jcharArray chars, jint start, jint count, jbyteArray output,
                         std::span<const UnicodeRange> ranges) {
    CallSafely(env, [&] {
        if (!CharacterSlice(env, chars, start, count)) return;
        if (!output) {
            Throw(env, "java/lang/NullPointerException", "Character properties destination");
            return;
        }
        if (env->GetArrayLength(output) < count) {
            Throw(env, "java/lang/ArrayIndexOutOfBoundsException", "Character properties destination");
            return;
        }
        const auto text = Chars(env, chars, start, count);
        if (env->ExceptionCheck()) return;
        std::vector<jbyte> result(count);
        for (jint i = 0; i < count;) {
            const auto codepoint = text_layout::CodePoint(text, i);
            const auto value = CharacterProperty(ranges, codepoint);
            result[i++] = value;
            if (codepoint >= 0x10000) result[i++] = value;
        }
        env->SetByteArrayRegion(output, 0, count, result.data());
    });
}

void CharacterDirections(JNIEnv* env, jclass, jcharArray chars, jbyteArray output, jint count) {
    CharacterProperties(env, chars, 0, count, output, kDirectionality);
}
jint CharacterWidth(JNIEnv*, jclass, jchar ch) {
    return CharacterProperty(kEastAsianWidth, ch);
}
void CharacterWidths(JNIEnv* env, jclass, jcharArray chars, jint start, jint count, jbyteArray out) {
    CharacterProperties(env, chars, start, count, out, kEastAsianWidth);
}
jchar MirrorCharacter(JNIEnv*, jclass, jchar ch) {
    const auto match = std::ranges::lower_bound(kMirror, ch, {}, &UnicodeMirror::from);
    return match != std::end(kMirror) && match->from == ch ? match->to : ch;
}
jboolean MirrorCharacters(JNIEnv* env, jclass cls, jcharArray chars, jint start, jint count) {
    return CallSafely(env, [&]() -> jboolean {
        if (!CharacterSlice(env, chars, start, count)) return false;
        std::vector<jchar> text(count);
        env->GetCharArrayRegion(chars, start, count, text.data());
        if (env->ExceptionCheck()) return false;
        bool changed = false;
        for (auto& ch : text) {
            const auto mirror = MirrorCharacter(env, cls, ch);
            changed |= ch != mirror;
            ch = mirror;
        }
        if (changed) env->SetCharArrayRegion(chars, start, count, text.data());
        return changed;
    });
}

template <class Array, class Element, auto SetRegion>
void ZeroArray(JNIEnv* env, jclass, Array array) {
    if (!array) {
        Throw(env, "java/lang/NullPointerException", "Array to zeroize");
        return;
    }
    constexpr std::array<Element, 256> zeros{};
    const jsize count = env->GetArrayLength(array);
    for (jsize offset = 0; offset < count;) {
        const jsize length = std::min<jsize>(zeros.size(), count - offset);
        (env->*SetRegion)(array, offset, length, zeros.data());
        if (env->ExceptionCheck()) return;
        offset += length;
    }
}

void InitHyphenator(JNIEnv*, jclass) {
    // The host line breaker has no Minikin hyphenation dictionaries to preload.
}

void RegisterTextUtilities(interp::Vm& vm) {
    if (vm.HasClass("Landroid/text/AndroidCharacter;"))
        Register(vm, "android/text/AndroidCharacter",
                 Method<CharacterDirections>(vm, "getDirectionalities", "([C[BI)V"),
                 Method<CharacterWidth>(vm, "getEastAsianWidth", "(C)I"),
                 Method<CharacterWidths>(vm, "getEastAsianWidths", "([CII[B)V"),
                 Method<MirrorCharacter>(vm, "getMirror", "(C)C"),
                 Method<MirrorCharacters>(vm, "mirror", "([CII)Z"));
    if (vm.HasClass("Landroid/text/Hyphenator;"))
        Register(vm, "android/text/Hyphenator", Method<InitHyphenator>(vm, "nInit", "()V"));
    if (vm.HasClass("Lcom/android/internal/util/ArrayUtils;"))
        Register(vm, "com/android/internal/util/ArrayUtils",
                 Method<ZeroArray<jbyteArray, jbyte, &JNIEnv::SetByteArrayRegion>>(vm, "zeroize", "([B)V"),
                 Method<ZeroArray<jcharArray, jchar, &JNIEnv::SetCharArrayRegion>>(vm, "zeroize", "([C)V"));
}

} // namespace
