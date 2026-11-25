#include "native_graphics.h"

#include <cstdint>
#include <iterator>

namespace wa::ui_jni::detail {
namespace text_layout {

struct StyleRun {
    jint start;
    jint end;
    PaintData paint;
    std::optional<float> replacement;
    bool keep_together;
};

struct MeasuredParagraph {
    std::u16string text;
    std::vector<StyleRun> runs;
    std::vector<float> advances;
};

struct BreakConfig {
    std::vector<jint> indents;
};

struct Line {
    jint end;
    float width;
    float ascent;
    float descent;
    jint flags;
};

struct Peers {
    std::shared_ptr<GraphicsPeers<std::vector<StyleRun>>> builders =
        std::make_shared<GraphicsPeers<std::vector<StyleRun>>>();
    std::shared_ptr<GraphicsPeers<MeasuredParagraph>> measured =
        std::make_shared<GraphicsPeers<MeasuredParagraph>>();
    std::shared_ptr<GraphicsPeers<BreakConfig>> configs =
        std::make_shared<GraphicsPeers<BreakConfig>>();
    std::shared_ptr<GraphicsPeers<std::vector<Line>>> results =
        std::make_shared<GraphicsPeers<std::vector<Line>>>();
};

thread_local std::unordered_map<JNIEnv*, Peers> text_peers;

template <auto Store>
auto& StoreFor(JNIEnv* env) {
    return *(text_peers.at(env).*Store);
}

template <auto Store>
jlong Retained(JNIEnv* env, jobject owner, auto&& value) {
    auto& store = StoreFor<Store>(env);
    const jlong handle = store.Insert(std::forward<decltype(value)>(value));
    try {
        store.Retain(env, owner, handle);
    } catch (...) {
        store.Erase(handle);
        throw;
    }
    return handle;
}

char32_t CodePoint(std::u16string_view text, jint index) {
    const char32_t high = text[index];
    if (high >= 0xd800 && high <= 0xdbff && static_cast<size_t>(index) + 1 < text.size()) {
        const char32_t low = text[index + 1];
        if (low >= 0xdc00 && low <= 0xdfff)
            return 0x10000 + ((high - 0xd800) << 10) + low - 0xdc00;
    }
    return high;
}

jint NextCodePoint(std::u16string_view text, jint index) {
    return index + (CodePoint(text, index) >= 0x10000 ? 2 : 1);
}

bool IsExtending(char32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036f) || (cp >= 0x1ab0 && cp <= 0x1aff)
        || (cp >= 0x1dc0 && cp <= 0x1dff) || (cp >= 0x20d0 && cp <= 0x20ff)
        || (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xfe20 && cp <= 0xfe2f)
        || (cp >= 0x1f3fb && cp <= 0x1f3ff) || (cp >= 0xe0020 && cp <= 0xe007f)
        || (cp >= 0xe0100 && cp <= 0xe01ef);
}

bool IsRegionalIndicator(char32_t cp) {
    return cp >= 0x1f1e6 && cp <= 0x1f1ff;
}

// TextDirectionHeuristics needs the strong types even when the guest ICU
// module is absent. The VM supplies the character categories used below.
jbyte StrongDirection(JNIEnv* env, jclass, jint cp) {
    if (cp == 0x200e || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return 0;
    if (cp == 0x200f) return 1;
    if (cp == 0x061c) return 2;
    if (cp < 0x80) return 13;
    const auto character = env->FindClass("java/lang/Character");
    if (!character) return 13;
    const auto category = env->GetStaticMethodID(character, "getType", "(I)I");
    if (!category) return 13;
    const jvalue argument{.i = cp};
    const jint type = env->CallStaticIntMethodA(character, category, &argument);
    env->DeleteLocalRef(character);
    if (type == 0 || type == 19) return -1; // UNASSIGNED or SURROGATE
    if (type < 1 || type > 5) return 13;
    if ((cp >= 0x0600 && cp <= 0x08ff) || (cp >= 0xfb50 && cp <= 0xfdff)
            || (cp >= 0xfe70 && cp <= 0xfefc) || (cp >= 0x1ee00 && cp <= 0x1eeff)) {
        return 2;
    }
    if ((cp >= 0x0590 && cp <= 0x05ff) || (cp >= 0xfb1d && cp <= 0xfb4f)
            || (cp >= 0x10800 && cp <= 0x10fff) || (cp >= 0x1e800 && cp <= 0x1efff)) {
        return 1;
    }
    return 0;
}

jint BidiLevels(JNIEnv* env, jclass cls, jint direction, jcharArray chars, jbyteArray output) {
    return CallSafely(env, [&]() -> jint {
        const jint length = env->GetArrayLength(chars);
        const auto text = Chars(env, chars, 0, length);
        if (env->ExceptionCheck()) return 0;
        std::vector<jbyte> types(length);
        jbyte first_strong = -1;
        for (jint i = 0; i < length;) {
            const auto point = CodePoint(text, i);
            const jint next = NextCodePoint(text, i);
            jbyte type = StrongDirection(env, cls, point);
            if (env->ExceptionCheck()) return 0;
            if (type >= 0 && type <= 2 && first_strong == -1) first_strong = type;
            if ((point >= U'0' && point <= U'9') || (point >= 0x06f0 && point <= 0x06f9))
                type = 3;
            if (point >= 0x0660 && point <= 0x0669) type = 6;
            if (IsExtending(point) && i) type = types[i - 1];
            std::fill(types.begin() + i, types.begin() + next, type);
            i = next;
        }
        const bool default_direction = direction == 2 || direction == -2;
        const jbyte base = default_direction && first_strong != -1
            ? static_cast<jbyte>(first_strong != 0)
            : static_cast<jbyte>(direction == -1 || direction == -2);
        jbyte previous = base;
        for (auto& type : types) {
            if (type >= 0 && type <= 2) {
                previous = type;
                type = type == 0 ? 0 : 1;
            } else if (type == 3 || type == 6) {
                type = previous == 0 && type == 3 ? 0 : 2;
            } else {
                type = -1;
            }
        }
        for (jint i = 0; i < length;) {
            if (types[i] != -1) {
                ++i;
                continue;
            }
            jint next = i + 1;
            while (next < length && types[next] == -1) ++next;
            const jbyte left = i == 0 ? base : static_cast<jbyte>(types[i - 1] != 0);
            const jbyte right = next == length ? base : static_cast<jbyte>(types[next] != 0);
            const jbyte resolved = left == right ? left : base;
            std::fill(types.begin() + i, types.begin() + next, resolved);
            i = next;
        }
        if (base) {
            for (auto& type : types) {
                if (type == 0) type = 2;
            }
        }
        env->SetByteArrayRegion(output, 0, length, types.data());
        return base ? -1 : 1;
    });
}

jint NextCluster(std::u16string_view text, jint index, jint end) {
    char32_t previous = CodePoint(text, index);
    jint next = NextCodePoint(text, index);
    if (next < end && ((previous == U'\r' && text[next] == u'\n')
            || (IsRegionalIndicator(previous) && IsRegionalIndicator(CodePoint(text, next))))) {
        next = NextCodePoint(text, next);
    }
    while (next < end) {
        const char32_t cp = CodePoint(text, next);
        if (cp != 0x200d && previous != 0x200d && !IsExtending(cp)) break;
        previous = cp;
        next = NextCodePoint(text, next);
    }
    return std::min(next, end);
}

auto FirstRun(const MeasuredParagraph& paragraph, jint start) {
    return std::upper_bound(paragraph.runs.begin(), paragraph.runs.end(), start,
        [](jint offset, const StyleRun& run) { return offset < run.end; });
}

float RunWidth(State& state, const MeasuredParagraph& paragraph, const StyleRun& run,
               jint start, jint end) {
    if (run.replacement) return start == run.start ? *run.replacement : 0.f;
    return Measure(state, run.paint, std::u16string_view(paragraph.text).substr(start, end - start)).advance;
}

float RangeWidth(State& state, const MeasuredParagraph& paragraph, jint start, jint end) {
    float width = 0;
    for (auto run = FirstRun(paragraph, start); run != paragraph.runs.end() && run->start < end; ++run) {
        width += RunWidth(state, paragraph, *run, std::max(start, run->start), std::min(end, run->end));
    }
    return width;
}

wa_text_metrics_t Extent(const State& state, const MeasuredParagraph& paragraph, jint start, jint end) {
    wa_text_metrics_t extent{};
    for (auto run = FirstRun(paragraph, start); run != paragraph.runs.end() && run->start < end; ++run) {
        const auto& paint = run->paint;
        const auto metrics = wa_canvas_measure_text(state.sf, "", 0, paint.text_size,
                                                    paint.Weight(), paint.Face());
        extent.ascent = std::max(extent.ascent, metrics.ascent);
        extent.descent = std::max(extent.descent, metrics.descent);
    }
    return extent;
}

jlong InitBuilder(JNIEnv* env, jclass, jobject owner) {
    return CallSafely(env, [&] {
        return Retained<&Peers::builders>(env, owner, std::vector<StyleRun>{});
    });
}

void AppendRun(JNIEnv* env, jlong builder, jlong paint, jint start, jint end,
               std::optional<float> replacement, bool keep_together) {
    CallSafely(env, [&] {
        auto* runs = StoreFor<&Peers::builders>(env).Get(env, builder);
        const auto* style = Paint(env, paint);
        if (runs && style) runs->push_back({start, end, *style, replacement, keep_together});
    });
}

void AddStyleRun(JNIEnv* env, jclass, jlong builder, jlong paint, jint style, jint,
                 jboolean, jint start, jint end, jboolean) {
    // android.graphics.text.LineBreakConfig.LINE_BREAK_STYLE_NO_BREAK
    AppendRun(env, builder, paint, start, end, std::nullopt, style == 4);
}

void AddReplacementRun(JNIEnv* env, jclass, jlong builder, jlong paint, jint start, jint end, jfloat width) {
    AppendRun(env, builder, paint, start, end, width, true);
}

jlong BuildMeasuredText(JNIEnv* env, jclass, jobject owner, jlong builder, jlong, jcharArray chars,
                        jboolean, jboolean, jboolean, jboolean) {
    return CallSafely(env, [&]() -> jlong {
        auto* state = GetState(env);
        auto* runs = StoreFor<&Peers::builders>(env).Get(env, builder);
        if (!state || !runs) return 0;
        const jsize length = env->GetArrayLength(chars);
        MeasuredParagraph paragraph{Chars(env, chars, 0, length), std::move(*runs),
                                    std::vector<float>(length)};
        if (env->ExceptionCheck()) return 0;
        for (const auto& run : paragraph.runs) {
            if (run.start >= run.end) continue;
            if (run.replacement) {
                paragraph.advances[run.start] = *run.replacement;
                continue;
            }
            // The rasterizer rounds a complete run. Measure at a larger scale
            // so allocating that width to UTF-16 clusters does not round every glyph.
            PaintData precise = run.paint;
            constexpr float scale = 1024;
            precise.text_size *= scale;
            precise.word_spacing *= scale;
            float total = 0;
            for (jint i = run.start; i < run.end;) {
                const jint next = NextCluster(paragraph.text, i, run.end);
                const auto text = std::u16string_view(paragraph.text).substr(i, next - i);
                const float advance = Measure(*state, precise, text).advance / scale;
                paragraph.advances[i] = advance;
                total += advance;
                i = next;
            }
            if (total > 0) {
                const float ratio = RunWidth(*state, paragraph, run, run.start, run.end) / total;
                for (jint i = run.start; i < run.end; ++i) paragraph.advances[i] *= ratio;
            }
        }
        return Retained<&Peers::measured>(env, owner, std::move(paragraph));
    });
}

void FreeBuilder(JNIEnv* env, jclass, jlong builder) {
    StoreFor<&Peers::builders>(env).Erase(builder);
}

jfloat Width(JNIEnv* env, jclass, jlong handle, jint start, jint end) {
    return CallSafely(env, [&] {
        auto* state = GetState(env);
        const auto* paragraph = StoreFor<&Peers::measured>(env).Get(env, handle);
        if (!state || !paragraph || start == end) return 0.f;
        return RangeWidth(*state, *paragraph, start, end);
    });
}

jfloat CharWidth(JNIEnv* env, jclass, jlong handle, jint offset) {
    const auto* paragraph = StoreFor<&Peers::measured>(env).Get(env, handle);
    return paragraph ? paragraph->advances[offset] : 0.f;
}

jint MemoryUsage(JNIEnv* env, jclass, jlong handle) {
    const auto* paragraph = StoreFor<&Peers::measured>(env).Get(env, handle);
    if (!paragraph) return 0;
    const size_t bytes = sizeof(*paragraph) + paragraph->text.capacity() * sizeof(char16_t)
        + paragraph->runs.capacity() * sizeof(StyleRun)
        + paragraph->advances.capacity() * sizeof(float);
    return static_cast<jint>(std::min(bytes, static_cast<size_t>(INT32_MAX)));
}

void Bounds(JNIEnv* env, jclass, jlong handle, jcharArray, jint start, jint end, jobject rect) {
    CallSafely(env, [&] {
        auto* state = GetState(env);
        const auto* paragraph = StoreFor<&Peers::measured>(env).Get(env, handle);
        if (!state || !paragraph) return;
        const auto extent = Extent(*state, *paragraph, start, end);
        SetRect(env, rect, {0, -extent.ascent, RangeWidth(*state, *paragraph, start, end), extent.descent});
    });
}

jlong PackedExtent(JNIEnv* env, jclass, jlong handle, jcharArray, jint start, jint end) {
    auto* state = GetState(env);
    const auto* paragraph = StoreFor<&Peers::measured>(env).Get(env, handle);
    if (!state || !paragraph) return 0;
    const auto extent = Extent(*state, *paragraph, start, end);
    const auto pixels = [](float size) {
        return static_cast<uint32_t>(std::min(std::ceil(double(size)), double(INT32_MAX)));
    };
    return static_cast<jlong>(uint64_t{0u - pixels(extent.ascent)} << 32 | pixels(extent.descent));
}

jlong InitBreaker(JNIEnv* env, jclass, jobject owner, jint, jint, jboolean, jintArray array, jboolean) {
    return CallSafely(env, [&]() -> jlong {
        BreakConfig config;
        if (array) {
            config.indents.resize(env->GetArrayLength(array));
            env->GetIntArrayRegion(array, 0, config.indents.size(), config.indents.data());
            if (env->ExceptionCheck()) return 0;
        }
        return Retained<&Peers::configs>(env, owner, std::move(config));
    });
}

bool IsLineSpace(char16_t ch) {
    return ch == u' ' || ch == u'\t' || ch == u'\r' || ch == u'\n'
        || (ch >= 0x2000 && ch <= 0x200a && ch != 0x2007) || ch == 0x3000;
}

bool IsIdeograph(char32_t cp) {
    return (cp >= 0x3040 && cp <= 0x30ff) || (cp >= 0x3400 && cp <= 0x9fff)
        || (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0x20000 && cp <= 0x323af);
}

float TabStop(float x, const std::vector<float>& stops, float interval) {
    for (const float stop : stops) {
        if (stop > x) return stop;
    }
    return interval > 0 ? (std::floor(x / interval) + 1) * interval : x;
}

float LineWidth(State& state, const MeasuredParagraph& paragraph, jint start, jint end,
                const std::vector<float>& tabs, float interval) {
    const auto text = std::u16string_view(paragraph.text).substr(start, end - start);
    size_t previous = 0;
    float width = 0;
    for (size_t tab = text.find(u'\t'); tab != text.npos; tab = text.find(u'\t', previous)) {
        width += RangeWidth(state, paragraph, start + previous, start + tab);
        width = TabStop(width, tabs, interval);
        previous = tab + 1;
    }
    return width + RangeWidth(state, paragraph, start + previous, end);
}

Line BreakLine(State& state, const MeasuredParagraph& paragraph, jint start, float limit,
               const std::vector<float>& tabs, float interval) {
    const auto text = std::u16string_view(paragraph.text);
    const jint length = static_cast<jint>(text.size());
    jint fit = start, word_break = start, visible_end = start;
    float fit_width = 0, word_width = 0;
    auto run = FirstRun(paragraph, start);
    for (jint i = start; i < length;) {
        while (i >= run->end) ++run;
        const jint next = run->keep_together ? run->end : NextCluster(text, i, length);
        if (text[i] == u'\n' || text[i] == u'\r') {
            fit = next;
            break;
        }
        if (!IsLineSpace(text[i])) visible_end = next;
        const float width = visible_end == start ? 0
            : LineWidth(state, paragraph, start, visible_end, tabs, interval);
        if (width > limit && fit > start) {
            if (word_break > start) {
                fit = word_break;
                fit_width = word_width;
            }
            break;
        }
        fit = next;
        fit_width = width;
        const bool slash_break = text[i] == u'/' && next < length && text[next] != u'/' &&
            (i == start || text[i - 1] != u'/') && (text[next] < u'0' || text[next] > u'9');
        if (IsLineSpace(text[i]) || text[i] == u'-' || text[i] == 0x200b || slash_break
                || IsIdeograph(CodePoint(text, i))) {
            word_break = next;
            word_width = width;
        }
        if (width > limit) break;
        i = next;
    }
    const auto extent = Extent(state, paragraph, start, fit);
    const bool has_tab = text.substr(start, fit - start).find(u'\t') != text.npos;
    return {fit, fit_width, -extent.ascent, extent.descent, has_tab ? 0x20000000 : 0};
}

jlong ComputeLineBreaks(JNIEnv* env, jclass, jobject owner, jlong handle, jcharArray,
                       jlong measured, jint, jfloat first_width, jint first_count, jfloat width,
                       jfloatArray array, jfloat interval, jint offset) {
    return CallSafely(env, [&]() -> jlong {
        auto* state = GetState(env);
        const auto* config = StoreFor<&Peers::configs>(env).Get(env, handle);
        const auto* paragraph = StoreFor<&Peers::measured>(env).Get(env, measured);
        if (!state || !config || !paragraph) return 0;
        std::vector<float> tabs;
        if (array) {
            tabs.resize(env->GetArrayLength(array));
            env->GetFloatArrayRegion(array, 0, tabs.size(), tabs.data());
            if (env->ExceptionCheck()) return 0;
        }
        std::vector<Line> lines;
        for (jint start = 0; static_cast<size_t>(start) < paragraph->text.size();) {
            float limit = lines.size() < static_cast<size_t>(std::max(0, first_count)) ? first_width : width;
            if (!config->indents.empty()) {
                const auto indent = std::min(static_cast<size_t>(std::max(0, offset)) + lines.size(),
                                             config->indents.size() - 1);
                limit -= config->indents[indent];
            }
            const auto line = BreakLine(*state, *paragraph, start, std::max(0.f, limit), tabs, interval);
            lines.push_back(line);
            start = line.end;
        }
        return Retained<&Peers::results>(env, owner, std::move(lines));
    });
}

jint LineCount(JNIEnv* env, jclass, jlong handle) {
    const auto* lines = StoreFor<&Peers::results>(env).Get(env, handle);
    return lines ? static_cast<jint>(lines->size()) : 0;
}

template <auto Member>
auto LineValue(JNIEnv* env, jclass, jlong handle, jint index) -> decltype(Line{}.*Member + 0) {
    const auto* lines = StoreFor<&Peers::results>(env).Get(env, handle);
    if (lines && index >= 0 && static_cast<size_t>(index) < lines->size())
        return (*lines)[index].*Member;
    if (lines) Throw(env, "java/lang/IndexOutOfBoundsException", "Line index outside paragraph");
    return {};
}

void GraphemeBoundaries(JNIEnv* env, jclass, jfloatArray, jcharArray chars, jint start, jint end,
                        jbooleanArray breaks) {
    CallSafely(env, [&] {
        const auto text = Chars(env, chars, start, end - start);
        if (env->ExceptionCheck() || text.empty()) return;
        std::vector<jboolean> boundaries(text.size());
        for (jint i = 0; i < end - start; i = NextCluster(text, i, end - start))
            boundaries[i] = JNI_TRUE;
        env->SetBooleanArrayRegion(breaks, 0, boundaries.size(), boundaries.data());
    });
}

jint TargetSdkVersion(JNIEnv* env, jclass) {
    auto* state = GetState(env);
    if (!state) return 0;
    const auto runtime = env->FindClass("android/app/UiRuntime");
    if (!runtime) return 0;
    const auto activity_method = env->GetStaticMethodID(runtime, "getActivity", "()Landroid/app/Activity;");
    if (!activity_method) return 0;
    const auto activity = env->CallStaticObjectMethodA(runtime, activity_method, nullptr);
    if (env->ExceptionCheck()) return 0;
    if (!activity) return 37;
    const auto app_info_method = env->GetMethodID(env->GetObjectClass(activity),
        "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    if (!app_info_method) return 0;
    const auto app_info = env->CallObjectMethodA(activity, app_info_method, nullptr);
    if (!app_info) return 0;
    const auto field = env->GetFieldID(env->GetObjectClass(app_info), "targetSdkVersion", "I");
    return field ? env->GetIntField(app_info, field) : 0;
}

jobject WrapFont(JNIEnv* env, jclass, jlong handle) {
    if (!interp::JniBridge::From(env).Vm().linker.HasClass("Landroid/graphics/fonts/Font;"))
        return nullptr;
    const auto cls = env->FindClass("android/graphics/fonts/Font");
    if (!cls) return nullptr;
    const auto ctor = env->GetMethodID(cls, "<init>", "(J)V");
    const jvalue argument{.j = handle};
    return ctor ? env->NewObjectA(cls, ctor, &argument) : nullptr;
}

#include "glyph_run.inc"

} // namespace text_layout

void RegisterTextLayout(interp::Vm& vm) {
    using namespace text_layout;
    if (!vm.HasClass("Landroid/graphics/text/MeasuredText;")) return;
    Register(vm, "android/graphics/text/MeasuredText",
             Method<BuildMeasuredText>(vm, "nBuildMeasuredText",
                 "(Landroid/graphics/text/MeasuredText;JJ[CZZZZ)J"),
             Method<Width>(vm, "nGetWidth", "(JII)F"),
             Method<CharWidth>(vm, "nGetCharWidthAt", "(JI)F"),
             Method<MemoryUsage>(vm, "nGetMemoryUsage", "(J)I"),
             Method<Bounds>(vm, "nGetBounds", "(J[CIILandroid/graphics/Rect;)V"),
             Method<PackedExtent>(vm, "nGetExtent", "(J[CII)J"));
    Register(vm, "android/graphics/text/MeasuredText$Builder",
             Method<InitBuilder>(vm, "nInitBuilder", "(Landroid/graphics/text/MeasuredText$Builder;)J"),
             Method<FreeBuilder>(vm, "nFreeBuilder", "(J)V"),
             Method<AddStyleRun>(vm, "nAddStyleRun", "(JJIIZIIZ)V"),
             Method<AddReplacementRun>(vm, "nAddReplacementRun", "(JJIIF)V"));
    Register(vm, "android/graphics/text/LineBreaker",
             Method<InitBreaker>(vm, "nInit", "(Landroid/graphics/text/LineBreaker;IIZ[IZ)J"),
             Method<ComputeLineBreaks>(vm, "nComputeLineBreaks",
                 "(Landroid/graphics/text/LineBreaker$Result;J[CJIFIF[FFI)J"),
             Method<LineCount>(vm, "nGetLineCount", "(J)I"),
             Method<LineValue<&Line::end>>(vm, "nGetLineBreakOffset", "(JI)I"),
             Method<LineValue<&Line::width>>(vm, "nGetLineWidth", "(JI)F"),
             Method<LineValue<&Line::ascent>>(vm, "nGetLineAscent", "(JI)F"),
             Method<LineValue<&Line::descent>>(vm, "nGetLineDescent", "(JI)F"),
             Method<LineValue<&Line::flags>>(vm, "nGetLineFlag", "(JI)I"));
    Register(vm, "android/graphics/text/GraphemeBreak",
             Method<GraphemeBoundaries>(vm, "nIsGraphemeBreak", "([F[CII[Z)V"));
    Register(vm, "android/graphics/text/LineBreakConfig",
             Method<TargetSdkVersion>(vm, "nGetTargetSdkVersion", "()I"));
    Register(vm, "android/graphics/text/PositionedGlyphs",
             Method<WrapFont>(vm, "nWrapFont", "(J)Landroid/graphics/fonts/Font;"));
    RegisterGlyphRuns(vm);
    Register(vm, "android/text/TextDirectionHeuristics",
             Method<StrongDirection>(vm, "nGetStrongDirection", "(I)B"));
    Register(vm, "android/text/AndroidBidi", Method<BidiLevels>(vm, "nBidi", "(I[C[B)I"));
    text_peers.try_emplace(vm.JniEnv());
}

void UnregisterTextLayout(JNIEnv* env) {
    text_layout::text_peers.erase(env);
}

} // namespace wa::ui_jni::detail
