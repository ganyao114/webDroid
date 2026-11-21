#include "native_methods.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wa::ui_jni::detail {
namespace {

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

uint32_t PublishBitmap(JNIEnv* env, State& state, jintArray pixels, jint width, jint height,
                       jlong identity, jint generation, jboolean has_alpha) {
    for (const auto& image : state.images) {
        if (image.identity == identity && image.generation == generation)
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
    std::vector<uint8_t> rgba(count * 4);
    for (size_t i = 0; i < count; ++i) {
        uint32_t color = static_cast<uint32_t>(argb[i]);
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
    state.images.push_back({identity, generation, id});
    state.result = wa_bitmap_put(store, id, width, height, width * 4, rgba.data());
    if (state.result != WA_SF_OK) {
        state.images.pop_back();
        return 0;
    }
    return id;
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

void RegisterCanvas(interp::Vm& vm) {
    Register(vm, "android/graphics/Canvas", Method<Shape>(vm, "nativeShape", "(IFFFFFFIIF)V"),
             Method<DrawPath>(vm, "nativeDrawPath", "([FIIIIF)V"),
             Method<DrawText>(vm, "nativeDrawText", "(Ljava/lang/String;FFFIIII)V"),
             Method<DrawBitmap>(vm, "nativeDrawBitmap", "([IIIJIZFFFFFFFFFIF)V"),
             Method<DrawGradient>(vm, "nativeDrawGradient", "(FFFFFIIF)V"));
    Register(vm, "android/graphics/Paint",
             Method<MeasureText>(vm, "nativeMeasure", "(Ljava/lang/String;FII[F)F"));
}

} // namespace wa::ui_jni::detail
