struct PathClip {
    std::vector<interp::software_raster::Edge> edges;
    bool even_odd;
};

struct CanvasClip {
    Matrix9 inverse;
    std::variant<PathClip, std::shared_ptr<const ShaderData>> source;
    bool difference;
};

struct CanvasData {
    struct Backdrop {
        wa_rect_t local;
        Matrix9 transform;
        jint left, top, width, height;
        std::vector<jint> pixels;
        std::vector<uint8_t> saved;
    };

    struct Frame {
        Matrix9 matrix = kIdentity;
        wa_rect_t clip{};
        float alpha = 1;
        std::vector<interp::Value> raster_clips;
        std::vector<std::shared_ptr<const CanvasClip>> masks;
        std::shared_ptr<Backdrop> backdrop;
    };
    jlong bitmap = 0;
    jint width = 0, height = 0;
    bool recording = false;
    uint64_t serial = 0;
    Frame frame;
    std::vector<Frame> stack;

    void Reset() {
        frame = {};
        frame.clip = {0, 0, float(width), float(height)};
        stack.clear();
    }
};

namespace {

using interp::Object;
using interp::Value;

struct CanvasTarget {
    State* state = nullptr;
    CanvasData* data = nullptr;
    BitmapData* bitmap = nullptr;
    wa_canvas_t* display = nullptr;

    explicit operator bool() const { return data != nullptr; }
};

CanvasTarget Canvas(JNIEnv* env, jlong id) {
    auto* state = GetState(env);
    auto* canvas = Peer<CanvasData, &State::canvases>(env, id);
    if (!state || !canvas) return {};
    if (canvas->recording && state->canvas && canvas->serial != state->recording_serial) {
        canvas->serial = state->recording_serial;
        canvas->Reset();
    }
    auto* bitmap = canvas->bitmap ? Bitmap(env, canvas->bitmap) : nullptr;
    if (bitmap && bitmap->recycled) {
        Throw(env, "java/lang/IllegalStateException", "Canvas bitmap has been recycled");
        return {};
    }
    return {state, canvas, bitmap, canvas->recording && state->result == WA_SF_OK ? state->canvas : nullptr};
}

jlong InitRasterCanvas(JNIEnv* env, jclass, jlong bitmap) {
    return CallSafely(env, [&]() -> jlong {
        auto* state = GetState(env);
        const auto* image = bitmap ? Bitmap(env, bitmap) : nullptr;
        if (!state || env->ExceptionCheck()) return 0;
        CanvasData canvas;
        canvas.bitmap = bitmap;
        canvas.width = image ? image->width : 0;
        canvas.height = image ? image->height : 0;
        canvas.Reset();
        return Peers<CanvasData, &State::canvases>(*state).Insert(std::move(canvas));
    });
}
jobject RetainCanvas(JNIEnv* env, jclass cls, jobject peer, jlong id) {
    RetainPeer<CanvasData, &State::canvases>(env, cls, peer, id);
    // This interpreter releases the native peer during GC.
    return nullptr;
}
void SetCanvasBitmap(JNIEnv* env, jclass, jlong id, jlong bitmap) {
    auto* canvas = Peer<CanvasData, &State::canvases>(env, id);
    const auto* image = bitmap ? Bitmap(env, bitmap) : nullptr;
    if (!canvas || env->ExceptionCheck()) return;
    canvas->bitmap = bitmap;
    canvas->width = image ? image->width : 0;
    canvas->height = image ? image->height : 0;
    canvas->recording = false;
    canvas->Reset();
}
void SetRecordingSize(JNIEnv* env, jclass, jlong id, jint width, jint height) {
    if (auto* canvas = Peer<CanvasData, &State::canvases>(env, id)) {
        canvas->width = width;
        canvas->height = height;
        canvas->recording = true;
        canvas->Reset();
    }
}
template <auto Field>
jint CanvasDimension(JNIEnv* env, jclass, jlong id) {
    const auto target = Canvas(env, id);
    return target ? target.data->*Field : 0;
}
jboolean CanvasIsOpaque(JNIEnv* env, jclass, jlong id) {
    const auto target = Canvas(env, id);
    return target.bitmap && !target.bitmap->has_alpha;
}
jboolean CanvasHighContrast(JNIEnv*, jclass, jlong) { return false; }

jint SaveCanvas(JNIEnv* env, jclass, jlong id, jint) {
    return CallSafely(env, [&]() -> jint {
        const auto canvas = Canvas(env, id);
        if (!canvas) return 0;
        const auto count = static_cast<jint>(canvas.data->stack.size() + 1);
        if (count > WA_CANVAS_SAVE_DEPTH) {
            Throw(env, "java/lang/IllegalStateException", "Canvas save stack is full");
            return count;
        }
        canvas.data->stack.push_back(canvas.data->frame);
        canvas.data->frame.backdrop.reset();
        if (canvas.display) canvas.state->result = wa_canvas_save(canvas.display);
        return count;
    });
}
jint CanvasSaveCount(JNIEnv* env, jclass, jlong id) {
    const auto canvas = Canvas(env, id);
    return canvas ? canvas.data->stack.size() + 1 : 0;
}
jboolean RestoreCanvas(JNIEnv* env, jclass, jlong id) {
    const auto canvas = Canvas(env, id);
    if (!canvas || canvas.data->stack.empty()) return false;
    if (const auto& backdrop = canvas.data->frame.backdrop; canvas.bitmap && backdrop) {
        auto& bitmap = *canvas.bitmap;
        for (jint y = 0; y < backdrop->height && y + backdrop->top < bitmap.height; ++y) {
            for (jint x = 0; x < backdrop->width && x + backdrop->left < bitmap.width; ++x) {
                const auto index = static_cast<size_t>(y) * backdrop->width + x;
                if (!backdrop->saved[index]) continue;
                auto& pixel = bitmap.pixels[static_cast<size_t>(y + backdrop->top) * bitmap.width + x + backdrop->left];
                pixel = bitmap.PutColor(interp::software_raster::SourceOver(pixel, backdrop->pixels[index]));
            }
        }
        bitmap.Changed();
    }
    canvas.data->frame = std::move(canvas.data->stack.back());
    canvas.data->stack.pop_back();
    if (canvas.display) canvas.state->result = wa_canvas_restore(canvas.display);
    return true;
}
void RestoreCanvasToCount(JNIEnv* env, jclass cls, jlong id, jint count) {
    if (count < 1) {
        Throw(env, "java/lang/IllegalArgumentException", "Canvas save count must be positive");
        return;
    }
    for (jint depth = CanvasSaveCount(env, cls, id); depth > count; --depth)
        if (!RestoreCanvas(env, cls, id)) break;
}

void EmitMatrix(const CanvasTarget& canvas, const Matrix9& m) {
    auto* display = canvas.display;
    if (!display || m == kIdentity) return;
    // The display list has translation, rotation and scale commands. An affine
    // transform factors into a translation and two rotations around signed scale.
    auto& status = canvas.state->result;
    if (m[2] || m[5]) status = wa_canvas_translate(display, m[2], m[5]);
    if (status != WA_SF_OK) return;
    if (m[1] == 0 && m[3] == 0) {
        if (m[0] != 1 || m[4] != 1) status = wa_canvas_scale(display, m[0], m[4]);
        return;
    }
    const double a = m[0], b = m[1], c = m[3], d = m[4];
    const double v = std::atan2(2 * (a * b + c * d), a * a + c * c - b * b - d * d) / 2;
    const double x = a * std::cos(v) + b * std::sin(v), y = c * std::cos(v) + d * std::sin(v);
    const double sx = std::hypot(x, y);
    const double degrees = 180 / std::numbers::pi_v<double>;
    const double u = std::atan2(y, x);
    if (u != 0) status = wa_canvas_rotate(display, u * degrees, 0, 0);
    if (status == WA_SF_OK) status = wa_canvas_scale(display, sx, sx > 0 ? (a * d - b * c) / sx : 0);
    if (status == WA_SF_OK && v != 0) status = wa_canvas_rotate(display, -v * degrees, 0, 0);
}
void ConcatCanvas(const CanvasTarget& canvas, const Matrix9& matrix) {
    canvas.data->frame.matrix = Multiply(canvas.data->frame.matrix, matrix);
    EmitMatrix(canvas, matrix);
}
template <auto Transformation>
void TransformCanvas(JNIEnv* env, jclass, jlong id, jfloat x, jfloat y) {
    if (auto canvas = Canvas(env, id)) ConcatCanvas(canvas, Transformation(x, y));
}
void RotateCanvas(JNIEnv* env, jclass, jlong id, jfloat degrees) {
    if (auto canvas = Canvas(env, id)) {
        canvas.data->frame.matrix = Multiply(canvas.data->frame.matrix, Degrees(degrees));
        if (canvas.display) canvas.state->result = wa_canvas_rotate(canvas.display, degrees, 0, 0);
    }
}
void ConcatCanvasMatrix(JNIEnv* env, jclass, jlong id, jlong matrix) {
    if (const auto* m = MatrixValues(env, matrix))
        if (auto canvas = Canvas(env, id)) ConcatCanvas(canvas, *m);
}
void ConcatCanvas44(JNIEnv* env, jclass, jlong id, jfloatArray array) {
    std::array<float, 16> matrix{};
    env->GetFloatArrayRegion(array, 0, matrix.size(), matrix.data());
    if (env->ExceptionCheck()) return;
    if (auto canvas = Canvas(env, id))
        ConcatCanvas(canvas, {matrix[0], matrix[1], matrix[3], matrix[4], matrix[5], matrix[7],
                              matrix[12], matrix[13], matrix[15]});
}
void SetCanvasMatrix(JNIEnv* env, jclass, jlong id, jlong matrix) {
    auto canvas = Canvas(env, id);
    const auto* m = matrix ? MatrixValues(env, matrix) : &kIdentity;
    if (!canvas || !m) return;
    if (canvas.display) {
        if (const auto previous = Inverse(canvas.data->frame.matrix))
            EmitMatrix(canvas, Multiply(*previous, *m));
        else {
            Throw(env, "java/lang/IllegalStateException", "Cannot replace a singular recording transform");
            return;
        }
    }
    canvas.data->frame.matrix = *m;
}
void GetCanvasMatrix(JNIEnv* env, jclass, jlong id, jlong matrix) {
    if (auto canvas = Canvas(env, id))
        if (auto* m = MatrixValues(env, matrix)) *m = canvas.data->frame.matrix;
}
wa_rect_t Intersect(wa_rect_t a, wa_rect_t b) {
    return {std::max(a.l, b.l), std::max(a.t, b.t), std::min(a.r, b.r), std::min(a.b, b.b)};
}

bool ClipCanvas(const CanvasTarget& canvas, wa_round_rect_t clip) {
    auto& frame = canvas.data->frame;
    frame.clip = Intersect(frame.clip, MapRect(frame.matrix, clip.rect));
    if (!HasArea(frame.clip)) frame.clip = {};
    if (HasArea(frame.clip) && ((clip.rx > 0 && clip.ry > 0) || !RectStaysRect(frame.matrix))) {
        if (auto inverse = Inverse(frame.matrix)) {
            for (size_t i = 0; i < 6; ++i) frame.raster_clips.push_back(Value::Float((*inverse)[i]));
            const auto [l, t, r, b] = clip.rect;
            for (float value : {l, t, r, b, clip.rx, clip.ry})
                frame.raster_clips.push_back(Value::Float(value));
        }
    }
    if (canvas.display) {
        auto* display = canvas.display;
        canvas.state->result = clip.rx > 0 && clip.ry > 0 ? wa_canvas_clip_round_rect(display, clip)
                                                          : wa_canvas_clip_rect(display, clip.rect);
    }
    return HasArea(frame.clip);
}

void AddPathClip(const CanvasTarget& canvas, const PathGeometry& path, bool difference) {
    auto& frame = canvas.data->frame;
    const auto inverse = Inverse(frame.matrix);
    if (!inverse) return;
    PathClip clip{{}, (path.fill & 1) != 0};
    const auto values = RasterPathValues(path);
    for (const auto& contour : interp::software_raster::Flatten(values, path.verbs.size()))
        interp::software_raster::AppendEdges(clip.edges, contour.points, {}, false);
    frame.masks.push_back(
        std::make_shared<CanvasClip>(CanvasClip{*inverse, std::move(clip), difference}));
}

jboolean ClipCanvasRect(JNIEnv* env, jclass, jlong id, jfloat l, jfloat t, jfloat r, jfloat b, jint op) {
    return CallSafely(env, [&]() -> jboolean {
        const auto canvas = Canvas(env, id);
        if (!canvas) return false;
        auto rect = wa_rect_t{l, t, r, b};
        if (op == 0) {
            if (!HasArea(rect)) return HasArea(canvas.data->frame.clip);
            PathGeometry excluded;
            excluded.Rect(rect);
            AddPathClip(canvas, excluded, true);
            // Narrow the rectangular bounds when the exclusion leaves one strip.
            if (canvas.data->frame.matrix[1] || canvas.data->frame.matrix[3])
                return HasArea(canvas.data->frame.clip);
            const auto inverse = Inverse(canvas.data->frame.matrix);
            if (!inverse) return false;
            const auto old = MapRect(*inverse, canvas.data->frame.clip);
            if (!HasArea(Intersect(old, rect))) return HasArea(old);
            if (l <= old.l && r >= old.r) {
                if (t <= old.t) rect = {old.l, std::max(b, old.t), old.r, old.b};
                else if (b >= old.b) rect = {old.l, old.t, old.r, std::min(t, old.b)};
                else return true;
            } else if (t <= old.t && b >= old.b) {
                if (l <= old.l) rect = {std::max(r, old.l), old.t, old.r, old.b};
                else if (r >= old.r) rect = {old.l, old.t, std::min(l, old.r), old.b};
                else return true;
            } else {
                return true;
            }
        }
        if (!HasArea(rect)) rect = {};
        return ClipCanvas(canvas, {rect, 0, 0});
    });
}
jboolean ClipCanvasRound(JNIEnv* env, jclass, jlong id, jfloat l, jfloat t, jfloat r, jfloat b,
                         jfloat rx, jfloat ry) {
    return CallSafely(env, [&]() -> jboolean {
        const auto canvas = Canvas(env, id);
        return canvas && ClipCanvas(canvas, {{l, t, r, b}, std::max(0.f, rx), std::max(0.f, ry)});
    });
}
jboolean ClipCanvasPath(JNIEnv* env, jclass, jlong id, jlong path, jint op) {
    return CallSafely(env, [&]() -> jboolean {
        const auto canvas = Canvas(env, id);
        const auto* geometry = Path(env, path);
        if (!canvas || !geometry) return false;
        if (geometry->fill >= 2) op = 1 - op;
        if (geometry->Empty() && op == 0) return HasArea(canvas.data->frame.clip);
        if (op == 0 || !geometry->clip) AddPathClip(canvas, *geometry, op == 0);
        if (op == 0) return HasArea(canvas.data->frame.clip);
        if (geometry->clip) return ClipCanvas(canvas, *geometry->clip);
        return ClipCanvas(canvas, {geometry->Bounds(), 0, 0});
    });
}

void ClipCanvasShader(JNIEnv* env, jclass, jlong id, jlong shader, jint op) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        auto source = GetShader(env, shader);
        if (!canvas || !source) return;
        auto& frame = canvas.data->frame;
        if (auto inverse = Inverse(frame.matrix))
            frame.masks.push_back(std::make_shared<CanvasClip>(
                CanvasClip{*inverse, std::move(source), op == 0}));
    });
}
jboolean GetClipBounds(JNIEnv* env, jclass, jlong id, jobject out) {
    const auto canvas = Canvas(env, id);
    if (!canvas) return false;
    const auto& frame = canvas.data->frame;
    const auto inverse = Inverse(frame.matrix);
    const bool area = HasArea(frame.clip) && inverse.has_value();
    if (out) SetRect(env, out, area ? MapRect(*inverse, frame.clip) : wa_rect_t{});
    return area;
}
jboolean QuickRejectRect(JNIEnv* env, jclass, jlong id, jfloat l, jfloat t, jfloat r, jfloat b) {
    const auto canvas = Canvas(env, id);
    return !canvas || !HasArea(Intersect(canvas.data->frame.clip,
                                        MapRect(canvas.data->frame.matrix, {l, t, r, b})));
}
jboolean QuickRejectPath(JNIEnv* env, jclass cls, jlong id, jlong handle) {
    const auto* path = Path(env, handle);
    if (!path || path->fill >= 2) return false;
    const auto [l, t, r, b] = path->Bounds();
    return QuickRejectRect(env, cls, id, l, t, r, b);
}
void MultiplyCanvasAlpha(JNIEnv* env, jclass, jlong id, jfloat alpha) {
    if (const auto canvas = Canvas(env, id))
        canvas.data->frame.alpha *= std::clamp(alpha, 0.f, 1.f);
}

jint SaveLayerAlpha(JNIEnv* env, jclass cls, jlong id, jfloat l, jfloat t, jfloat r, jfloat b, jint alpha) {
    const jint count = SaveCanvas(env, cls, id, 3);
    if (env->ExceptionCheck()) return count;
    if (std::isfinite(l) && std::isfinite(t) && std::isfinite(r) && std::isfinite(b) && l < r && t < b)
        ClipCanvasRect(env, cls, id, l, t, r, b, 1);
    MultiplyCanvasAlpha(env, cls, id, std::clamp(alpha, 0, 255) / 255.f);
    return count;
}
jint SaveLayer(JNIEnv* env, jclass cls, jlong id, jfloat l, jfloat t, jfloat r, jfloat b, jlong paint) {
    const auto* p = paint ? Paint(env, paint) : nullptr;
    if (env->ExceptionCheck()) return 0;
    return SaveLayerAlpha(env, cls, id, l, t, r, b, p ? p->color >> 24 : 255);
}

uint32_t Opacity(uint32_t color, float opacity) {
    return (color & 0xffffffu) | static_cast<uint32_t>(std::lround((color >> 24) * opacity)) << 24;
}
wa_paint_t DisplayPaint(const CanvasTarget& canvas, const PaintData& p, uint32_t style = WA_PAINT_FILL) {
    return {Opacity(FilterColor(p, p.color), canvas.data->frame.alpha), style, p.stroke_width};
}
template <class Emit>
void DisplayShape(const CanvasTarget& canvas, const PaintData& paint, Emit&& emit) {
    auto& status = canvas.state->result;
    status = emit(DisplayPaint(canvas, paint, paint.style == 1 ? WA_PAINT_STROKE : WA_PAINT_FILL));
    if (status == WA_SF_OK && paint.style == 2) status = emit(DisplayPaint(canvas, paint, WA_PAINT_STROKE));
}

Object RasterTransform(const CanvasData::Frame& frame) {
    Object transform;
    transform.elements.reserve(10 + frame.raster_clips.size());
    for (size_t i = 0; i < 6; ++i) transform.elements.push_back(Value::Float(frame.matrix[i]));
    const auto [l, t, r, b] = frame.clip;
    for (float x : {l, t, r, b}) transform.elements.push_back(Value::Float(x));
    transform.elements.insert(transform.elements.end(), frame.raster_clips.begin(), frame.raster_clips.end());
    return transform;
}
const GradientData* PaintGradient(const PaintData& paint) {
    return paint.shader ? std::get_if<GradientData>(&paint.shader->content) : nullptr;
}

#include "shader_raster.inc"

void EmitPath(const CanvasTarget& canvas, const PathGeometry& path, const PaintData& paint) {
    if (!HasArea(canvas.data->frame.clip)) return;
    if (canvas.bitmap || paint.shader || !canvas.data->frame.masks.empty()) {
        RasterizePath(canvas, path, paint);
    } else if (canvas.display && !path.Empty()) {
        auto verbs = std::span<const wa_path_verb_t>(path.verbs);
        PathGeometry inverse;
        if (path.fill >= 2) {
            if (const auto m = Inverse(canvas.data->frame.matrix)) {
                inverse.Rect(MapRect(*m, canvas.data->frame.clip));
                inverse.verbs.insert(inverse.verbs.end(), path.verbs.begin(), path.verbs.end());
                verbs = inverse.verbs;
            }
        }
        DisplayShape(canvas, paint, [&](const auto& style) {
            return wa_canvas_draw_path(canvas.display, verbs.data(), verbs.size(),
                                       path.fill >= 2 ? WA_PATH_EVEN_ODD : path.fill, style);
        });
    }
}

void MaskCanvasBitmap(JNIEnv* env, jclass, jlong bitmap_id, jlong path_id) {
    CallSafely(env, [&] {
        auto* bitmap = Bitmap(env, bitmap_id);
        const auto* path = Path(env, path_id);
        if (!bitmap || !path || bitmap->immutable || bitmap->recycled) return;
        const auto transform = RasterTransform(CanvasData::Frame{});
        const auto coverage = RasterizeCoverage(*path, PaintData{}, transform.elements,
                                                {0, 0, bitmap->width, bitmap->height});
        for (size_t i = 0; i < coverage.size(); ++i)
            bitmap->pixels[i] = Opacity(bitmap->pixels[i], coverage[i] / 255.f);
        bitmap->Changed();
    });
}

void DrawCanvasColor(JNIEnv* env, jclass, jlong id, jint color, jint mode) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        if (!canvas || !HasArea(canvas.data->frame.clip)) return;
        if (!canvas.data->frame.masks.empty()) {
            PaintData paint;
            paint.color = color;
            paint.blend = mode;
            PathGeometry coverage;
            coverage.fill = 2;
            EmitPath(canvas, coverage, paint);
            return;
        }
        const auto value = Opacity(color, canvas.data->frame.alpha);
        if (canvas.bitmap) {
            const auto& frame = canvas.data->frame;
            auto transform = RasterTransform(frame);
            const auto [l, t, r, b] = frame.clip;
            const auto pixel = [](float edge, jint limit) {
                return static_cast<jint>(std::clamp(std::ceil(edge - .5f), 0.f, float(limit)));
            };
            const jint right = pixel(r, canvas.bitmap->width), bottom = pixel(b, canvas.bitmap->height);
            const jint left = pixel(l, canvas.bitmap->width), top = pixel(t, canvas.bitmap->height);
            for (jint y = top; y < bottom; ++y) {
                for (jint x = left; x < right; ++x) {
                    if (!interp::software_raster::InsideClips(transform.elements, x + 0.5, y + 0.5)) continue;
                    auto& pixel = canvas.bitmap->pixels[y * canvas.bitmap->width + x];
                    const auto blended = BlendColor(value, pixel, mode);
                    pixel = canvas.bitmap->PutColor(blended);
                }
            }
            canvas.bitmap->Changed();
        } else if (canvas.display) {
            canvas.state->result = wa_canvas_draw_color(canvas.display, value);
        }
    });
}
void DrawCanvasColorLong(JNIEnv* env, jclass cls, jlong id, jlong, jlong color, jint mode) {
    DrawCanvasColor(env, cls, id, UnpackColor(color), mode);
}
void DrawCanvasPaint(JNIEnv* env, jclass cls, jlong id, jlong paint) {
    CallSafely(env, [&] {
        const auto* p = Paint(env, paint);
        const auto canvas = Canvas(env, id);
        if (!canvas || !p) return;
        if (p->shader || !canvas.data->frame.masks.empty()) {
            PathGeometry coverage;
            coverage.fill = 2;
            auto fill = *p;
            fill.style = 0;
            EmitPath(canvas, coverage, fill);
        } else {
            DrawCanvasColor(env, cls, id, FilterColor(*p, p->color), p->blend);
        }
    });
}

bool DrawGradientShape(const CanvasTarget& canvas, wa_round_rect_t shape, const PaintData& paint) {
    const auto* gradient = PaintGradient(paint);
    if (!gradient || gradient->type != 1 || paint.style == 1 || !canvas.data->frame.masks.empty()) return false;
    const auto [x, y, end_x, end_y] = gradient->geometry;
    const float angle = std::atan2(end_y - y, end_x - x) * (180.f / std::numbers::pi_v<float>);
    const float alpha = canvas.data->frame.alpha * (paint.color >> 24) / 255.f;
    const wa_canvas_gradient_t draw{shape, Opacity(gradient->colors.front(), alpha),
                                   Opacity(gradient->colors.back(), alpha), angle};
    canvas.state->result = wa_canvas_draw_linear_gradient(canvas.display, &draw);
    return true;
}

void DrawCanvasRect(JNIEnv* env, jclass, jlong id, jfloat l, jfloat t, jfloat r, jfloat b, jlong paint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* p = Paint(env, paint);
        const auto rect = wa_rect_t{l, t, r, b};
        if (!canvas || !p || !HasArea(rect)) return;
        if (canvas.display && DrawGradientShape(canvas, {rect, 0, 0}, *p)) return;
        if (canvas.bitmap || p->shader || !canvas.data->frame.masks.empty()) {
            PathGeometry path;
            path.Rect(rect);
            EmitPath(canvas, path, *p);
        } else if (canvas.display) {
            DisplayShape(canvas, *p, [&](const auto& style) { return wa_canvas_draw_rect(canvas.display, rect, style); });
        }
    });
}
void DrawCanvasRoundRect(JNIEnv* env, jclass, jlong id, jfloat l, jfloat t, jfloat r, jfloat b,
                         jfloat rx, jfloat ry, jlong paint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* p = Paint(env, paint);
        const auto rect = wa_rect_t{l, t, r, b};
        if (!canvas || !p || !HasArea(rect)) return;
        const auto round = wa_round_rect_t{rect, std::max(0.f, rx), std::max(0.f, ry)};
        if (canvas.display && DrawGradientShape(canvas, round, *p)) return;
        if (canvas.bitmap || p->shader || !canvas.data->frame.masks.empty()) {
            PathGeometry path;
            path.RoundRect(rect, {rx, ry, rx, ry, rx, ry, rx, ry});
            EmitPath(canvas, path, *p);
        } else if (canvas.display) {
            DisplayShape(canvas, *p, [&](const auto& style) {
                return wa_canvas_draw_round_rect(canvas.display, round, style);
            });
        }
    });
}
void DrawCanvasOval(JNIEnv* env, jclass, jlong id, jfloat l, jfloat t, jfloat r, jfloat b, jlong paint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* p = Paint(env, paint);
        if (!canvas || !p) return;
        PathGeometry path;
        path.Oval({l, t, r, b});
        EmitPath(canvas, path, *p);
    });
}
void DrawCanvasCircle(JNIEnv* env, jclass, jlong id, jfloat x, jfloat y, jfloat radius, jlong paint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* p = Paint(env, paint);
        if (!canvas || !p || !(radius > 0)) return;
        if (canvas.bitmap || p->shader || !canvas.data->frame.masks.empty()) {
            PathGeometry path;
            path.Oval({x - radius, y - radius, x + radius, y + radius});
            EmitPath(canvas, path, *p);
        } else if (canvas.display) {
            DisplayShape(canvas, *p, [&](const auto& style) {
                return wa_canvas_draw_circle(canvas.display, x, y, radius, style);
            });
        }
    });
}
void DrawCanvasArc(JNIEnv* env, jclass, jlong id, jfloat l, jfloat t, jfloat r, jfloat b, jfloat from,
                   jfloat sweep, jboolean center, jlong paint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* p = Paint(env, paint);
        if (!canvas || !p || sweep == 0 || !HasArea({l, t, r, b})) return;
        PathGeometry path;
        if (std::abs(sweep) >= 360) path.Oval({l, t, r, b}, sweep < 0);
        else {
            if (center) path.Move((l + r) / 2, (t + b) / 2);
            path.Arc({l, t, r, b}, from, sweep, !center);
            if (center) path.Close();
        }
        EmitPath(canvas, path, *p);
    });
}
void DrawCanvasPath(JNIEnv* env, jclass, jlong id, jlong path, jlong paint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* g = Path(env, path);
        const auto* p = Paint(env, paint);
        if (canvas && g && p) EmitPath(canvas, *g, *p);
    });
}
void EmitLine(JNIEnv* env, const CanvasTarget& canvas, const PaintData& paint, float x0, float y0,
              float x1, float y1) {
    if (canvas.bitmap || paint.shader || !canvas.data->frame.masks.empty()) {
        auto p = paint;
        p.style = 1;
        PathGeometry path;
        path.Move(x0, y0);
        path.Line(x1, y1);
        EmitPath(canvas, path, p);
    } else if (canvas.display) {
        canvas.state->result = wa_canvas_draw_line(canvas.display, x0, y0, x1, y1,
                                                   DisplayPaint(canvas, paint, WA_PAINT_STROKE));
    }
}
void DrawCanvasLine(JNIEnv* env, jclass, jlong id, jfloat x0, jfloat y0, jfloat x1, jfloat y1,
                    jlong paint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* p = Paint(env, paint);
        if (canvas && p) EmitLine(env, canvas, *p, x0, y0, x1, y1);
    });
}
void EmitPoint(JNIEnv* env, const CanvasTarget& canvas, const PaintData& paint, float x, float y) {
    const auto radius = std::max(1.f, paint.stroke_width) / 2;
    if (canvas.display && !paint.shader && canvas.data->frame.masks.empty()) {
        const auto style = DisplayPaint(canvas, paint);
        auto* display = canvas.display;
        canvas.state->result = paint.cap == 1 ? wa_canvas_draw_circle(display, x, y, radius, style)
            : wa_canvas_draw_rect(display, {x - radius, y - radius, x + radius, y + radius}, style);
    } else if (canvas.bitmap || canvas.display) {
        PathGeometry point;
        const auto rect = wa_rect_t{x - radius, y - radius, x + radius, y + radius};
        if (paint.cap == 1) point.Oval(rect);
        else point.Rect(rect);
        auto p = paint;
        p.style = 0;
        EmitPath(canvas, point, p);
    }
}
void DrawCanvasPoint(JNIEnv* env, jclass, jlong id, jfloat x, jfloat y, jlong paint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* p = Paint(env, paint);
        if (canvas && p) EmitPoint(env, canvas, *p, x, y);
    });
}
template <bool Lines>
void DrawCanvasPoints(JNIEnv* env, jclass, jlong id, jfloatArray xy, jint offset, jint count, jlong paint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* p = Paint(env, paint);
        if (!canvas || !p || count <= 0) return;
        constexpr jint step = Lines ? 4 : 2;
        const jint end = count - count % step;
        if (offset < 0 || end > env->GetArrayLength(xy) - offset) {
            Throw(env, "java/lang/ArrayIndexOutOfBoundsException", "Invalid Canvas point range");
            return;
        }
        if (end > WA_SF_LAYER_LIST_CAPACITY) {
            Throw(env, "java/lang/IllegalArgumentException", "Canvas point count exceeds display list capacity");
            return;
        }
        std::vector<float> coords(end);
        env->GetFloatArrayRegion(xy, offset, end, coords.data());
        if (env->ExceptionCheck()) return;
        for (jint i = 0; i < end && canvas.state->result == WA_SF_OK; i += step) {
            if constexpr (Lines) EmitLine(env, canvas, *p, coords[i], coords[i + 1], coords[i + 2], coords[i + 3]);
            else EmitPoint(env, canvas, *p, coords[i], coords[i + 1]);
        }
    });
}

void EmitText(JNIEnv* env, const CanvasTarget& canvas, const PaintData& paint,
              std::u16string_view text, float x, float y) {
    const auto* p = &paint;
    if (text.empty() || !(p->text_size > 0) || env->ExceptionCheck()) return;
    if (!canvas.display) {
        if (canvas.bitmap) Throw(env, "java/lang/UnsupportedOperationException", "Text requires the browser glyph rasterizer");
        return;
    }
    const auto bytes = Utf8(text);
    if (bytes.size() > WA_SF_LAYER_LIST_CAPACITY) {
        canvas.state->result = WA_SF_FULL;
        return;
    }
    float scale = p->text_scale;
    if (p->letter_spacing != 0 || p->word_spacing != 0) {
        const auto raw = wa_canvas_measure_text(canvas.state->sf, bytes.data(), bytes.size(),
                                                p->text_size, p->Weight(), p->Face()).advance;
        if (raw > 0) scale = Measure(*canvas.state, *p, text).advance / raw;
    }
    auto& status = canvas.state->result;
    const float skew = p->text_skew - (p->Italic() ? 0.25f : 0.f);
    const bool transform = scale != 1.f || skew != 0.f;
    if (transform) {
        status = wa_canvas_save(canvas.display);
        if (status == WA_SF_OK)
            EmitMatrix(canvas, AtPivot(Multiply(Scaling(scale, 1), Skew(skew, 0)), x, y));
    }
    const auto draw = [&](float left, float baseline, uint32_t color) {
        if (status != WA_SF_OK) return;
        const wa_canvas_text_t record{0, static_cast<uint32_t>(bytes.size()), left, baseline, 0,
            p->text_size, p->Weight(), static_cast<uint32_t>(p->align),
            Opacity(color, canvas.data->frame.alpha), p->Face()};
        status = wa_canvas_draw_text(canvas.display, &record, bytes.data());
    };
    if (p->shadow_radius > 0 && p->shadow_color >> 24) draw(x + p->shadow_dx, y + p->shadow_dy, p->shadow_color);
    draw(x, y, p->color);
    if (transform && status == WA_SF_OK) status = wa_canvas_restore(canvas.display);
}
void EmitText(JNIEnv* env, jlong id, jlong paint, std::u16string_view text, float x, float y) {
    const auto canvas = Canvas(env, id);
    const auto* p = Paint(env, paint);
    if (canvas && p) EmitText(env, canvas, *p, text, x, y);
}
template <typename Text>
void DrawCanvasText(JNIEnv* env, jclass, jlong id, Text text, jint start, jint end, jfloat x, jfloat y,
                    jint, jlong paint) {
    CallSafely(env, [&] {
        auto run = Chars(env, text, start, end);
        if (!env->ExceptionCheck()) EmitText(env, id, paint, run, x, y);
    });
}
void DrawCanvasTextRun(JNIEnv* env, jclass cls, jlong id, jstring text, jint start, jint end,
                       jint, jint, jfloat x, jfloat y, jboolean rtl, jlong paint) {
    DrawCanvasText<jstring>(env, cls, id, text, start, end, x, y, rtl, paint);
}
void DrawCanvasTextRunArray(JNIEnv* env, jclass cls, jlong id, jcharArray text, jint index, jint count,
                            jint, jint, jfloat x, jfloat y, jboolean rtl, jlong paint, jlong) {
    DrawCanvasText<jcharArray>(env, cls, id, text, index, count, x, y, rtl, paint);
}

void PaintBitmap(const CanvasTarget& canvas, const BitmapData& bitmap, jlong identity,
                 wa_rect_t src, wa_rect_t dst, const PaintData& paint, jint mask = WA_MASK_RECT, float radius = 0) {
    if (!HasArea(src) || !HasArea(dst) || bitmap.recycled) return;
    const float alpha = canvas.data->frame.alpha * (paint.color >> 24) / 255.f;
    if (canvas.bitmap) {
        RasterizeBitmap(canvas, bitmap, src, dst, paint, mask, radius);
    } else if (canvas.display && canvas.data->frame.masks.empty()) {
        const auto handle = PublishBitmapPixels(*canvas.state, bitmap.pixels, bitmap.width, bitmap.height,
                                                identity, bitmap.generation, bitmap.has_alpha,
                                                bitmap.config == 1 ? paint.color & 0xffffffu : 0);
        if (!handle) return;
        if (!std::ranges::contains(canvas.state->recording_images, handle))
            canvas.state->recording_images.push_back(handle);
        const wa_canvas_bitmap_t draw{handle, static_cast<uint32_t>(mask), src, dst, radius, alpha};
        canvas.state->result = wa_canvas_draw_bitmap(canvas.display, &draw);
    } else if (canvas.display) {
        PathGeometry path;
        if (mask == WA_MASK_ROUNDED_RECT)
            path.RoundRect(dst, {radius, radius, radius, radius, radius, radius, radius, radius});
        else if (mask >= WA_MASK_CIRCLE) path.Oval(dst);
        else path.Rect(dst);
        auto copy = paint;
        copy.style = 0;
        RasterizePaintedPath(canvas, path, copy, [&](float x, float y) {
            const auto sx = std::lerp(src.l, src.r, (x - dst.l) / (dst.r - dst.l));
            const auto sy = std::lerp(src.t, src.b, (y - dst.t) / (dst.b - dst.t));
            return Opacity(FilterColor(paint, SampleBitmap(bitmap, 0, 0, paint.flags & 2, sx, sy, paint.color)),
                           alpha);
        });
    }
}

const PaintData* BitmapPaint(JNIEnv* env, jlong paint) {
    static const PaintData fallback = [] {
        PaintData p;
        p.flags = 2;
        return p;
    }();
    return paint ? Paint(env, paint) : &fallback;
}

void SnapPatchDivisions(std::span<PatchSpan> spans, float scale, float offset) {
    // The compositor antialiases each quad. Shared edges on a pixel boundary avoid translucent seams.
    for (size_t i = 1; i < spans.size(); ++i) {
        auto& before = spans[i - 1];
        auto& after = spans[i];
        const float edge = (std::round(before.dst_to * scale + offset) - offset) / scale;
        before.dst_to = after.dst_from = std::clamp(edge, before.dst_from, after.dst_to);
    }
}

void DrawCanvasNinePatch(JNIEnv* env, jclass, jlong id, jlong bitmap, jlong chunk, jfloat left,
                         jfloat top, jfloat right, jfloat bottom, jlong paint, jint screen_density,
                         jint bitmap_density) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* image = Bitmap(env, bitmap);
        const auto* patch = Peer<NinePatchData, &State::nine_patches>(env, chunk);
        const auto* p = BitmapPaint(env, paint);
        if (!canvas || !image || !patch || !p || !(right > left && bottom > top)) return;
        const float density = screen_density > 0 && bitmap_density > 0
            ? float(screen_density) / bitmap_density : 1;
        auto x = PartitionPatch(patch->x_divisions, image->width, left, right, density);
        auto y = PartitionPatch(patch->y_divisions, image->height, top, bottom, density);
        const auto& matrix = canvas.data->frame.matrix;
        if (canvas.display && matrix[1] == 0 && matrix[3] == 0 && matrix[0] != 0 && matrix[4] != 0) {
            SnapPatchDivisions(x, matrix[0], matrix[2]);
            SnapPatchDivisions(y, matrix[4], matrix[5]);
        }
        for (const auto& row : y) {
            for (const auto& col : x) {
                if (canvas.state->result != WA_SF_OK) return;
                PaintBitmap(canvas, *image, bitmap, {col.from, row.from, col.to, row.to},
                            {col.dst_from, row.dst_from, col.dst_to, row.dst_to}, *p);
            }
        }
    });
}

void DrawCanvasBitmapRect(JNIEnv* env, jclass, jlong id, jlong bitmap, jfloat sl, jfloat st, jfloat sr,
                          jfloat sb, jfloat dl, jfloat dt, jfloat dr, jfloat db, jlong paint, jint, jint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* image = Bitmap(env, bitmap);
        const auto* p = BitmapPaint(env, paint);
        if (canvas && image && p) PaintBitmap(canvas, *image, bitmap, {sl, st, sr, sb}, {dl, dt, dr, db}, *p);
    });
}
void DrawCanvasBitmapAt(JNIEnv* env, jclass cls, jlong id, jlong bitmap, jfloat x, jfloat y, jlong paint,
                        jint density, jint, jint bitmap_density) {
    const auto* image = Bitmap(env, bitmap);
    if (!image) return;
    const float scale = density > 0 && bitmap_density > 0 ? float(density) / bitmap_density : 1.f;
    DrawCanvasBitmapRect(env, cls, id, bitmap, 0, 0, image->width, image->height,
                          x, y, x + image->width * scale, y + image->height * scale, paint, 0, 0);
}
void DrawCanvasBitmapMatrix(JNIEnv* env, jclass cls, jlong id, jlong bitmap, jlong matrix, jlong paint) {
    const jint saved = SaveCanvas(env, cls, id, 3);
    if (env->ExceptionCheck()) return;
    ConcatCanvasMatrix(env, cls, id, matrix);
    if (!env->ExceptionCheck()) DrawCanvasBitmapAt(env, cls, id, bitmap, 0, 0, paint, 0, 0, 0);
    RestoreCanvasToCount(env, cls, id, saved);
}
void DrawCanvasBitmapPixels(JNIEnv* env, jclass, jlong id, jintArray colors, jint offset, jint stride,
                             jfloat x, jfloat y, jint width, jint height, jboolean alpha, jlong paint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* p = BitmapPaint(env, paint);
        if (!canvas || !p || width <= 0 || height <= 0 || width > 4096 || height > 4096) return;
        BitmapData pixels;
        pixels.width = width;
        pixels.height = height;
        pixels.has_alpha = alpha;
        pixels.pixels.resize(static_cast<size_t>(width) * height);
        const auto length = env->GetArrayLength(colors);
        for (jint row = 0; row < height; ++row) {
            const int64_t start = offset + int64_t(row) * stride;
            if (start < 0 || start + width > length) {
                Throw(env, "java/lang/ArrayIndexOutOfBoundsException", "Canvas bitmap rows");
                return;
            }
            env->GetIntArrayRegion(colors, start, width, pixels.pixels.data() + row * width);
            if (env->ExceptionCheck()) return;
            if (!alpha)
                for (jint col = 0; col < width; ++col) pixels.pixels[row * width + col] |= 0xff000000;
        }
        PaintBitmap(canvas, pixels, 0, {0, 0, float(width), float(height)}, {x, y, x + width, y + height}, *p);
    });
}
void DrawCanvasMaskedBitmap(JNIEnv* env, jclass, jlong id, jlong bitmap, jfloat sl, jfloat st, jfloat sr,
                            jfloat sb, jfloat dl, jfloat dt, jfloat dr, jfloat db, jlong paint,
                            jint mask, jfloat radius) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* image = Bitmap(env, bitmap);
        const auto* p = BitmapPaint(env, paint);
        if (canvas && image && p) PaintBitmap(canvas, *image, bitmap,
            {sl, st, sr, sb}, {dl, dt, dr, db}, *p, mask, radius);
    });
}

void DrawCanvasGradient(JNIEnv* env, jclass, jlong id, jfloat l, jfloat t, jfloat r, jfloat b, jfloat radius,
                        jint first, jint last, jfloat degrees) {
    const auto canvas = Canvas(env, id);
    if (!canvas.display) return;
    const float alpha = canvas.data->frame.alpha;
    const wa_canvas_gradient_t draw{{{l, t, r, b}, radius, radius}, Opacity(first, alpha),
                                    Opacity(last, alpha), degrees};
    canvas.state->result = wa_canvas_draw_linear_gradient(canvas.display, &draw);
}
#include "canvas_draw_ops.inc"

void FreeCanvasCaches(JNIEnv* env, jclass) {
    if (auto* state = GetState(env)) state->CollectImages();
}
void FreeTextLayoutCaches(JNIEnv*, jclass) {
    // Native peers are collected with their owners. Atlas pages belong to the compositor.
}
void CanvasCompatibility(JNIEnv*, jclass, jint) {
    // Compatibility checks are handled by upstream Canvas before invoking JNI.
}

bool RegisterModernCanvas(interp::Vm& vm) {
    constexpr const char* name = "android/graphics/Canvas";
    if (!RegisterIfDeclared(vm, name, Method<InitRasterCanvas>(vm, "nInitRaster", "(J)J"))) return false;
    Register(vm, name,
             Method<RetainCanvas>(vm, "nRegisterNativeAllocation", "(Landroid/graphics/Canvas;J)Ljava/lang/Runnable;"),
             Method<ManagedFinalizer>(vm, "nGetNativeFinalizer", "()J"),
             Method<FreeCanvasCaches>(vm, "nFreeCaches", "()V"),
             Method<FreeTextLayoutCaches>(vm, "nFreeTextLayoutCaches", "()V"),
             Method<CanvasCompatibility>(vm, "nSetCompatibilityVersion", "(I)V"),
             Method<SetCanvasBitmap>(vm, "nSetBitmap", "(JJ)V"),
             Method<CanvasDimension<&CanvasData::width>>(vm, "nGetWidth", "(J)I"),
             Method<CanvasDimension<&CanvasData::height>>(vm, "nGetHeight", "(J)I"),
             Method<CanvasIsOpaque>(vm, "nIsOpaque", "(J)Z"),
             Method<CanvasHighContrast>(vm, "nIsHighContrastText", "(J)Z"),
             Method<SaveCanvas>(vm, "nSave", "(JI)I"),
             Method<SaveLayer>(vm, "nSaveLayer", "(JFFFFJ)I"),
             Method<SaveLayerAlpha>(vm, "nSaveLayerAlpha", "(JFFFFI)I"),
             Method<SaveUnclippedLayer>(vm, "nSaveUnclippedLayer", "(JIIII)I"),
             Method<RestoreUnclippedLayer>(vm, "nRestoreUnclippedLayer", "(JIJ)V"),
             Method<RestoreCanvas>(vm, "nRestore", "(J)Z"),
             Method<RestoreCanvasToCount>(vm, "nRestoreToCount", "(JI)V"),
             Method<CanvasSaveCount>(vm, "nGetSaveCount", "(J)I"),
             Method<TransformCanvas<Translation>>(vm, "nTranslate", "(JFF)V"),
             Method<TransformCanvas<Scaling>>(vm, "nScale", "(JFF)V"),
             Method<RotateCanvas>(vm, "nRotate", "(JF)V"),
             Method<TransformCanvas<Skew>>(vm, "nSkew", "(JFF)V"),
             Method<ConcatCanvasMatrix>(vm, "nConcat", "(JJ)V"),
             Method<ConcatCanvas44>(vm, "nConcat", "(J[F)V"),
             Method<SetCanvasMatrix>(vm, "nSetMatrix", "(JJ)V"),
             Method<GetCanvasMatrix>(vm, "nGetMatrix", "(JJ)V"),
             Method<ClipCanvasRect>(vm, "nClipRect", "(JFFFFI)Z"),
             Method<ClipCanvasPath>(vm, "nClipPath", "(JJI)Z"),
             Method<ClipCanvasShader>(vm, "nClipShader", "(JJI)V"),
             Method<SetCanvasDrawFilter>(vm, "nSetDrawFilter", "(JJ)V"),
             Method<GetClipBounds>(vm, "nGetClipBounds", "(JLandroid/graphics/Rect;)Z"),
             Method<QuickRejectRect>(vm, "nQuickReject", "(JFFFF)Z"),
             Method<QuickRejectPath>(vm, "nQuickReject", "(JJ)Z"));
    Register(vm, name,
             Method<SetRecordingSize>(vm, "nSetRecordingSize", "(JII)V"),
             Method<ReleaseShader>(vm, "nReleaseShader", "(J)V"),
             Method<MultiplyCanvasAlpha>(vm, "nMultiplyAlpha", "(JF)V"),
             Method<ClipCanvasRound>(vm, "nClipRoundRect", "(JFFFFFF)Z"),
             Method<DrawCanvasGradient>(vm, "nDrawLinearGradient", "(JFFFFFIIF)V"),
             Method<DrawCanvasMaskedBitmap>(vm, "nDrawMaskedBitmap", "(JJFFFFFFFFJIF)V"));
    RegisterIfDeclared(vm, name, Method<MaskCanvasBitmap>(vm, "nMaskBitmap", "(JJ)V"));
    Register(vm, "android/graphics/BaseCanvas",
             Method<DrawCanvasColor>(vm, "nDrawColor", "(JII)V"),
             Method<DrawCanvasNinePatch>(vm, "nDrawNinePatch", "(JJJFFFFJII)V"),
             Method<DrawCanvasColorLong>(vm, "nDrawColor", "(JJJI)V"),
             Method<DrawCanvasPaint>(vm, "nDrawPaint", "(JJ)V"),
             Method<DrawCanvasPoint>(vm, "nDrawPoint", "(JFFJ)V"),
             Method<DrawCanvasPoints<false>>(vm, "nDrawPoints", "(J[FIIJ)V"),
             Method<DrawCanvasLine>(vm, "nDrawLine", "(JFFFFJ)V"),
             Method<DrawCanvasPoints<true>>(vm, "nDrawLines", "(J[FIIJ)V"),
             Method<DrawCanvasRect>(vm, "nDrawRect", "(JFFFFJ)V"),
             Method<DrawCanvasRoundRect>(vm, "nDrawRoundRect", "(JFFFFFFJ)V"),
             Method<DrawDoubleRoundRect>(vm, "nDrawDoubleRoundRect", "(JFFFFFFFFFFFFJ)V"),
             Method<DrawDoubleRoundRectRadii>(vm, "nDrawDoubleRoundRect", "(JFFFF[FFFFF[FJ)V"),
             Method<DrawCanvasOval>(vm, "nDrawOval", "(JFFFFJ)V"),
             Method<DrawCanvasCircle>(vm, "nDrawCircle", "(JFFFJ)V"),
             Method<DrawCanvasArc>(vm, "nDrawArc", "(JFFFFFFZJ)V"),
             Method<DrawCanvasPath>(vm, "nDrawPath", "(JJJ)V"),
             Method<DrawCanvasRegion>(vm, "nDrawRegion", "(JJJ)V"),
             Method<DrawCanvasText<jstring>>(vm, "nDrawText", "(JLjava/lang/String;IIFFIJ)V"),
             Method<DrawCanvasText<jcharArray>>(vm, "nDrawText", "(J[CIIFFIJ)V"),
             Method<DrawCanvasTextRun>(vm, "nDrawTextRun", "(JLjava/lang/String;IIIIFFZJ)V"),
             Method<DrawCanvasTextRunArray>(vm, "nDrawTextRun", "(J[CIIIIFFZJJ)V"),
             Method<DrawTextOnPathString>(vm, "nDrawTextOnPath", "(JLjava/lang/String;JFFIJ)V"),
             Method<DrawTextOnPathArray>(vm, "nDrawTextOnPath", "(J[CIIJFFIJ)V"),
             Method<DrawCanvasGlyphs>(vm, "nDrawGlyphs", "(J[I[FIIIJJ)V"),
             Method<DrawCanvasBitmapAt>(vm, "nDrawBitmap", "(JJFFJIII)V"),
             Method<DrawCanvasBitmapRect>(vm, "nDrawBitmap", "(JJFFFFFFFFJII)V"),
             Method<DrawCanvasBitmapPixels>(vm, "nDrawBitmap", "(J[IIIFFIIZJ)V"),
             Method<DrawCanvasBitmapMatrix>(vm, "nDrawBitmapMatrix", "(JJJJ)V"),
             Method<DrawBitmapMesh>(vm, "nDrawBitmapMesh", "(JJII[FI[IIJ)V"),
             Method<DrawCanvasVertices>(vm, "nDrawVertices", "(JII[FI[FI[II[SIIJ)V"),
             Method<DrawCanvasMesh>(vm, "nDrawMesh", "(JJIJ)V"),
             Method<PunchCanvasHole>(vm, "nPunchHole", "(JFFFFFFF)V"));
    return true;
}

} // namespace
