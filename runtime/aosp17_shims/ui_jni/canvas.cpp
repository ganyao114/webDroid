#include "native_graphics.h"

#include "jni_bridge.h"
#include "software_raster.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <numbers>
#include <tuple>

namespace wa::ui_jni::detail {

struct MatrixStore {

    struct Entry {
        Matrix9 values;
        bool registered = false;
    };
    std::unordered_map<jlong, Entry> entries;
    jlong next_id = 1;
};

namespace {

MatrixStore::Entry* FindMatrix(JNIEnv* env, State& state, jlong handle) {
    if (state.matrices) {
        const auto it = state.matrices->entries.find(handle);
        if (it != state.matrices->entries.end()) return &it->second;
    }
    Throw(env, "java/lang/IllegalStateException", "Invalid native Matrix handle");
    return nullptr;
}

jlong CreateMatrix(JNIEnv* env, jclass, jlong source) {
    return CallSafely(env, [&]() -> jlong {
        State* state = GetState(env);
        if (!state) return 0;
        auto values = kIdentity;
        if (source != 0) {
            auto* entry = FindMatrix(env, *state, source);
            if (!entry) return 0;
            values = entry->values;
        }
        if (!state->matrices) state->matrices = std::make_shared<MatrixStore>();
        auto& store = *state->matrices;
        if (store.next_id == std::numeric_limits<jlong>::max()) {
            Throw(env, "java/lang/OutOfMemoryError", "Matrix handles exhausted");
            return 0;
        }
        const jlong handle = store.next_id++;
        store.entries.emplace(handle, MatrixStore::Entry{values});
        return handle;
    });
}

void RetainMatrix(JNIEnv* env, jclass, jobject peer, jlong handle) {
    CallSafely(env, [&] {
        State* state = GetState(env);
        if (!state) return;
        auto* entry = FindMatrix(env, *state, handle);
        if (!entry) return;
        if (entry->registered) {
            Throw(env, "java/lang/IllegalStateException", "Matrix already has a managed owner");
            return;
        }
        // The deleter also removes an allocation if registering its owner fails.
        std::shared_ptr<void> allocation(entry, [store = std::weak_ptr(state->matrices), handle](void*) {
            if (const auto live = store.lock()) live->entries.erase(handle);
        });
        interp::JniBridge::From(env).RetainNativePeer(peer, std::move(allocation));
        entry->registered = true;
    });
}

} // namespace

Matrix9* MatrixValues(JNIEnv* env, jlong handle) {
    State* state = GetState(env);
    if (!state) return nullptr;
    auto* entry = FindMatrix(env, *state, handle);
    return entry ? &entry->values : nullptr;
}

namespace {

jboolean IsIdentityMatrix(JNIEnv* env, jclass, jlong handle) {
    const auto* values = MatrixValues(env, handle);
    return values && *values == kIdentity;
}

void ResetMatrix(JNIEnv* env, jclass, jlong handle) {
    if (auto* values = MatrixValues(env, handle)) *values = kIdentity;
}

void CopyMatrix(JNIEnv* env, jclass, jlong handle, jlong source) {
    if (auto* dest = MatrixValues(env, handle)) {
        if (const auto* src = MatrixValues(env, source)) *dest = *src;
    }
}

void TranslateMatrix(JNIEnv* env, jclass, jlong handle, jfloat x, jfloat y) {
    if (auto* values = MatrixValues(env, handle)) *values = {1, 0, x, 0, 1, y, 0, 0, 1};
}

void GetMatrixValues(JNIEnv* env, jclass, jlong handle, jfloatArray array) {
    if (const auto* values = MatrixValues(env, handle)) {
        env->SetFloatArrayRegion(array, 0, values->size(), values->data());
    }
}

void SetMatrixValues(JNIEnv* env, jclass, jlong handle, jfloatArray array) {
    if (auto* values = MatrixValues(env, handle)) {
        env->GetFloatArrayRegion(array, 0, values->size(), values->data());
    }
}

void MapMatrixPoints(JNIEnv* env, jclass, jlong handle, jfloatArray dst, jint dst_index,
                     jfloatArray src, jint src_index, jint count, jboolean include_translation) {
    CallSafely(env, [&] {
        const auto* values = MatrixValues(env, handle);
        if (!values) return;
        const auto size = 2 * static_cast<int64_t>(count);
        if (count < 0 || src_index < 0 || dst_index < 0 ||
            src_index + size > env->GetArrayLength(src) || dst_index + size > env->GetArrayLength(dst)) {
            Throw(env, "java/lang/ArrayIndexOutOfBoundsException", "Matrix point range");
            return;
        }
        if (size == 0) return;
        // Read before writing because Java permits overlapping source and destination ranges.
        std::vector<jfloat> points(size);
        env->GetFloatArrayRegion(src, src_index, size, points.data());
        if (env->ExceptionCheck()) return;
        const auto& m = *values;
        const auto origin = MapPoint(m, 0, 0);
        const bool affine = m[6] == 0 && m[7] == 0 && m[8] == 1;
        for (size_t i = 0; i < points.size(); i += 2) {
            const jfloat x = points[i], y = points[i + 1];
            if (!include_translation && affine) {
                points[i] = m[0] * x + m[1] * y;
                points[i + 1] = m[3] * x + m[4] * y;
            } else {
                const auto mapped = MapPoint(m, x, y);
                points[i] = include_translation ? mapped[0] : mapped[0] - origin[0];
                points[i + 1] = include_translation ? mapped[1] : mapped[1] - origin[1];
            }
        }
        env->SetFloatArrayRegion(dst, dst_index, size, points.data());
    });
}

} // namespace
std::optional<Matrix9> Inverse(const Matrix9& m) {
    const auto& [a, b, c, d, e, f, g, h, i] = m;
    const auto det2 = [](double x, double y, double z, double w) { return x * w - y * z; };
    const std::array<double, 9> adj{
        det2(e, f, h, i), det2(c, b, i, h), det2(b, c, e, f),
        det2(f, d, i, g), det2(a, c, g, i), det2(c, a, f, d),
        det2(d, e, g, h), det2(b, a, h, g), det2(a, b, d, e)};
    const double det = a * adj[0] + b * adj[3] + c * adj[6];
    if (det == 0 || !std::isfinite(det)) return std::nullopt;
    Matrix9 result;
    for (size_t pos = 0; pos < result.size(); ++pos) {
        result[pos] = static_cast<jfloat>(adj[pos] / det);
        if (!std::isfinite(result[pos])) return std::nullopt;
    }
    return result;
}

wa_rect_t MapRect(const Matrix9& m, wa_rect_t rect) {
    const auto [l, t, r, b] = rect;
    const auto a = MapPoint(m, l, t), c = MapPoint(m, r, t);
    const auto d = MapPoint(m, l, b), e = MapPoint(m, r, b);
    return {std::min({a[0], c[0], d[0], e[0]}), std::min({a[1], c[1], d[1], e[1]}),
            std::max({a[0], c[0], d[0], e[0]}), std::max({a[1], c[1], d[1], e[1]})};
}
namespace {

jboolean InvertMatrix(JNIEnv* env, jclass, jlong handle, jlong inverse) {
    const auto* matrix = MatrixValues(env, handle);
    if (!matrix) return false;
    const auto result = Inverse(*matrix);
    if (!result) return false;
    if (inverse) {
        auto* output = MatrixValues(env, inverse);
        if (!output) return false;
        *output = *result;
    }
    return true;
}

#include "matrix.inc"

void Shape(JNIEnv* env, jclass, jint op, jfloat a, jfloat b, jfloat c, jfloat d, jfloat e, jfloat f,
           jint color, jint style, jfloat stroke) {
    State* state = GetState(env);
    if (!state)
        return;
    auto* canvas = RecordingCanvas(env, *state);
    if (!canvas)
        return;

    auto emit = [&](uint32_t paint_style) -> int32_t {
        const wa_paint_t paint{static_cast<uint32_t>(color), paint_style, stroke};
        const wa_rect_t rect{a, b, c, d};
        switch (op) {
        case WA_CANVAS_SAVE:
            return wa_canvas_save(canvas);
        case WA_CANVAS_RESTORE:
            return wa_canvas_restore(canvas);
        case WA_CANVAS_TRANSLATE:
            return wa_canvas_translate(canvas, a, b);
        case WA_CANVAS_SCALE:
            return wa_canvas_scale(canvas, a, b);
        case WA_CANVAS_ROTATE:
            return wa_canvas_rotate(canvas, a, b, c);
        case WA_CANVAS_CLIP_RECT:
            return wa_canvas_clip_rect(canvas, rect);
        case WA_CANVAS_CLIP_ROUND_RECT:
            return wa_canvas_clip_round_rect(canvas, {rect, e, f});
        case WA_CANVAS_DRAW_COLOR:
            return wa_canvas_draw_color(canvas, color);
        case WA_CANVAS_DRAW_RECT:
            return wa_canvas_draw_rect(canvas, rect, paint);
        case WA_CANVAS_DRAW_ROUND_RECT:
            return wa_canvas_draw_round_rect(canvas, {rect, e, f}, paint);
        case WA_CANVAS_DRAW_CIRCLE:
            return wa_canvas_draw_circle(canvas, a, b, c, paint);
        case WA_CANVAS_DRAW_LINE:
            return wa_canvas_draw_line(canvas, a, b, c, d, paint);
        default:
            return WA_SF_INVALID;
        }
    };
    state->result = emit(style == 1 ? WA_PAINT_STROKE : WA_PAINT_FILL);
    if (state->result == WA_SF_OK && style == 2)
        state->result = emit(WA_PAINT_STROKE);
}

void DrawPath(JNIEnv* env, jclass, jfloatArray data, jint count, jint fill, jint color, jint style,
              jfloat stroke_width) {
    CallSafely(env, [&] {
        State* state = GetState(env);
        if (!state)
            return;
        auto* canvas = RecordingCanvas(env, *state);
        if (!canvas)
            return;
        if (count <= 0 || count > env->GetArrayLength(data) / 7) {
            state->result = WA_SF_INVALID;
            return;
        }
        if (count > static_cast<jint>(WA_SF_LAYER_LIST_CAPACITY / sizeof(uint32_t))) {
            state->result = WA_SF_FULL;
            return;
        }
        auto& commands = state->path_coordinates;
        commands.resize(count * 7);
        env->GetFloatArrayRegion(data, 0, commands.size(), commands.data());
        if (env->ExceptionCheck())
            return;
        auto& verbs = state->path_verbs;
        verbs.resize(count);
        for (jint i = 0; i < count; ++i) {
            const jfloat* at = &commands[i * 7];
            if (!std::isfinite(at[0]) || at[0] < WA_PATH_MOVE || at[0] > WA_PATH_CLOSE) {
                state->result = WA_SF_INVALID;
                return;
            }
            auto& verb = verbs[i];
            verb.command = static_cast<uint32_t>(at[0]);
            std::copy_n(at + 1, 6, verb.points);
        }
        wa_paint_t paint{static_cast<uint32_t>(color), style == 1 ? WA_PAINT_STROKE : WA_PAINT_FILL,
                         stroke_width};
        state->result = wa_canvas_draw_path(canvas, verbs.data(), count, fill, paint);
        if (state->result == WA_SF_OK && style == 2) {
            paint.style = WA_PAINT_STROKE;
            state->result = wa_canvas_draw_path(canvas, verbs.data(), count, fill, paint);
        }
    });
}

void DrawText(JNIEnv* env, jclass, jstring text, jfloat x, jfloat y, jfloat size, jint weight,
              jint align, jint color, jint face) {
    CallSafely(env, [&] {
        State* state = GetState(env);
        if (!state)
            return;
        auto* canvas = RecordingCanvas(env, *state);
        if (!canvas)
            return;
        std::string utf8 = Utf8(env, text);
        if (env->ExceptionCheck())
            return;
        if (utf8.size() > WA_SF_LAYER_LIST_CAPACITY) {
            state->result = WA_SF_FULL;
            return;
        }
        const wa_canvas_text_t draw{0,
                                    static_cast<uint32_t>(utf8.size()),
                                    x,
                                    y,
                                    0,
                                    size,
                                    static_cast<uint32_t>(weight),
                                    static_cast<uint32_t>(align),
                                    static_cast<uint32_t>(color),
                                    static_cast<uint32_t>(face)};
        state->result = wa_canvas_draw_text(canvas, &draw, utf8.data());
    });
}

jfloat MeasureText(JNIEnv* env, jclass, jstring text, jfloat size, jint weight, jint face,
                   jfloatArray metrics) {
    return CallSafely(env, [&] {
        State* state = GetState(env);
        if (!state)
            return 0.f;
        std::string utf8 = Utf8(env, text);
        if (env->ExceptionCheck())
            return 0.f;
        const wa_text_metrics_t result =
            wa_canvas_measure_text(state->sf, utf8.data(), utf8.size(), size, weight, face);
        if (metrics) {
            const jfloat values[] = {result.ascent, result.descent};
            env->SetFloatArrayRegion(metrics, 0, 2, values);
        }
        return result.advance;
    });
}

uint32_t PublishBitmapPixels(State& state, std::span<const jint> argb, jint width, jint height,
                             jlong identity, jint generation, jboolean has_alpha, uint32_t tint = 0) {
    for (const auto& image : state.images) {
        if (identity && image.identity == identity && image.generation == generation && image.tint == tint)
            return image.id;
    }
    const size_t count = argb.size();
    std::vector<uint8_t> rgba(count * 4);
    for (size_t i = 0; i < count; ++i) {
        uint32_t color = static_cast<uint32_t>(argb[i]) | tint;
        rgba[i * 4] = color >> 16;
        rgba[i * 4 + 1] = color >> 8;
        rgba[i * 4 + 2] = color;
        rgba[i * 4 + 3] = has_alpha ? color >> 24 : 255;
    }
    auto* store = wa_sf_bitmaps(state.sf);
    auto next_id = [&] {
        uint32_t id = state.next_image_id++;
        if (state.next_image_id == 0)
            state.next_image_id = 0x80000000u;
        return id;
    };
    uint32_t id = next_id();
    while (const auto* existing = wa_bitmap_acquire(store, id)) {
        wa_bitmap_release(store, existing);
        id = next_id();
    }
    state.images.push_back({identity, generation, id, tint});
    state.result = wa_bitmap_put(store, id, width, height, width * 4, rgba.data());
    if (state.result != WA_SF_OK) {
        state.images.pop_back();
        return 0;
    }
    return id;
}


uint32_t PublishBitmap(JNIEnv* env, State& state, jintArray pixels, jint width, jint height,
                       jlong identity, jint generation, jboolean has_alpha) {
    for (const auto& image : state.images) {
        if (image.identity == -identity && image.generation == generation)
            return image.id;
    }
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        env->GetArrayLength(pixels) != width * height) {
        state.result = WA_SF_INVALID;
        return 0;
    }

    const size_t count = static_cast<size_t>(width) * height;
    std::vector<jint> argb(count);
    env->GetIntArrayRegion(pixels, 0, argb.size(), argb.data());
    if (env->ExceptionCheck())
        return 0;
    return PublishBitmapPixels(state, argb, width, height, -identity, generation, has_alpha);
}

void DrawBitmap(JNIEnv* env, jclass, jintArray pixels, jint width, jint height, jlong identity,
                jint generation, jboolean has_alpha, jfloat sl, jfloat st, jfloat sr, jfloat sb,
                jfloat dl, jfloat dt, jfloat dr, jfloat db, jfloat alpha, jint mask,
                jfloat radius) {
    CallSafely(env, [&] {
        State* state = GetState(env);
        if (!state)
            return;
        auto* canvas = RecordingCanvas(env, *state);
        if (!canvas)
            return;
        uint32_t id =
            PublishBitmap(env, *state, pixels, width, height, identity, generation, has_alpha);
        if (id == 0)
            return;
        if (!std::ranges::contains(state->recording_images, id))
            state->recording_images.push_back(id);
        const wa_canvas_bitmap_t draw{
            id, static_cast<uint32_t>(mask), {sl, st, sr, sb}, {dl, dt, dr, db}, radius, alpha};
        state->result = wa_canvas_draw_bitmap(canvas, &draw);
    });
}

void DrawGradient(JNIEnv* env, jclass, jfloat l, jfloat t, jfloat r, jfloat b, jfloat radius,
                  jint first, jint last, jfloat degrees) {
    State* state = GetState(env);
    if (!state)
        return;
    auto* canvas = RecordingCanvas(env, *state);
    if (!canvas)
        return;
    const wa_canvas_gradient_t draw{{{l, t, r, b}, radius, radius},
                                    static_cast<uint32_t>(first),
                                    static_cast<uint32_t>(last),
                                    degrees};
    state->result = wa_canvas_draw_linear_gradient(canvas, &draw);
}

} // namespace

#include "paint.inc"
#include "paint_effects.inc"
#include "recording_canvas.inc"
#include "vector_drawable.inc"

void RegisterCanvas(interp::Vm& vm) {
    if (!RegisterModernCanvas(vm)) Register(vm, "android/graphics/Canvas", Method<Shape>(vm, "nativeShape", "(IFFFFFFIIF)V"),
             Method<DrawPath>(vm, "nativeDrawPath", "([FIIIIF)V"),
             Method<DrawText>(vm, "nativeDrawText", "(Ljava/lang/String;FFFIIII)V"),
             Method<DrawBitmap>(vm, "nativeDrawBitmap", "([IIIJIZFFFFFFFFFIF)V"),
             Method<DrawGradient>(vm, "nativeDrawGradient", "(FFFFFIIF)V"));
    RegisterPaint(vm);
    RegisterTypeface(vm);
    RegisterPaintEffects(vm);
}

void RegisterMatrix(interp::Vm& vm) {
    if (!vm.HasClass("Landroid/graphics/Matrix$ExtraNatives;")) return;
    Register(vm, "android/graphics/Matrix$ExtraNatives", Method<CreateMatrix>(vm, "nCreate", "(J)J"),
             Method<ManagedFinalizer>(vm, "nGetNativeFinalizer", "()J"));
    Register(vm, "android/graphics/Matrix",
             Method<RetainMatrix>(vm, "nRegisterNativeAllocation", "(Landroid/graphics/Matrix;J)V"),
             Method<IsIdentityMatrix>(vm, "nIsIdentity", "(J)Z"),
             Method<ResetMatrix>(vm, "nReset", "(J)V"),
             Method<CopyMatrix>(vm, "nSet", "(JJ)V"),
             Method<TranslateMatrix>(vm, "nSetTranslate", "(JFF)V"),
             Method<GetMatrixValues>(vm, "nGetValues", "(J[F)V"),
             Method<SetMatrixValues>(vm, "nSetValues", "(J[F)V"),
             Method<MapMatrixPoints>(vm, "nMapPoints", "(J[FI[FIIZ)V"),
             Method<InvertMatrix>(vm, "nInvert", "(JJ)Z"),
             Method<MatrixIsAffine>(vm, "nIsAffine", "(J)Z"),
             Method<MatrixRectStaysRect>(vm, "nRectStaysRect", "(J)Z"),
             Method<MatrixRotate<MatrixOp::Set>>(vm, "nSetRotate", "(JF)V"),
             Method<MatrixPivotRotate<MatrixOp::Set>>(vm, "nSetRotate", "(JFFF)V"),
             Method<MatrixSinCos>(vm, "nSetSinCos", "(JFF)V"),
             Method<MatrixPivotSinCos>(vm, "nSetSinCos", "(JFFFF)V"),
             Method<MatrixPair<MatrixOp::Set, Scaling>>(vm, "nSetScale", "(JFF)V"),
             Method<MatrixPivot<MatrixOp::Set, Scaling>>(vm, "nSetScale", "(JFFFF)V"),
             Method<MatrixPair<MatrixOp::Set, Skew>>(vm, "nSetSkew", "(JFF)V"),
             Method<MatrixPivot<MatrixOp::Set, Skew>>(vm, "nSetSkew", "(JFFFF)V"),
             Method<MatrixSetConcat>(vm, "nSetConcat", "(JJJ)V"),
             Method<MatrixPair<MatrixOp::Pre, Translation>>(vm, "nPreTranslate", "(JFF)V"),
             Method<MatrixPair<MatrixOp::Post, Translation>>(vm, "nPostTranslate", "(JFF)V"),
             Method<MatrixPair<MatrixOp::Pre, Scaling>>(vm, "nPreScale", "(JFF)V"),
             Method<MatrixPivot<MatrixOp::Pre, Scaling>>(vm, "nPreScale", "(JFFFF)V"),
             Method<MatrixPair<MatrixOp::Post, Scaling>>(vm, "nPostScale", "(JFF)V"),
             Method<MatrixPivot<MatrixOp::Post, Scaling>>(vm, "nPostScale", "(JFFFF)V"),
             Method<MatrixRotate<MatrixOp::Pre>>(vm, "nPreRotate", "(JF)V"),
             Method<MatrixPivotRotate<MatrixOp::Pre>>(vm, "nPreRotate", "(JFFF)V"),
             Method<MatrixRotate<MatrixOp::Post>>(vm, "nPostRotate", "(JF)V"),
             Method<MatrixPivotRotate<MatrixOp::Post>>(vm, "nPostRotate", "(JFFF)V"),
             Method<MatrixPair<MatrixOp::Pre, Skew>>(vm, "nPreSkew", "(JFF)V"),
             Method<MatrixPivot<MatrixOp::Pre, Skew>>(vm, "nPreSkew", "(JFFFF)V"),
             Method<MatrixPair<MatrixOp::Post, Skew>>(vm, "nPostSkew", "(JFF)V"),
             Method<MatrixPivot<MatrixOp::Post, Skew>>(vm, "nPostSkew", "(JFFFF)V"),
             Method<MatrixConcat<MatrixOp::Pre>>(vm, "nPreConcat", "(JJ)V"),
             Method<MatrixConcat<MatrixOp::Post>>(vm, "nPostConcat", "(JJ)V"),
             Method<MatrixMapRect>(vm, "nMapRect", "(JLandroid/graphics/RectF;Landroid/graphics/RectF;)Z"),
             Method<MatrixRectToRect>(vm, "nSetRectToRect", "(JLandroid/graphics/RectF;Landroid/graphics/RectF;I)Z"),
             Method<MatrixPolyToPoly>(vm, "nSetPolyToPoly", "(J[FI[FII)Z"),
             Method<MatrixMapRadius>(vm, "nMapRadius", "(JF)F"),
             Method<MatrixEquals>(vm, "nEquals", "(JJ)Z"));
}

} // namespace wa::ui_jni::detail
