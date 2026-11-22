std::u16string Chars(JNIEnv* env, jstring string, jint start, jint end) {
    if (!string || start < 0 || end < start || end > env->GetStringLength(string)) {
        Throw(env, "java/lang/IndexOutOfBoundsException", "String run outside text");
        return {};
    }
    std::vector<jchar> chars(end - start);
    if (!chars.empty())
        env->GetStringRegion(string, start, chars.size(), chars.data());
    return {chars.begin(), chars.end()};
}

std::u16string Chars(JNIEnv* env, jcharArray array, jint offset, jint count) {
    if (!array || offset < 0 || count < 0 || offset > env->GetArrayLength(array) - count) {
        Throw(env, "java/lang/IndexOutOfBoundsException", "Character run outside text");
        return {};
    }
    std::vector<jchar> chars(count);
    if (count) env->GetCharArrayRegion(array, offset, count, chars.data());
    return {chars.begin(), chars.end()};
}

std::string Utf8(std::u16string_view text) {
    std::string bytes;
    bytes.reserve(text.size());
    const auto append = [&](unsigned byte) { bytes.push_back(static_cast<char>(byte)); };
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned cp = text[i];
        if (cp >= 0xd800 && cp <= 0xdfff) {
            if (cp <= 0xdbff && i + 1 < text.size() && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff)
                cp = 0x10000 + ((cp - 0xd800) << 10) + text[++i] - 0xdc00;
            else
                cp = 0xfffd;
        }
        if (cp <= 0x7f) {
            append(cp);
        } else if (cp <= 0x7ff) {
            append(0xc0 | (cp >> 6));
            append(0x80 | (cp & 63));
        } else if (cp <= 0xffff) {
            append(0xe0 | (cp >> 12));
            append(0x80 | ((cp >> 6) & 63));
            append(0x80 | (cp & 63));
        } else {
            append(0xf0 | (cp >> 18));
            append(0x80 | ((cp >> 12) & 63));
            append(0x80 | ((cp >> 6) & 63));
            append(0x80 | (cp & 63));
        }
    }
    return bytes;
}

uint32_t UnpackColor(jlong value) {
    const auto color = static_cast<uint64_t>(value);
    if (!(color & 63)) return color >> 32;
    const auto component = [](uint16_t half) {
        const uint32_t exponent = (half >> 10) & 31;
        const float mantissa = (half & 1023) / 1024.f;
        if (half & 0x8000) return 0u;
        if (exponent == 31) return mantissa == 0 ? 255u : 0u;
        const float f = exponent ? std::ldexp(1.f + mantissa, int(exponent) - 15)
                                 : std::ldexp(mantissa, -14);
        return static_cast<uint32_t>(std::lround(std::min(f, 1.f) * 255));
    };
    return ((color >> 6 & 1023) * 255 + 511) / 1023 << 24 |
           component(color >> 48) << 16 | component(color >> 32) << 8 | component(color >> 16);
}

wa_text_metrics_t Measure(State& state, const PaintData& paint, std::u16string_view text) {
    const auto bytes = Utf8(text);
    auto result = wa_canvas_measure_text(state.sf, bytes.data(), bytes.size(),
                                         paint.text_size, paint.Weight(), paint.Face());
    if (result.advance == 0) return result;
    const auto length = std::ranges::count_if(bytes, [](unsigned char byte) { return (byte & 0xc0) != 0x80; });
    const float spacing = std::max<ptrdiff_t>(0, length - 1) * paint.letter_spacing * paint.text_size +
                          std::ranges::count(text, u' ') * paint.word_spacing;
    result.advance = std::max(0.f, (result.advance + spacing) * paint.text_scale);
    return result;
}

namespace {

jlong CopyPaint(JNIEnv* env, jclass, jlong id) {
    return CallSafely(env, [&]() -> jlong {
        auto* state = GetState(env);
        if (!state) return 0;
        auto& paints = Peers<PaintData, &State::paints>(*state);
        if (!id) {
            PaintData paint;
            paint.typeface = state->default_typeface;
            return paints.Insert(std::move(paint));
        }
        const auto* existing = paints.Get(env, id);
        return existing ? paints.Insert(*existing) : 0;
    });
}
jlong InitPaint(JNIEnv* env, jclass type) { return CopyPaint(env, type, 0); }
void ResetPaint(JNIEnv* env, jclass, jlong id) {
    if (auto* p = Paint(env, id)) {
        *p = PaintData{};
        p->typeface = GetState(env)->default_typeface;
    }
}
void SetPaint(JNIEnv* env, jclass, jlong dst, jlong src) {
    CallSafely(env, [&] {
        auto* paint = Paint(env, dst);
        const auto* source = Paint(env, src);
        if (paint && source) *paint = *source;
    });
}

template <auto Member, class T>
T PaintGet(JNIEnv* env, jclass, jlong id) {
    const auto* p = Paint(env, id);
    return p ? static_cast<T>(p->*Member) : T{};
}
template <auto Member, class T>
void PaintSet(JNIEnv* env, jclass, jlong id, T value) {
    if (auto* p = Paint(env, id)) p->*Member = value;
}
template <auto Member>
jlong PaintExchange(JNIEnv* env, jclass, jlong id, jlong value) {
    auto* p = Paint(env, id);
    return p ? std::exchange(p->*Member, value) : 0;
}
template <jint Mask>
void PaintFlag(JNIEnv* env, jclass, jlong id, jboolean value) {
    if (auto* p = Paint(env, id)) p->flags = value ? p->flags | Mask : p->flags & ~Mask;
}
void PaintAlpha(JNIEnv* env, jclass, jlong id, jint alpha) {
    if (auto* p = Paint(env, id)) p->color = (p->color & 0x00ffffffu) | uint32_t(std::clamp(alpha, 0, 255)) << 24;
}
void PaintColorLong(JNIEnv* env, jclass, jlong id, jlong, jlong color) {
    if (auto* p = Paint(env, id)) p->color = UnpackColor(color);
}
void PaintShadow(JNIEnv* env, jclass, jlong id, jfloat radius, jfloat dx, jfloat dy, jlong, jlong color) {
    if (auto* p = Paint(env, id)) {
        p->shadow_radius = std::max(0.f, radius);
        p->shadow_dx = dx;
        p->shadow_dy = dy;
        p->shadow_color = UnpackColor(color);
    }
}
void PaintTypeface(JNIEnv* env, jclass, jlong id, jlong typeface) {
    if (auto* p = Paint(env, id)) p->typeface = typeface ? typeface : GetState(env)->default_typeface;
}
jboolean PaintHasShadow(JNIEnv* env, jclass, jlong id) {
    const auto* paint = Paint(env, id);
    return paint && paint->shadow_radius > 0;
}
jint PaintLocale(JNIEnv* env, jclass, jlong id, jstring locales) {
    return CallSafely(env, [&] {
        auto* p = Paint(env, id);
        if (!p) return jint{0};
        uint32_t hash = 0;
        for (unsigned char c : Utf8(env, locales)) hash = hash * 31 + c;
        return p->locale = static_cast<jint>(hash & INT32_MAX);
    });
}
void PaintFeatures(JNIEnv* env, jclass, jlong id, jstring features) {
    CallSafely(env, [&] {
        if (auto* p = Paint(env, id)) p->features = features ? Utf8(env, features) : std::string{};
    });
}
struct PaintLineMetrics {
    float ascent, descent, top, bottom;
};

PaintLineMetrics Metrics(JNIEnv* env, jlong id) {
    const auto* state = GetState(env);
    const auto* p = Paint(env, id);
    if (!state || !p) return {};
    const auto measured = wa_canvas_measure_text(state->sf, "", 0, p->text_size, p->Weight(), p->Face());
    // Keep the padded line box stable when a browser supplies a taller fallback font.
    return {measured.ascent, measured.descent, -std::max(measured.ascent, p->text_size),
            std::max(measured.descent, p->text_size * 0.25f)};
}
jfloat PaintMetrics(JNIEnv* env, jclass, jlong id, jobject out, jboolean) {
    const auto m = Metrics(env, id);
    if (out) {
        const auto cls = env->GetObjectClass(out);
        const auto put = [&](const char* name, float value) {
            if (const auto field = env->GetFieldID(cls, name, "F")) env->SetFloatField(out, field, value);
        };
        put("top", m.top);
        put("ascent", -m.ascent);
        put("descent", m.descent);
        put("bottom", m.bottom);
        put("leading", 0.f);
    }
    return m.ascent + m.descent;
}

jint PixelOutward(float value, bool lower = false) {
    if (std::isnan(value)) return 0;
    const double rounded = lower ? std::floor(double(value)) : std::ceil(double(value));
    return static_cast<jint>(std::clamp(rounded, double(INT32_MIN), double(INT32_MAX)));
}
void RectFields(JNIEnv* env, jobject rect, wa_rect_t values, bool floating) {
    if (!rect) return;
    const auto cls = env->GetObjectClass(rect);
    constexpr const char* fields[] = {"left", "top", "right", "bottom"};
    const auto [l, t, r, b] = values;
    const std::array coords{l, t, r, b};
    for (size_t i = 0; i < coords.size(); ++i) {
        const auto field = env->GetFieldID(cls, fields[i], floating ? "F" : "I");
        if (!field) return;
        if (floating) env->SetFloatField(rect, field, coords[i]);
        else env->SetIntField(rect, field, PixelOutward(coords[i], i < 2));
    }
}
jint PaintMetricsInt(JNIEnv* env, jclass, jlong id, jobject out, jboolean) {
    const auto m = Metrics(env, id);
    const jint ascent = PixelOutward(m.ascent), descent = PixelOutward(m.descent);
    if (out) {
        const auto cls = env->GetObjectClass(out);
        const auto put = [&](const char* name, jint value) {
            if (const auto field = env->GetFieldID(cls, name, "I")) env->SetIntField(out, field, value);
        };
        put("top", PixelOutward(m.top, true));
        put("ascent", -ascent);
        put("descent", descent);
        put("bottom", PixelOutward(m.bottom));
        put("leading", 0);
    }
    return static_cast<jint>(std::min<int64_t>(INT32_MAX, int64_t(ascent) + descent));
}
template <typename Text>
void PaintTextMetrics(JNIEnv* env, jclass cls, jlong id, Text, jint, jint, jint, jint, jboolean, jobject out) {
    PaintMetricsInt(env, cls, id, out, true);
}
jfloat PaintAscent(JNIEnv* env, jclass, jlong id) { return -Metrics(env, id).ascent; }
jfloat PaintDescent(JNIEnv* env, jclass, jlong id) { return Metrics(env, id).descent; }
template <int Percent, bool MinimumPixel = false> jfloat DecorationSize(JNIEnv* env, jclass, jlong id) {
    const auto* p = Paint(env, id);
    return p ? std::max(MinimumPixel ? 1.f : -INFINITY, p->text_size * (Percent / 100.f)) : 0;
}

bool IsSurrogatePair(std::u16string_view text, size_t i) {
    return i + 1 < text.size() && text[i] >= 0xd800 && text[i] <= 0xdbff &&
           text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff;
}

template <typename Text>
std::u16string GetRun(JNIEnv* env, Text text, jint start, jint length_or_end) {
    return Chars(env, text, start, length_or_end);
}

float AdvanceRun(JNIEnv* env, jlong id, std::u16string_view text, jfloatArray advances, jint offset) {
    auto* state = GetState(env);
    const auto* paint = Paint(env, id);
    if (!state || !paint || env->ExceptionCheck()) return 0.f;
    const float width = Measure(*state, *paint, text).advance;
    if (advances && !text.empty()) {
        std::vector<float> values(text.size());
        float sum = 0;
        for (size_t i = 0; i < text.size();) {
            const size_t units = IsSurrogatePair(text, i) ? 2 : 1;
            values[i] = Measure(*state, *paint, text.substr(i, units)).advance;
            sum += values[i];
            i += units;
        }
        if (sum > 0) for (auto& value : values) value *= width / sum;
        env->SetFloatArrayRegion(advances, offset, values.size(), values.data());
    }
    return width;
}

template <typename Text>
jfloat PaintAdvances(JNIEnv* env, jclass, jlong id, Text text, jint start, jint end, jint, jint, jint,
                     jfloatArray advances, jint offset) {
    return CallSafely(env, [&] { return AdvanceRun(env, id, GetRun(env, text, start, end), advances, offset); });
}

jint FitText(JNIEnv* env, jlong id, std::u16string_view text, bool forwards, jfloat max_width,
             jfloatArray measured) {
    auto* state = GetState(env);
    const auto* paint = Paint(env, id);
    if (!state || !paint || env->ExceptionCheck()) return 0;
    const auto width = [&](size_t n) {
        return Measure(*state, *paint, forwards ? text.substr(0, n) : text.substr(text.size() - n)).advance;
    };
    size_t left = 0, right = text.size();
    while (left < right) {
        const size_t mid = left + (right - left + 1) / 2;
        if (width(mid) <= max_width) left = mid;
        else right = mid - 1;
    }
    if (left && left < text.size() &&
        IsSurrogatePair(text, forwards ? left - 1 : text.size() - left - 1)) --left;
    if (measured) {
        const jfloat result = width(left);
        env->SetFloatArrayRegion(measured, 0, 1, &result);
    }
    return static_cast<jint>(left);
}

jint BreakString(JNIEnv* env, jclass, jlong id, jstring text, jboolean forwards, jfloat max_width,
                 jint, jfloatArray measured) {
    return CallSafely(env, [&] {
        return FitText(env, id, Chars(env, text, 0, env->GetStringLength(text)), forwards, max_width, measured);
    });
}
jint BreakArray(JNIEnv* env, jclass, jlong id, jcharArray text, jint start, jint count, jfloat max_width,
                jint, jfloatArray measured) {
    return CallSafely(env, [&] {
        if (count == INT32_MIN) return jint{0};
        return FitText(env, id, Chars(env, text, start, std::abs(count)), count >= 0, max_width, measured);
    });
}
template <typename Text>
void PaintBounds(JNIEnv* env, jclass, jlong id, Text text, jint start, jint end, jint, jobject out) {
    CallSafely(env, [&] {
        auto* state = GetState(env);
        const auto* paint = Paint(env, id);
        if (!state || !paint) return;
        auto run = GetRun(env, text, start, end);
        if (env->ExceptionCheck()) return;
        const auto m = Measure(*state, *paint, run);
        SetRect(env, out, run.empty() ? wa_rect_t{} : wa_rect_t{0, -m.ascent, m.advance, m.descent});
    });
}

template <typename Text>
jint PaintCursor(JNIEnv* env, jobject, jlong, Text text, jint start, jint length_or_end, jint,
                 jint offset, jint operation) {
    return CallSafely(env, [&]() -> jint {
        const auto run = GetRun(env, text, start, length_or_end);
        if (env->ExceptionCheck()) return -1;
        auto position = offset - start;
        const auto boundary = [&](int at) { return at <= 0 || !IsSurrogatePair(run, at - 1); };
        const auto size = static_cast<jint>(run.size());
        if (operation == 0 || (operation == 1 && !boundary(position))) ++position;
        if (operation == 2 || (operation == 3 && !boundary(position))) --position;
        while (position >= 0 && position <= size && !boundary(position)) {
            if (operation < 2) ++position;
            else if (operation < 4) --position;
            else return -1;
        }
        return position >= 0 && position <= size ? start + position : -1;
    });
}

jfloat PaintRunAdvance(JNIEnv* env, jclass, jlong id, jcharArray text, jint start, jint end, jint, jint,
                       jboolean rtl, jint offset) {
    return CallSafely(env, [&] {
        if (rtl) start = offset;
        else end = offset;
        return AdvanceRun(env, id, Chars(env, text, start, end - start), nullptr, 0);
    });
}
jint OffsetForAdvance(JNIEnv* env, jclass, jlong id, jcharArray text, jint start, jint end,
                      jint, jint, jboolean rtl, jfloat advance) {
    return CallSafely(env, [&] {
        const auto run = Chars(env, text, start, end - start);
        const jint count = FitText(env, id, run, !rtl, advance, nullptr);
        return rtl ? end - count : start + count;
    });
}
jfloat GetRunCharacterAdvance(JNIEnv* env, jclass cls, jlong id, jcharArray text,
                              jint start, jint end, jint context_start, jint context_end,
                              jboolean rtl, jint offset, jfloatArray advances, jint at,
                              jobject bounds, jobject info) {
    return CallSafely(env, [&] {
        const auto run = Chars(env, text, start, end - start);
        if (env->ExceptionCheck()) return 0.f;
        const float width = AdvanceRun(env, id, run, advances, at);
        if (bounds) {
            const auto metrics = Metrics(env, id);
            SetRect(env, bounds, {0.f, -metrics.ascent, width, metrics.descent}, true);
        }
        if (info) {
            jint count = 0;
            for (size_t i = 0; i < run.size(); ++count) i += IsSurrogatePair(run, i) ? 2 : 1;
            const auto field = env->GetFieldID(env->GetObjectClass(info), "mClusterCount", "I");
            if (field) env->SetIntField(info, field, count);
        }
        return env->ExceptionCheck() ? 0.f
            : PaintRunAdvance(env, cls, id, text, start, end, context_start, context_end, rtl, offset);
    });
}
jfloat GetRunCharacterAdvanceLegacy(JNIEnv* env, jclass cls, jlong id, jcharArray text,
                                    jint start, jint end, jint context_start, jint context_end,
                                    jboolean rtl, jint offset, jfloatArray advances, jint at,
                                    jobject bounds) {
    return GetRunCharacterAdvance(env, cls, id, text, start, end, context_start, context_end,
                                  rtl, offset, advances, at, bounds, nullptr);
}

jlong NewFontVariation(JNIEnv* env, jclass, jint) {
    return CallSafely(env, [&]() -> jlong {
        auto* state = GetState(env);
        return state ? Peers<FontVariation, &State::variations>(*state).Insert() : 0;
    });
}
void AddFontVariation(JNIEnv* env, jclass, jlong id, jint tag, jfloat value) {
    auto* axes = Peer<FontVariation, &State::variations>(env, id);
    if (!axes || !std::isfinite(value)) return;
    if (tag == 0x77676874) axes->weight = std::clamp(value, 1.f, 1000.f); // wght
    if (tag == 0x6974616c) axes->italic = value >= 0.5f;                 // ital
}
void SetFontVariation(JNIEnv* env, jclass, jlong paint, jlong variation) {
    auto* p = Paint(env, paint);
    auto* state = GetState(env);
    if (!p || !state || !state->variations) return;
    if (auto* axes = state->variations->Get(env, variation)) {
        p->variation = *axes;
        state->variations->Erase(variation);
    }
}

jboolean PaintEqualForMeasurement(JNIEnv* env, jclass, jlong a, jlong b) {
    const auto* x = Paint(env, a);
    const auto* y = Paint(env, b);
    return x && y && x->flags == y->flags && x->typeface == y->typeface &&
           x->variation.weight == y->variation.weight && x->variation.italic == y->variation.italic &&
           x->text_size == y->text_size && x->text_scale == y->text_scale &&
           x->text_skew == y->text_skew && x->letter_spacing == y->letter_spacing &&
           x->word_spacing == y->word_spacing && x->locale == y->locale &&
           x->features == y->features && x->elegant == y->elegant && x->hinting == y->hinting;
}
jboolean PaintFillPath(JNIEnv* env, jclass, jlong id, jlong src, jlong dst) {
    return CallSafely(env, [&]() -> jboolean {
        const auto* paint = Paint(env, id);
        const auto* source = Path(env, src);
        auto* target = Path(env, dst);
        if (!paint || !source || !target) return false;
        *target = *source;
        return paint->style != 1 || paint->stroke_width > 0;
    });
}
jboolean PaintHasGlyph(JNIEnv* env, jclass, jlong, jint, jstring text) {
    return text && env->GetStringLength(text) > 0;
}
jlong ManagedFinalizer(JNIEnv*, jclass) { return 0; }
jboolean FontCompatibility(JNIEnv*, jclass, jlong) { return false; }

jlong EncodeTypeface(jint face, jint weight, bool italic) {
    return 1LL << 16 | jlong(face) << 12 | jlong(std::clamp(weight, 1, 1000)) << 1 | italic;
}
jint TypefaceWeight(JNIEnv*, jclass, jlong face) {
    return face ? (face >> 1) & 1023 : 400;
}
jint TypefaceStyle(JNIEnv* env, jclass type, jlong face) {
    return (TypefaceWeight(env, type, face) >= 600 ? 1 : 0) | (face & 1 ? 2 : 0);
}
jlong TypefaceExact(JNIEnv*, jclass, jlong base, jint weight, jboolean italic) {
    return EncodeTypeface((base >> 12) & 3, weight, italic);
}
jlong TypefaceAlias(JNIEnv* env, jclass cls, jlong base, jint weight) {
    return TypefaceExact(env, cls, base, weight, base & 1);
}
jlong TypefaceByStyle(JNIEnv* env, jclass cls, jlong base, jint style) {
    const auto previous = TypefaceWeight(env, cls, base);
    return TypefaceExact(env, cls, base, style & 1 ? 700 : previous >= 600 ? 400 : previous, style & 2);
}
jlong HostTypeface(JNIEnv* env, jclass, jstring family, jint weight, jboolean italic) {
    return CallSafely(env, [&] {
        const auto name = family ? Utf8(env, family) : std::string{};
        const jint face = name == "monospace" ? WA_FONT_MONOSPACE : name == "system-ui" ? WA_FONT_SYSTEM : WA_FONT_ROBOTO;
        return EncodeTypeface(face, weight, italic);
    });
}
jlong TypefaceWithFamilies(JNIEnv* env, jclass cls, jlongArray, jlong fallback, jint weight, jint italic) {
    return TypefaceExact(env, cls, fallback, weight < 0 ? TypefaceWeight(env, cls, fallback) : weight,
                         italic < 0 ? (fallback & 1) : italic != 0);
}
jlong TypefaceVariation(JNIEnv* env, jclass cls, jlong base, jobject axes) {
    return CallSafely(env, [&] {
        jint weight = TypefaceWeight(env, cls, base);
        bool italic = base & 1;
        if (axes) {
            auto list = env->GetObjectClass(axes);
            auto size = env->GetMethodID(list, "size", "()I");
            auto get = env->GetMethodID(list, "get", "(I)Ljava/lang/Object;");
            if (!size || !get) return jlong{};
            const jint count = env->CallIntMethod(axes, size);
            for (jint i = 0; i < count && !env->ExceptionCheck(); ++i) {
                const auto axis = env->CallObjectMethod(axes, get, i);
                if (!axis) continue;
                const auto type = env->GetObjectClass(axis);
                const auto tag_id = env->GetMethodID(type, "getTag", "()Ljava/lang/String;");
                const auto value_id = env->GetMethodID(type, "getStyleValue", "()F");
                if (!tag_id || !value_id) return jlong{};
                const auto tag = static_cast<jstring>(env->CallObjectMethod(axis, tag_id));
                const auto name = Utf8(env, tag);
                const auto value = env->CallFloatMethod(axis, value_id);
                if (std::isfinite(value)) {
                    if (name == "wght") weight = std::clamp(value, 1.f, 1000.f);
                    else if (name == "ital") italic = value >= 0.5f;
                }
                env->DeleteLocalRef(tag);
                env->DeleteLocalRef(type);
                env->DeleteLocalRef(axis);
            }
        }
        return TypefaceExact(env, cls, base, weight, italic);
    });
}
jintArray TypefaceAxes(JNIEnv* env, jclass, jlong) {
    const jint tags[] = {0x6974616c, 0x77676874}; // ital, wght in the native OpenType tag order
    auto result = env->NewIntArray(std::size(tags));
    if (result) env->SetIntArrayRegion(result, 0, std::size(tags), tags);
    return result;
}
void TypefaceDefault(JNIEnv* env, jclass, jlong face) {
    if (auto* state = GetState(env)) state->default_typeface = face;
}
void RegisterGenericTypeface(JNIEnv*, jclass, jstring, jlong) {
    // Java owns the generic family map. The display list uses the face encoded in the value.
}
} // namespace

void SetRect(JNIEnv* env, jobject rect, wa_rect_t values, bool floating) {
    RectFields(env, rect, values, floating);
}

void RegisterPaint(interp::Vm& vm) {
    constexpr const char* name = "android/graphics/Paint";
    if (!RegisterIfDeclared(vm, name, Method<InitPaint>(vm, "nInit", "()J"))) {
        Register(vm, name, Method<MeasureText>(vm, "nativeMeasure", "(Ljava/lang/String;FII[F)F"));
        return;
    }
    Register(vm, name,
             Method<RetainPeer<PaintData, &State::paints>>(vm, "nRegisterNativeAllocation", "(Landroid/graphics/Paint;J)V"),
             Method<ManagedFinalizer>(vm, "nGetNativeFinalizer", "()J"),
             Method<CopyPaint>(vm, "nInitWithPaint", "(J)J"),
             Method<ResetPaint>(vm, "nReset", "(J)V"),
             Method<SetPaint>(vm, "nSet", "(JJ)V"),
             Method<FontCompatibility>(vm, "isUiFontChangeEnabled", "(J)Z"),
             Method<PaintSet<&PaintData::color, jint>>(vm, "nSetColor", "(JI)V"),
             Method<PaintColorLong>(vm, "nSetColor", "(JJJ)V"),
             Method<PaintAlpha>(vm, "nSetAlpha", "(JI)V"),
             Method<PaintFlag<0x01>>(vm, "nSetAntiAlias", "(JZ)V"),
             Method<PaintFlag<0x02>>(vm, "nSetFilterBitmap", "(JZ)V"),
             Method<PaintFlag<0x04>>(vm, "nSetDither", "(JZ)V"),
             Method<PaintFlag<0x08>>(vm, "nSetUnderlineText", "(JZ)V"),
             Method<PaintFlag<0x10>>(vm, "nSetStrikeThruText", "(JZ)V"),
             Method<PaintFlag<0x20>>(vm, "nSetFakeBoldText", "(JZ)V"),
             Method<PaintFlag<0x40>>(vm, "nSetLinearText", "(JZ)V"),
             Method<PaintFlag<0x80>>(vm, "nSetSubpixelText", "(JZ)V"),
             Method<PaintGet<&PaintData::flags, jint>>(vm, "nGetFlags", "(J)I"),
             Method<PaintSet<&PaintData::flags, jint>>(vm, "nSetFlags", "(JI)V"),
             Method<PaintGet<&PaintData::style, jint>>(vm, "nGetStyle", "(J)I"),
             Method<PaintSet<&PaintData::style, jint>>(vm, "nSetStyle", "(JI)V"),
             Method<PaintGet<&PaintData::cap, jint>>(vm, "nGetStrokeCap", "(J)I"),
             Method<PaintSet<&PaintData::cap, jint>>(vm, "nSetStrokeCap", "(JI)V"),
             Method<PaintGet<&PaintData::join, jint>>(vm, "nGetStrokeJoin", "(J)I"),
             Method<PaintSet<&PaintData::join, jint>>(vm, "nSetStrokeJoin", "(JI)V"),
             Method<PaintGet<&PaintData::miter, jfloat>>(vm, "nGetStrokeMiter", "(J)F"),
             Method<PaintSet<&PaintData::miter, jfloat>>(vm, "nSetStrokeMiter", "(JF)V"),
             Method<PaintGet<&PaintData::stroke_width, jfloat>>(vm, "nGetStrokeWidth", "(J)F"),
             Method<PaintSet<&PaintData::stroke_width, jfloat>>(vm, "nSetStrokeWidth", "(JF)V"),
             Method<PaintGet<&PaintData::hinting, jint>>(vm, "nGetHinting", "(J)I"),
             Method<PaintSet<&PaintData::hinting, jint>>(vm, "nSetHinting", "(JI)V"),
             Method<PaintGet<&PaintData::elegant, jint>>(vm, "nGetElegantTextHeight", "(J)I"),
             Method<PaintSet<&PaintData::elegant, jint>>(vm, "nSetElegantTextHeight", "(JI)V"),
             Method<PaintGet<&PaintData::align, jint>>(vm, "nGetTextAlign", "(J)I"),
             Method<PaintSet<&PaintData::align, jint>>(vm, "nSetTextAlign", "(JI)V"),
             Method<PaintGet<&PaintData::text_size, jfloat>>(vm, "nGetTextSize", "(J)F"),
             Method<PaintSet<&PaintData::text_size, jfloat>>(vm, "nSetTextSize", "(JF)V"),
             Method<PaintGet<&PaintData::text_scale, jfloat>>(vm, "nGetTextScaleX", "(J)F"),
             Method<PaintSet<&PaintData::text_scale, jfloat>>(vm, "nSetTextScaleX", "(JF)V"),
             Method<PaintGet<&PaintData::text_skew, jfloat>>(vm, "nGetTextSkewX", "(J)F"),
             Method<PaintSet<&PaintData::text_skew, jfloat>>(vm, "nSetTextSkewX", "(JF)V"),
             Method<PaintGet<&PaintData::letter_spacing, jfloat>>(vm, "nGetLetterSpacing", "(J)F"),
             Method<PaintSet<&PaintData::letter_spacing, jfloat>>(vm, "nSetLetterSpacing", "(JF)V"),
             Method<PaintGet<&PaintData::word_spacing, jfloat>>(vm, "nGetWordSpacing", "(J)F"),
             Method<PaintSet<&PaintData::word_spacing, jfloat>>(vm, "nSetWordSpacing", "(JF)V"),
             Method<PaintGet<&PaintData::start_hyphen, jint>>(vm, "nGetStartHyphenEdit", "(J)I"),
             Method<PaintSet<&PaintData::start_hyphen, jint>>(vm, "nSetStartHyphenEdit", "(JI)V"),
             Method<PaintGet<&PaintData::end_hyphen, jint>>(vm, "nGetEndHyphenEdit", "(J)I"),
             Method<PaintSet<&PaintData::end_hyphen, jint>>(vm, "nSetEndHyphenEdit", "(JI)V"),
             Method<PaintLocale>(vm, "nSetTextLocales", "(JLjava/lang/String;)I"),
             Method<PaintSet<&PaintData::locale, jint>>(vm, "nSetTextLocalesByMinikinLocaleListId", "(JI)V"),
             Method<PaintFeatures>(vm, "nSetFontFeatureSettings", "(JLjava/lang/String;)V"),
             Method<PaintShadow>(vm, "nSetShadowLayer", "(JFFFJJ)V"),
             Method<PaintHasShadow>(vm, "nHasShadowLayer", "(J)Z"),
             Method<PaintExchange<&PaintData::shader>>(vm, "nSetShader", "(JJ)J"),
             Method<PaintExchange<&PaintData::color_filter>>(vm, "nSetColorFilter", "(JJ)J"),
             Method<PaintExchange<&PaintData::mask_filter>>(vm, "nSetMaskFilter", "(JJ)J"),
             Method<PaintExchange<&PaintData::path_effect>>(vm, "nSetPathEffect", "(JJ)J"),
             Method<PaintTypeface>(vm, "nSetTypeface", "(JJ)V"),
             Method<PaintSet<&PaintData::blend, jint>>(vm, "nSetXfermode", "(JI)V"),
             Method<PaintMetrics>(vm, "nGetFontMetrics", "(JLandroid/graphics/Paint$FontMetrics;Z)F"),
             Method<PaintMetricsInt>(vm, "nGetFontMetricsInt", "(JLandroid/graphics/Paint$FontMetricsInt;Z)I"),
             Method<PaintTextMetrics<jstring>>(vm, "nGetFontMetricsIntForText", "(JLjava/lang/String;IIIIZLandroid/graphics/Paint$FontMetricsInt;)V"),
             Method<PaintTextMetrics<jcharArray>>(vm, "nGetFontMetricsIntForText", "(J[CIIIIZLandroid/graphics/Paint$FontMetricsInt;)V"),
             Method<PaintAscent>(vm, "nAscent", "(J)F"),
             Method<PaintDescent>(vm, "nDescent", "(J)F"),
             Method<DecorationSize<10>>(vm, "nGetUnderlinePosition", "(J)F"),
             Method<DecorationSize<5, true>>(vm, "nGetUnderlineThickness", "(J)F"),
             Method<DecorationSize<-30>>(vm, "nGetStrikeThruPosition", "(J)F"),
             Method<DecorationSize<5, true>>(vm, "nGetStrikeThruThickness", "(J)F"),
             Method<PaintAdvances<jstring>>(vm, "nGetTextAdvances", "(JLjava/lang/String;IIIII[FI)F"),
             Method<PaintAdvances<jcharArray>>(vm, "nGetTextAdvances", "(J[CIIIII[FI)F"),
             Method<PaintBounds<jstring>>(vm, "nGetStringBounds", "(JLjava/lang/String;IIILandroid/graphics/Rect;)V"),
             Method<PaintBounds<jcharArray>>(vm, "nGetCharArrayBounds", "(J[CIIILandroid/graphics/Rect;)V"),
             Method<BreakString>(vm, "nBreakText", "(JLjava/lang/String;ZFI[F)I"),
             Method<BreakArray>(vm, "nBreakText", "(J[CIIFI[F)I"),
             Method<PaintCursor<jstring>>(vm, "nGetTextRunCursor", "(JLjava/lang/String;IIIII)I"),
             Method<PaintCursor<jcharArray>>(vm, "nGetTextRunCursor", "(J[CIIIII)I"),
             Method<PaintRunAdvance>(vm, "nGetRunAdvance", "(J[CIIIIZI)F"),
             Method<GetRunCharacterAdvance>(vm, "nGetRunCharacterAdvance",
                 "(J[CIIIIZI[FILandroid/graphics/RectF;Landroid/graphics/Paint$RunInfo;)F"),
             Method<GetRunCharacterAdvanceLegacy>(vm, "nGetRunCharacterAdvance",
                 "(J[CIIIIZI[FILandroid/graphics/RectF;)F"),
             Method<OffsetForAdvance>(vm, "nGetOffsetForAdvance", "(J[CIIIIZF)I"),
             Method<NewFontVariation>(vm, "nCreateFontVariationBuilder", "(I)J"),
             Method<AddFontVariation>(vm, "nAddFontVariationToBuilder", "(JIF)V"),
             Method<SetFontVariation>(vm, "nSetFontVariationOverride", "(JJ)V"),
             Method<PaintEqualForMeasurement>(vm, "nEqualsForTextMeasurement", "(JJ)Z"),
             Method<PaintHasGlyph>(vm, "nHasGlyph", "(JILjava/lang/String;)Z"),
             Method<PaintFillPath>(vm, "nGetFillPath", "(JJJ)Z"));
}

void RegisterTypeface(interp::Vm& vm) {
    constexpr const char* name = "android/graphics/Typeface";
    if (!RegisterIfDeclared(vm, name, Method<TypefaceWeight>(vm, "nativeGetWeight", "(J)I"))) return;
    Register(vm, name,
             Method<HostTypeface>(vm, "nCreateHostFamily", "(Ljava/lang/String;IZ)J"),
             Method<TypefaceStyle>(vm, "nativeGetStyle", "(J)I"),
             Method<TypefaceByStyle>(vm, "nativeCreateFromTypeface", "(JI)J"),
             Method<TypefaceExact>(vm, "nativeCreateFromTypefaceWithExactStyle", "(JIZ)J"),
             Method<TypefaceVariation>(vm, "nativeCreateFromTypefaceWithVariation", "(JLjava/util/List;)J"),
             Method<TypefaceAlias>(vm, "nativeCreateWeightAlias", "(JI)J"),
             Method<TypefaceWithFamilies>(vm, "nativeCreateFromArray", "([JJII)J"),
             Method<TypefaceAxes>(vm, "nativeGetSupportedAxes", "(J)[I"),
             Method<TypefaceDefault>(vm, "nativeSetDefault", "(J)V"),
             Method<ManagedFinalizer>(vm, "nativeGetReleaseFunc", "()J"),
             Method<RegisterGenericTypeface>(vm, "nativeRegisterGenericFamily", "(Ljava/lang/String;J)V"));
}
