struct CanvasData {
    struct Frame {
        Matrix9 matrix = kIdentity;
        wa_rect_t clip{};
        float alpha = 1;
        std::vector<interp::Value> raster_clips;
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
    if (canvas.bitmap && HasArea(frame.clip)) {
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

jboolean ClipCanvasRect(JNIEnv* env, jclass, jlong id, jfloat l, jfloat t, jfloat r, jfloat b, jint op) {
    return CallSafely(env, [&]() -> jboolean {
        const auto canvas = Canvas(env, id);
        if (!canvas) return false;
        auto rect = wa_rect_t{l, t, r, b};
        if (op == 0) {
            // Exact for exclusions that leave one rectangular strip.
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
jboolean ClipCanvasPath(JNIEnv* env, jclass cls, jlong id, jlong path, jint op) {
    return CallSafely(env, [&]() -> jboolean {
        const auto canvas = Canvas(env, id);
        const auto* geometry = Path(env, path);
        if (!canvas || !geometry) return false;
        if (geometry->fill >= 2) op = 1 - op;
        const auto [l, t, r, b] = geometry->Bounds();
        if (op == 0) return ClipCanvasRect(env, cls, id, l, t, r, b, op);
        if (geometry->clip) return ClipCanvas(canvas, *geometry->clip);
        return ClipCanvas(canvas, {{l, t, r, b}, 0, 0});
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
Object RasterPixels(const BitmapData& bitmap) {
    Object pixels;
    pixels.elements.reserve(bitmap.pixels.size());
    for (jint color : bitmap.pixels) pixels.elements.push_back(Value::Int(color));
    return pixels;
}
void StoreRasterPixels(BitmapData& bitmap, const Object& pixels) {
    for (size_t i = 0; i < bitmap.pixels.size(); ++i)
        bitmap.pixels[i] = bitmap.PutColor(pixels.elements[i].i());
    bitmap.Changed();
}

const GradientData* PaintGradient(JNIEnv* env, const CanvasTarget& canvas, const PaintData& paint) {
    if (!paint.shader || !canvas.state->gradients) return nullptr;
    return canvas.state->gradients->Get(env, paint.shader);
}

void SoftwarePath(JNIEnv* env, const CanvasTarget& canvas, const PathGeometry& path, const PaintData& paint) {
    auto dst = RasterPixels(*canvas.bitmap);
    auto transform = RasterTransform(canvas.data->frame);
    Object verbs;
    verbs.elements.reserve(path.verbs.size() * 7);
    for (const auto& verb : path.verbs) {
        verbs.elements.push_back(Value::Float(verb.command));
        for (float coordinate : verb.points) verbs.elements.push_back(Value::Float(coordinate));
    }
    const auto color = DisplayPaint(canvas, paint).argb;
    Object geometry, stops, colors;
    const auto* gradient = PaintGradient(env, canvas, paint);
    if (env->ExceptionCheck()) return;
    if (gradient) {
        for (auto coordinate : gradient->geometry) geometry.elements.push_back(Value::Float(coordinate));
        for (auto stop : gradient->positions) stops.elements.push_back(Value::Float(stop));
        for (auto color : gradient->colors) colors.elements.push_back(Value::Int(FilterColor(paint, color)));
    }
    const uint64_t mask = paint.mask_filter;
    const float blur = mask ? std::bit_cast<float>(uint32_t(mask)) : 0.f;
    const jint blur_style = mask ? (mask >> 32) - 1 : 0;
    const Value args[] = {
        Value::Ref(&dst), Value::Int(canvas.bitmap->width), Value::Int(canvas.bitmap->height),
        Value::Ref(&verbs), Value::Int(path.verbs.size()), Value::Int(path.fill), Value::Ref(&transform),
        Value::Int(color), Value::Float(blur), Value::Int(blur_style), Value::Int(canvas.bitmap->config == 1),
        Value::Int(gradient ? gradient->type : 0), Value::Int(gradient ? gradient->tile : 0),
        Value::Ref(&geometry), Value::Ref(&stops), Value::Ref(&colors), Value::Int(paint.style), Value::Float(paint.stroke_width),
        Value::Int(paint.cap), Value::Int(paint.join), Value::Float(paint.miter)};
    interp::software_raster::DrawPath(interp::JniBridge::From(env).Vm(), nullptr, args);
    StoreRasterPixels(*canvas.bitmap, dst);
}

void EmitPath(JNIEnv* env, const CanvasTarget& canvas, const PathGeometry& path, const PaintData& paint) {
    if (!HasArea(canvas.data->frame.clip)) return;
    if (canvas.bitmap) {
        SoftwarePath(env, canvas, path, paint);
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

void DrawCanvasColor(JNIEnv* env, jclass, jlong id, jint color, jint mode) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        if (!canvas || !HasArea(canvas.data->frame.clip)) return;
        const auto value = Opacity(color, canvas.data->frame.alpha);
        if (canvas.bitmap) {
            const auto& frame = canvas.data->frame;
            auto transform = RasterTransform(frame);
            const auto [l, t, r, b] = frame.clip;
            for (jint y = std::max(0.f, std::floor(t)); y < b && y < canvas.bitmap->height; ++y) {
                for (jint x = std::max(0.f, std::floor(l)); x < r && x < canvas.bitmap->width; ++x) {
                    if (!interp::software_raster::InsideClips(transform.elements, x + 0.5, y + 0.5)) continue;
                    auto& pixel = canvas.bitmap->pixels[y * canvas.bitmap->width + x];
                    const auto blended = mode == 0 ? 0u : mode == 1 ? value
                        : interp::software_raster::SourceOver(value, pixel);
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
        if (canvas.bitmap && p->shader) {
            PathGeometry coverage;
            coverage.fill = 2;
            auto fill = *p;
            fill.style = 0;
            EmitPath(env, canvas, coverage, fill);
        } else {
            DrawCanvasColor(env, cls, id, FilterColor(*p, p->color), p->blend);
        }
    });
}

bool DrawGradientShape(JNIEnv* env, const CanvasTarget& canvas, wa_round_rect_t shape, const PaintData& paint) {
    const auto* gradient = PaintGradient(env, canvas, paint);
    if (!gradient || gradient->type != 1 || paint.style == 1) return false;
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
        if (canvas.bitmap) {
            PathGeometry path;
            path.Rect(rect);
            EmitPath(env, canvas, path, *p);
        } else if (canvas.display && !DrawGradientShape(env, canvas, {rect, 0, 0}, *p)) {
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
        if (canvas.bitmap) {
            PathGeometry path;
            path.RoundRect(rect, {rx, ry, rx, ry, rx, ry, rx, ry});
            EmitPath(env, canvas, path, *p);
        } else if (canvas.display) {
            const auto round = wa_round_rect_t{rect, std::max(0.f, rx), std::max(0.f, ry)};
            if (DrawGradientShape(env, canvas, round, *p)) return;
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
        EmitPath(env, canvas, path, *p);
    });
}
void DrawCanvasCircle(JNIEnv* env, jclass, jlong id, jfloat x, jfloat y, jfloat radius, jlong paint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* p = Paint(env, paint);
        if (!canvas || !p || !(radius > 0)) return;
        if (canvas.bitmap) {
            PathGeometry path;
            path.Oval({x - radius, y - radius, x + radius, y + radius});
            EmitPath(env, canvas, path, *p);
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
        EmitPath(env, canvas, path, *p);
    });
}
void DrawCanvasPath(JNIEnv* env, jclass, jlong id, jlong path, jlong paint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* g = Path(env, path);
        const auto* p = Paint(env, paint);
        if (canvas && g && p) EmitPath(env, canvas, *g, *p);
    });
}
void EmitLine(JNIEnv* env, const CanvasTarget& canvas, const PaintData& paint, float x0, float y0,
              float x1, float y1) {
    if (canvas.bitmap) {
        auto p = paint;
        p.style = 1;
        PathGeometry path;
        path.Move(x0, y0);
        path.Line(x1, y1);
        EmitPath(env, canvas, path, p);
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
    if (canvas.display) {
        const auto style = DisplayPaint(canvas, paint);
        auto* display = canvas.display;
        canvas.state->result = paint.cap == 1 ? wa_canvas_draw_circle(display, x, y, radius, style)
            : wa_canvas_draw_rect(display, {x - radius, y - radius, x + radius, y + radius}, style);
    } else if (canvas.bitmap) {
        PathGeometry point;
        const auto rect = wa_rect_t{x - radius, y - radius, x + radius, y + radius};
        if (paint.cap == 1) point.Oval(rect);
        else point.Rect(rect);
        auto p = paint;
        p.style = 0;
        EmitPath(env, canvas, point, p);
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

void EmitText(JNIEnv* env, jlong id, jlong paint, std::u16string_view text, float x, float y) {
    const auto canvas = Canvas(env, id);
    const auto* p = Paint(env, paint);
    if (!canvas || !p || text.empty() || !(p->text_size > 0) || env->ExceptionCheck()) return;
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

void PaintBitmap(JNIEnv* env, const CanvasTarget& canvas, const BitmapData& bitmap, jlong identity,
                 wa_rect_t src, wa_rect_t dst, const PaintData& paint, jint mask = WA_MASK_RECT, float radius = 0) {
    if (!HasArea(src) || !HasArea(dst) || bitmap.recycled) return;
    const float alpha = canvas.data->frame.alpha * (paint.color >> 24) / 255.f;
    if (canvas.bitmap) {
        auto output = RasterPixels(*canvas.bitmap);
        auto source = RasterPixels(bitmap);
        auto transform = RasterTransform(canvas.data->frame);
        Object rectangles;
        rectangles.elements = {Value::Float(src.l), Value::Float(src.t), Value::Float(src.r), Value::Float(src.b),
                               Value::Float(dst.l), Value::Float(dst.t), Value::Float(dst.r), Value::Float(dst.b)};
        const Value args[] = {
            Value::Ref(&output), Value::Int(canvas.bitmap->width), Value::Int(canvas.bitmap->height),
            Value::Ref(&source), Value::Int(bitmap.width), Value::Int(bitmap.height),
            Value::Ref(&rectangles), Value::Ref(&transform), Value::Float(alpha),
            Value::Int(paint.color), Value::Int(bitmap.has_alpha), Value::Int(bitmap.config == 1),
            Value::Int(canvas.bitmap->config == 1), Value::Int((paint.flags & 2) != 0),
            Value::Int(mask), Value::Float(radius)};
        interp::software_raster::DrawBitmap(interp::JniBridge::From(env).Vm(), nullptr, args);
        StoreRasterPixels(*canvas.bitmap, output);
    } else if (canvas.display) {
        const auto handle = PublishBitmapPixels(*canvas.state, bitmap.pixels, bitmap.width, bitmap.height,
                                                identity, bitmap.generation, bitmap.has_alpha,
                                                bitmap.config == 1 ? paint.color & 0xffffffu : 0);
        if (!handle) return;
        if (!std::ranges::contains(canvas.state->recording_images, handle))
            canvas.state->recording_images.push_back(handle);
        const wa_canvas_bitmap_t draw{handle, static_cast<uint32_t>(mask), src, dst, radius, alpha};
        canvas.state->result = wa_canvas_draw_bitmap(canvas.display, &draw);
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

void DrawCanvasBitmapRect(JNIEnv* env, jclass, jlong id, jlong bitmap, jfloat sl, jfloat st, jfloat sr,
                          jfloat sb, jfloat dl, jfloat dt, jfloat dr, jfloat db, jlong paint, jint, jint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* image = Bitmap(env, bitmap);
        const auto* p = BitmapPaint(env, paint);
        if (canvas && image && p) PaintBitmap(env, canvas, *image, bitmap, {sl, st, sr, sb}, {dl, dt, dr, db}, *p);
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
        PaintBitmap(env, canvas, pixels, 0, {0, 0, float(width), float(height)}, {x, y, x + width, y + height}, *p);
    });
}
void DrawCanvasMaskedBitmap(JNIEnv* env, jclass, jlong id, jlong bitmap, jfloat sl, jfloat st, jfloat sr,
                            jfloat sb, jfloat dl, jfloat dt, jfloat dr, jfloat db, jlong paint,
                            jint mask, jfloat radius) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* image = Bitmap(env, bitmap);
        const auto* p = BitmapPaint(env, paint);
        if (canvas && image && p) PaintBitmap(env, canvas, *image, bitmap,
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
    Register(vm, "android/graphics/BaseCanvas",
             Method<DrawCanvasColor>(vm, "nDrawColor", "(JII)V"),
             Method<DrawCanvasColorLong>(vm, "nDrawColor", "(JJJI)V"),
             Method<DrawCanvasPaint>(vm, "nDrawPaint", "(JJ)V"),
             Method<DrawCanvasPoint>(vm, "nDrawPoint", "(JFFJ)V"),
             Method<DrawCanvasPoints<false>>(vm, "nDrawPoints", "(J[FIIJ)V"),
             Method<DrawCanvasLine>(vm, "nDrawLine", "(JFFFFJ)V"),
             Method<DrawCanvasPoints<true>>(vm, "nDrawLines", "(J[FIIJ)V"),
             Method<DrawCanvasRect>(vm, "nDrawRect", "(JFFFFJ)V"),
             Method<DrawCanvasRoundRect>(vm, "nDrawRoundRect", "(JFFFFFFJ)V"),
             Method<DrawCanvasOval>(vm, "nDrawOval", "(JFFFFJ)V"),
             Method<DrawCanvasCircle>(vm, "nDrawCircle", "(JFFFJ)V"),
             Method<DrawCanvasArc>(vm, "nDrawArc", "(JFFFFFFZJ)V"),
             Method<DrawCanvasPath>(vm, "nDrawPath", "(JJJ)V"),
             Method<DrawCanvasText<jstring>>(vm, "nDrawText", "(JLjava/lang/String;IIFFIJ)V"),
             Method<DrawCanvasText<jcharArray>>(vm, "nDrawText", "(J[CIIFFIJ)V"),
             Method<DrawCanvasTextRun>(vm, "nDrawTextRun", "(JLjava/lang/String;IIIIFFZJ)V"),
             Method<DrawCanvasTextRunArray>(vm, "nDrawTextRun", "(J[CIIIIFFZJJ)V"),
             Method<DrawCanvasBitmapAt>(vm, "nDrawBitmap", "(JJFFJIII)V"),
             Method<DrawCanvasBitmapRect>(vm, "nDrawBitmap", "(JJFFFFFFFFJII)V"),
             Method<DrawCanvasBitmapPixels>(vm, "nDrawBitmap", "(J[IIIFFIIZJ)V"),
             Method<DrawCanvasBitmapMatrix>(vm, "nDrawBitmapMatrix", "(JJJJ)V"));
    return true;
}

} // namespace
