void DrawDoubleRoundRects(JNIEnv* env, jlong id, wa_rect_t outer, const std::array<float, 8>& outer_radii,
                          wa_rect_t inner, const std::array<float, 8>& inner_radii, jlong paint) {
    const auto canvas = Canvas(env, id);
    const auto* p = Paint(env, paint);
    if (!canvas || !p || !HasArea(outer)) return;
    PathGeometry path;
    path.RoundRect(outer, outer_radii);
    if (HasArea(inner)) path.RoundRect(inner, inner_radii, 1);
    EmitPath(canvas, path, *p);
}
void DrawDoubleRoundRect(JNIEnv* env, jclass, jlong id, jfloat l, jfloat t, jfloat r, jfloat b,
                         jfloat rx, jfloat ry, jfloat il, jfloat it, jfloat ir, jfloat ib,
                         jfloat irx, jfloat iry, jlong paint) {
    CallSafely(env, [&] {
        DrawDoubleRoundRects(env, id, {l, t, r, b}, {rx, ry, rx, ry, rx, ry, rx, ry},
                             {il, it, ir, ib}, {irx, iry, irx, iry, irx, iry, irx, iry}, paint);
    });
}
void DrawDoubleRoundRectRadii(JNIEnv* env, jclass, jlong id, jfloat l, jfloat t, jfloat r, jfloat b,
                              jfloatArray outer, jfloat il, jfloat it, jfloat ir, jfloat ib,
                              jfloatArray inner, jlong paint) {
    CallSafely(env, [&] {
        std::array<float, 8> a{}, b_radii{};
        env->GetFloatArrayRegion(outer, 0, a.size(), a.data());
        if (env->ExceptionCheck()) return;
        env->GetFloatArrayRegion(inner, 0, b_radii.size(), b_radii.data());
        if (env->ExceptionCheck()) return;
        DrawDoubleRoundRects(env, id, {l, t, r, b}, a, {il, it, ir, ib}, b_radii, paint);
    });
}
void DrawCanvasRegion(JNIEnv* env, jclass, jlong id, jlong region, jlong paint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* r = GetRegion(env, region);
        const auto* p = Paint(env, paint);
        if (canvas && r && p) EmitPath(canvas, RegionPath(*r), *p);
    });
}

jint SaveUnclippedLayer(JNIEnv* env, jclass type, jlong id, jint l, jint t, jint r, jint b) {
    return CallSafely(env, [&]() -> jint {
        const auto canvas = Canvas(env, id);
        const auto local = PathRect({l, t, r, b});
        if (!canvas.bitmap || !HasArea(local)) return SaveCanvas(env, type, id, 3);
        const auto& frame = canvas.data->frame;
        const auto area = Intersect(Intersect(MapRect(frame.matrix, local), frame.clip),
                                    {0, 0, float(canvas.bitmap->width), float(canvas.bitmap->height)});
        const auto inverse = Inverse(frame.matrix);
        if (!HasArea(area) || !inverse) return SaveCanvas(env, type, id, 3);
        const jint left = std::floor(area.l), top = std::floor(area.t);
        const jint width = std::ceil(area.r) - left, height = std::ceil(area.b) - top;
        const auto size = static_cast<size_t>(width) * height;
        auto backdrop = std::make_shared<CanvasData::Backdrop>(CanvasData::Backdrop{
            local, frame.matrix, left, top, width, height, std::vector<jint>(size), std::vector<uint8_t>(size)});
        const auto clip = RasterTransform(frame);
        const auto saved = SaveCanvas(env, type, id, 3);
        if (env->ExceptionCheck()) return saved;
        auto& bitmap = *canvas.bitmap;
        for (jint y = 0; y < height; ++y) {
            for (jint x = 0; x < width; ++x) {
                const float dev_x = left + x + .5f, dev_y = top + y + .5f;
                if (dev_x < area.l || dev_x >= area.r || dev_y < area.t || dev_y >= area.b) continue;
                const auto [sx, sy] = MapPoint(*inverse, dev_x, dev_y);
                if (sx < local.l || sx >= local.r || sy < local.t || sy >= local.b ||
                    ClipOpacity(frame, clip.elements, dev_x, dev_y) == 0) continue;
                const auto index = static_cast<size_t>(y) * width + x;
                auto& pixel = bitmap.pixels[static_cast<size_t>(top + y) * bitmap.width + left + x];
                backdrop->saved[index] = true;
                backdrop->pixels[index] = std::exchange(pixel, 0);
            }
        }
        bitmap.Changed();
        canvas.data->frame.backdrop = std::move(backdrop);
        return saved;
    });
}
void RestoreUnclippedLayer(JNIEnv* env, jclass type, jlong id, jint count, jlong paint) {
    CallSafely(env, [&] {
        if (count < 1 || count >= CanvasSaveCount(env, type, id)) return;
        RestoreCanvasToCount(env, type, id, count + 1);
        const auto canvas = Canvas(env, id);
        const auto* p = Paint(env, paint);
        if (!canvas || !p || CanvasSaveCount(env, type, id) != count + 1) return;
        if (const auto& backdrop = canvas.data->frame.backdrop; backdrop && canvas.bitmap) {
            auto& frame = canvas.data->frame;
            PathGeometry path;
            path.Rect(backdrop->local);
            if (const auto inverse = Inverse(frame.matrix))
                path.Transform(Multiply(*inverse, backdrop->transform));
            auto copy = *p;
            copy.style = 0;
            EmitPath(canvas, path, copy);
        }
        RestoreCanvas(env, type, id);
    });
}
void SetCanvasDrawFilter(JNIEnv* env, jclass, jlong id, jlong filter) {
    if (Canvas(env, id) && filter)
        Throw(env, "java/lang/UnsupportedOperationException", "Native draw filters require Skia");
}
void PunchCanvasHole(JNIEnv* env, jclass, jlong id, jfloat l, jfloat t, jfloat r, jfloat b,
                     jfloat rx, jfloat ry, jfloat alpha) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        if (!canvas.bitmap || !HasArea({l, t, r, b}) || !(alpha > 0)) return;
        PaintData paint;
        paint.color = Opacity(0xff000000u, std::min(alpha, 1.f));
        paint.blend = 8; // DST_OUT
        PathGeometry path;
        path.RoundRect({l, t, r, b}, {rx, ry, rx, ry, rx, ry, rx, ry});
        EmitPath(canvas, path, paint);
    });
}

void EmitPositionedText(JNIEnv* env, const CanvasTarget& canvas, const PaintData& original,
                        std::u16string_view text, float x, float y, float angle = 0) {
    auto paint = original;
    paint.align = 0;
    const auto rotate = AtPivot(Degrees(angle), x, y);
    if (canvas.bitmap || paint.shader || !canvas.data->frame.masks.empty()) {
        PathGeometry glyph;
        AppendTextBounds(*canvas.state, glyph, paint, text, x, y);
        if (angle != 0) glyph.Transform(rotate);
        EmitPath(canvas, glyph, paint);
    } else if (canvas.display) {
        if (angle == 0) {
            EmitText(env, canvas, paint, text, x, y);
            return;
        }
        auto& status = canvas.state->result;
        if (status != WA_SF_OK) return;
        status = wa_canvas_save(canvas.display);
        if (status != WA_SF_OK) return;
        EmitMatrix(canvas, rotate);
        EmitText(env, canvas, paint, text, x, y);
        const auto restored = wa_canvas_restore(canvas.display);
        if (status == WA_SF_OK) status = restored;
    }
}

void DrawTextOnPath(JNIEnv* env, jlong id, std::u16string_view text, jlong path, float x, float y, jlong paint) {
    const auto canvas = Canvas(env, id);
    const auto* p = Paint(env, paint);
    const auto* geometry = Path(env, path);
    if (!canvas || !p || !geometry || env->ExceptionCheck()) return;
    const auto contours = interp::software_raster::Flatten(RasterPathValues(*geometry), geometry->verbs.size());
    if (contours.empty()) return;
    auto points = contours.front().points;
    if (contours.front().closed && !points.empty()) points.push_back(points.front());
    if (points.size() < 2) return;
    std::vector<double> distances(points.size());
    for (size_t i = 1; i < points.size(); ++i)
        distances[i] = distances[i - 1] + std::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y);
    double distance = x;
    if (p->align != 0) {
        const auto remaining = distances.back() - Measure(*canvas.state, *p, text).advance;
        distance += p->align == 1 ? remaining / 2 : remaining;
    }
    for (size_t i = 0; i < text.size();) {
        const size_t length = text[i] >= 0xd800 && text[i] <= 0xdbff && i + 1 < text.size() &&
                              text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff ? 2 : 1;
        const auto glyph = text.substr(i, length);
        const auto advance = Measure(*canvas.state, *p, glyph).advance;
        const double center = distance + advance / 2;
        if (center >= distances.back()) break;
        if (center >= 0) {
            const auto next = std::ranges::upper_bound(distances, center) - distances.begin();
            const auto length = distances[next] - distances[next - 1];
            const auto direction = (points[next] - points[next - 1]) * (1 / length);
            const auto baseline = points[next - 1] + direction * (distance - distances[next - 1]) +
                                  Point{-direction.y, direction.x} * y;
            const auto angle = std::atan2(direction.y, direction.x) * 180 / std::numbers::pi;
            EmitPositionedText(env, canvas, *p, glyph, baseline.x, baseline.y, angle);
        }
        distance += advance;
        i += length;
    }
}
void DrawTextOnPathString(JNIEnv* env, jclass, jlong id, jstring text, jlong path, jfloat x,
                          jfloat y, jint, jlong paint) {
    CallSafely(env, [&] { DrawTextOnPath(env, id, Chars(env, text, 0, env->GetStringLength(text)), path, x, y, paint); });
}
void DrawTextOnPathArray(JNIEnv* env, jclass, jlong id, jcharArray text, jint start, jint count,
                         jlong path, jfloat x, jfloat y, jint, jlong paint) {
    CallSafely(env, [&] { DrawTextOnPath(env, id, Chars(env, text, start, count), path, x, y, paint); });
}
void DrawCanvasGlyphs(JNIEnv* env, jclass, jlong id, jintArray glyphs, jfloatArray positions,
                      jint glyph_offset, jint position_offset, jint count, jlong font, jlong paint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* p = Paint(env, paint);
        if (!canvas || !p || count <= 0 || count > INT32_MAX / 2) return;
        std::vector<jint> ids(count);
        std::vector<jfloat> xy(size_t(count) * 2);
        env->GetIntArrayRegion(glyphs, glyph_offset, count, ids.data());
        if (!env->ExceptionCheck())
            env->GetFloatArrayRegion(positions, position_offset, xy.size(), xy.data());
        if (env->ExceptionCheck()) return;
        auto copy = *p;
        if (font) copy.typeface = font;
        for (jint i = 0; i < count; ++i) {
            uint32_t glyph = ids[i];
            if (glyph > 0x10ffff || (glyph >= 0xd800 && glyph <= 0xdfff)) glyph = 0xfffd;
            if (glyph < 0x10000) {
                const char16_t ch = glyph;
                EmitPositionedText(env, canvas, copy, {&ch, 1}, xy[2 * i], xy[2 * i + 1]);
            } else {
                glyph -= 0x10000;
                const char16_t pair[]{char16_t(0xd800 + (glyph >> 10)), char16_t(0xdc00 + (glyph & 0x3ff))};
                EmitPositionedText(env, canvas, copy, {pair, 2}, xy[2 * i], xy[2 * i + 1]);
            }
        }
    });
}

struct CanvasVertex {
    Point position;
    Point texture;
    uint32_t color = 0xffffffffu;
};
using CanvasTriangle = std::array<size_t, 3>;

void DrawCanvasTriangles(JNIEnv* env, const CanvasTarget& canvas, const PaintData& paint,
                         std::span<const CanvasVertex> vertices, std::span<const CanvasTriangle> triangles,
                         const BitmapData* image, bool vertex_colors) {
    struct Primitive {
        CanvasTriangle indices;
        double determinant;
    };
    std::vector<Primitive> primitives;
    PathGeometry shape;
    const auto cross = [](Point a, Point b) { return a.x * b.y - a.y * b.x; };
    for (auto triangle : triangles) {
        if (std::ranges::any_of(triangle, [&](size_t index) { return index >= vertices.size(); })) {
            Throw(env, "java/lang/IllegalArgumentException", "Canvas vertex index out of range");
            return;
        }
        const auto a = vertices[triangle[0]].position;
        const auto b = vertices[triangle[1]].position;
        const auto c = vertices[triangle[2]].position;
        const auto determinant = cross(b - a, c - a);
        if (!std::isfinite(determinant) || determinant == 0) continue;
        primitives.push_back({triangle, determinant});
        shape.Move(a.x, a.y);
        const auto first = determinant > 0 ? b : c, second = determinant > 0 ? c : b;
        shape.Line(first.x, first.y);
        shape.Line(second.x, second.y);
        shape.Close();
    }
    if (primitives.empty()) return;
    PaintData fill = paint;
    fill.style = 0;
    RasterizePaintedPath(canvas, shape, fill, [&](float x, float y) -> uint32_t {
        for (const auto& [triangle, determinant] : primitives) {
            const auto& a = vertices[triangle[0]], &b = vertices[triangle[1]], &c = vertices[triangle[2]];
            const auto delta = Point{x, y} - a.position;
            const auto tb = cross(delta, c.position - a.position) / determinant;
            const auto tc = cross(b.position - a.position, delta) / determinant;
            const auto ta = 1 - tb - tc;
            if (std::min({ta, tb, tc}) < -1e-8) continue;
            const auto uv = a.texture * ta + b.texture * tb + c.texture * tc;
            const auto sample = image ? SampleBitmap(*image, 0, 0, paint.flags & 2, uv.x, uv.y, paint.color)
                              : paint.shader ? SampleShader(*paint.shader, uv.x, uv.y, paint.color) : paint.color;
            uint32_t color = sample;
            if (vertex_colors) {
                const std::array colors{a.color, b.color, c.color};
                const std::array weights{ta, tb, tc};
                const auto vertex = InterpolateColors(colors, weights);
                color = image || paint.shader ? BlendColor(sample, vertex, 13) : vertex;
            }
            const auto opacity = canvas.data->frame.alpha *
                (vertex_colors || image || paint.shader ? (paint.color >> 24) / 255.f : 1.f);
            return Opacity(FilterColor(paint, color), opacity);
        }
        return 0;
    });
}

std::vector<float> ReadVertexCoordinates(JNIEnv* env, jfloatArray array, jint offset, jint count) {
    if (!array || offset < 0 || count < 0 || offset > env->GetArrayLength(array) - count) {
        Throw(env, "java/lang/ArrayIndexOutOfBoundsException", "Canvas vertex array range");
        return {};
    }
    std::vector<float> values(count);
    env->GetFloatArrayRegion(array, offset, count, values.data());
    return values;
}
void DrawBitmapMesh(JNIEnv* env, jclass, jlong id, jlong image, jint w, jint h,
                    jfloatArray points, jint offset, jintArray colors, jint color_offset, jlong paint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* bitmap = Bitmap(env, image);
        const auto* p = BitmapPaint(env, paint);
        if (!canvas || !bitmap || !p || bitmap->recycled || w <= 0 || h <= 0) return;
        const auto count = (int64_t(w) + 1) * (int64_t(h) + 1);
        if (count > INT32_MAX / 2) {
            Throw(env, "java/lang/IllegalArgumentException", "Bitmap mesh dimensions");
            return;
        }
        const auto xy = ReadVertexCoordinates(env, points, offset, count * 2);
        if (env->ExceptionCheck()) return;
        std::vector<jint> tint(colors ? count : 0);
        if (colors) env->GetIntArrayRegion(colors, color_offset, count, tint.data());
        if (env->ExceptionCheck()) return;
        std::vector<CanvasVertex> vertices;
        vertices.reserve(count);
        std::vector<CanvasTriangle> triangles;
        for (int y = 0; y <= h; ++y) {
            for (int x = 0; x <= w; ++x) {
                const size_t i = vertices.size();
                vertices.push_back({{xy[2 * i], xy[2 * i + 1]},
                    {double(x) * bitmap->width / w, double(y) * bitmap->height / h},
                    colors ? uint32_t(tint[i]) : 0xffffffffu});
                if (x > 0 && y > 0) {
                    const size_t top = i - w - 1;
                    triangles.push_back({top - 1, top, i});
                    triangles.push_back({top - 1, i, i - 1});
                }
            }
        }
        DrawCanvasTriangles(env, canvas, *p, vertices, triangles, bitmap, colors);
    });
}
void DrawCanvasVertices(JNIEnv* env, jclass, jlong id, jint mode, jint count, jfloatArray points,
                        jint offset, jfloatArray tex, jint tex_offset, jintArray colors, jint color_offset,
                        jshortArray indices, jint index_offset, jint index_count, jlong paint) {
    CallSafely(env, [&] {
        const auto canvas = Canvas(env, id);
        const auto* p = Paint(env, paint);
        if (!canvas || !p || count < 6 || mode < 0 || mode > 2) return;
        const auto xy = ReadVertexCoordinates(env, points, offset, count);
        if (env->ExceptionCheck()) return;
        const auto uv = tex ? ReadVertexCoordinates(env, tex, tex_offset, count) : std::vector<float>{};
        if (env->ExceptionCheck()) return;
        std::vector<jint> tint(colors ? count / 2 : 0);
        if (colors) env->GetIntArrayRegion(colors, color_offset, tint.size(), tint.data());
        if (env->ExceptionCheck()) return;
        std::vector<CanvasVertex> vertices;
        for (jint i = 0; i + 1 < count; i += 2) {
            const Point position{xy[i], xy[i + 1]};
            vertices.push_back({position, tex ? Point{uv[i], uv[i + 1]} : position,
                                colors ? uint32_t(tint[i / 2]) : 0xffffffffu});
        }
        std::vector<jshort> order;
        if (indices) {
            if (index_count < 3 || index_offset < 0 || index_offset > env->GetArrayLength(indices) - index_count)
                return;
            order.resize(index_count);
            env->GetShortArrayRegion(indices, index_offset, index_count, order.data());
        }
        if (env->ExceptionCheck()) return;
        const auto vertex = [&](size_t index) -> size_t {
            return indices ? static_cast<uint16_t>(order[index]) : index;
        };
        std::vector<CanvasTriangle> triangles;
        const size_t end = indices ? order.size() : vertices.size();
        for (size_t i = 2; i < end; i += mode == 0 ? 3 : 1)
            triangles.push_back({vertex(mode == 2 ? 0 : i - 2), vertex(i - 1), vertex(i)});
        DrawCanvasTriangles(env, canvas, *p, vertices, triangles, nullptr, colors);
    });
}
void DrawCanvasMesh(JNIEnv* env, jclass, jlong id, jlong mesh, jint, jlong) {
    if (Canvas(env, id) && mesh)
        Throw(env, "java/lang/UnsupportedOperationException", "Programmable meshes require Skia");
}
