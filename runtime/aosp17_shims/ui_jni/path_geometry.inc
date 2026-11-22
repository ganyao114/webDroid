void PathGeometry::Add(wa_path_command command, std::array<float, 2> a,
                       std::array<float, 2> b, std::array<float, 2> c) {
    if (verbs.size() >= WA_SF_LAYER_LIST_CAPACITY / sizeof(uint32_t))
        throw std::length_error("Path exceeds the display list capacity");
    verbs.push_back({static_cast<uint32_t>(command), {a[0], a[1], b[0], b[1], c[0], c[1]}});
    clip.reset();
    generation = generation == INT32_MAX ? 1 : generation + 1;
}

void PathGeometry::Move(float x, float y) {
    current = contour = {x, y};
    Add(WA_PATH_MOVE, current);
    needs_move = false;
}

void PathGeometry::Line(float x, float y) {
    if (needs_move) Move(current[0], current[1]);
    current = {x, y};
    Add(WA_PATH_LINE, current);
}

void PathGeometry::Quad(float x, float y, float x2, float y2) {
    if (needs_move) Move(current[0], current[1]);
    current = {x2, y2};
    Add(WA_PATH_QUAD, {x, y}, current);
}

void PathGeometry::Cubic(float x, float y, float x2, float y2, float x3, float y3) {
    if (needs_move) Move(current[0], current[1]);
    current = {x3, y3};
    Add(WA_PATH_CUBIC, {x, y}, {x2, y2}, current);
}

void PathGeometry::Close() {
    if (needs_move) return;
    Add(WA_PATH_CLOSE);
    current = contour;
    needs_move = true;
}

void PathGeometry::Rect(wa_rect_t rect, jint direction) {
    if (!HasArea(rect)) return;
    const bool first = verbs.empty();
    const auto [l, t, r, b] = rect;
    Move(l, t);
    Line(direction ? l : r, direction ? b : t);
    Line(r, b);
    Line(direction ? r : l, direction ? t : b);
    Close();
    if (first) clip = wa_round_rect_t{rect, 0, 0};
}

void PathGeometry::Arc(wa_rect_t rect, float start, float sweep, bool move) {
    if (!HasArea(rect) || !std::isfinite(start) || !std::isfinite(sweep)) return;
    constexpr double radians = std::numbers::pi / 180;
    const double rx = (rect.r - rect.l) / 2, ry = (rect.b - rect.t) / 2;
    const double cx = rect.l + rx, cy = rect.t + ry;
    double angle = std::remainder(start, 360.f) * radians;
    auto point = [&](double angle) { return Point{cx + rx * std::cos(angle), cy + ry * std::sin(angle)}; };
    auto from = point(angle);
    if (move || verbs.empty()) Move(from.x, from.y);
    else if (current != std::array<float, 2>{float(from.x), float(from.y)}) Line(from.x, from.y);
    if (sweep == 0) return;
    const int segments = std::max(1, static_cast<int>(std::ceil(std::min(360.f, std::abs(sweep)) / 90)));
    const double step = std::clamp(sweep, -360.f, 360.f) * radians / segments;
    const double factor = 4.0 / 3.0 * std::tan(step / 4);
    for (int i = 0; i < segments; ++i) {
        const auto a = from + Point{-rx * std::sin(angle), ry * std::cos(angle)} * factor;
        angle += step;
        const auto to = point(angle);
        const auto b = to - Point{-rx * std::sin(angle), ry * std::cos(angle)} * factor;
        Cubic(a.x, a.y, b.x, b.y, to.x, to.y);
        from = to;
    }
}

void PathGeometry::Oval(wa_rect_t rect, jint direction) {
    if (!HasArea(rect)) return;
    const bool first = verbs.empty();
    Arc(rect, 0, direction ? -360.f : 360.f, true);
    Close();
    if (first) clip = wa_round_rect_t{rect, (rect.r - rect.l) / 2, (rect.b - rect.t) / 2};
}

void PathGeometry::RoundRect(wa_rect_t rect, std::array<float, 8> radii, jint direction) {
    if (!HasArea(rect)) return;
    const auto [l, t, r, b] = rect;
    const auto clean = [](float x) { return std::isfinite(x) ? std::max(0.f, x) : 0.f; };
    std::ranges::transform(radii, radii.begin(), clean);
    const auto ratio = [](float extent, float sum) { return sum > extent ? extent / sum : 1.f; };
    const float scale = std::min({ratio(r - l, radii[0] + radii[2]), ratio(r - l, radii[4] + radii[6]),
                                  ratio(b - t, radii[1] + radii[7]), ratio(b - t, radii[3] + radii[5])});
    for (float& value : radii) value *= scale;
    const bool first = verbs.empty();
    const std::array cx{l + radii[0], r - radii[2], r - radii[4], l + radii[6]};
    const std::array cy{t + radii[1], t + radii[3], b - radii[5], b - radii[7]};
    for (int i = 0; i < 4; ++i) {
        const int corner = direction ? 3 - i : i;
        const float rx = radii[corner * 2], ry = radii[corner * 2 + 1];
        if (rx > 0 && ry > 0) {
            Arc({cx[corner] - rx, cy[corner] - ry, cx[corner] + rx, cy[corner] + ry},
                180.f + (corner + (direction ? 1 : 0)) * 90.f, direction ? -90.f : 90.f, i == 0);
        } else if (i == 0) {
            Move(corner == 0 || corner == 3 ? l : r, corner < 2 ? t : b);
        } else {
            Line(corner == 0 || corner == 3 ? l : r, corner < 2 ? t : b);
        }
    }
    Close();
    if (first && radii == std::array{radii[0], radii[1], radii[0], radii[1],
                                    radii[0], radii[1], radii[0], radii[1]}) {
        clip = wa_round_rect_t{rect, radii[0], radii[1]};
    }
}

int PathCoordinates(uint32_t verb) {
    constexpr std::array counts{0, 2, 2, 4, 6, 0};
    return counts[verb];
}

void PathGeometry::Transform(const Matrix9& matrix) {
    for (auto& verb : verbs) {
        for (int i = 0; i < PathCoordinates(verb.command); i += 2) {
            const auto point = MapPoint(matrix, verb.points[i], verb.points[i + 1]);
            std::ranges::copy(point, verb.points + i);
        }
    }
    current = MapPoint(matrix, current[0], current[1]);
    contour = MapPoint(matrix, contour[0], contour[1]);
    if (clip && matrix[1] == 0 && matrix[3] == 0 && matrix[6] == 0 && matrix[7] == 0 && matrix[8] == 1) {
        clip->rect = MapRect(matrix, clip->rect);
        clip->rx *= std::abs(matrix[0]);
        clip->ry *= std::abs(matrix[4]);
    } else {
        clip.reset();
    }
    generation = generation == INT32_MAX ? 1 : generation + 1;
}

wa_rect_t PathGeometry::Bounds() const {
    wa_rect_t bounds{};
    bool any = false;
    for (const auto& verb : verbs) {
        for (int i = 0; i < PathCoordinates(verb.command); i += 2) {
            const float x = verb.points[i], y = verb.points[i + 1];
            if (!any) { any = true; bounds = {x, y, x, y}; }
            bounds.l = std::min(bounds.l, x);
            bounds.t = std::min(bounds.t, y);
            bounds.r = std::max(bounds.r, x);
            bounds.b = std::max(bounds.b, y);
        }
    }
    return bounds;
}

bool PathGeometry::Empty() const {
    return std::ranges::none_of(verbs, [](const auto& v) {
        return v.command >= WA_PATH_LINE && v.command <= WA_PATH_CUBIC;
    });
}

namespace {

jlong NewPath(JNIEnv* env, jclass, jlong source) {
    return CallSafely(env, [&]() -> jlong {
        auto* state = GetState(env);
        if (!state) return 0;
        auto& paths = Peers<PathGeometry, &State::paths>(*state);
        if (!source) return paths.Insert();
        const auto* path = paths.Get(env, source);
        return path ? paths.Insert(*path) : 0;
    });
}
jlong InitPath(JNIEnv* env, jclass type) { return NewPath(env, type, 0); }
jlong PathFinalizer(JNIEnv*, jclass) { return 0; }
void ResetPath(JNIEnv* env, jclass, jlong id) {
    if (auto* path = Path(env, id)) {
        path->verbs.clear();
        path->current = path->contour = {};
        path->clip.reset();
        path->needs_move = true;
        path->generation = path->generation == INT32_MAX ? 1 : path->generation + 1;
    }
}
void SetPath(JNIEnv* env, jclass, jlong dst, jlong src) {
    CallSafely(env, [&] {
        auto* path = Path(env, dst);
        if (const auto* source = Path(env, src); path && source) *path = *source;
    });
}
void PathBounds(JNIEnv* env, jclass, jlong id, jobject bounds) {
    if (const auto* path = Path(env, id)) SetRect(env, bounds, path->Bounds(), true);
}
jint GetFillType(JNIEnv* env, jclass, jlong id) {
    const auto* path = Path(env, id);
    return path ? path->fill : 0;
}
void SetFillType(JNIEnv* env, jclass, jlong id, jint fill) {
    if (auto* path = Path(env, id)) {
        if (path->fill != fill) {
            path->fill = fill;
            path->generation = path->generation == INT32_MAX ? 1 : path->generation + 1;
        }
    }
}
jint PathGeneration(JNIEnv* env, jclass, jlong id) {
    const auto* path = Path(env, id);
    return path ? path->generation : 0;
}
jboolean IsPathEmpty(JNIEnv* env, jclass, jlong id) {
    const auto* path = Path(env, id);
    return !path || path->Empty();
}
jboolean IsPathRect(JNIEnv* env, jclass, jlong id, jobject rect) {
    const auto* path = Path(env, id);
    if (!path || !path->clip || path->clip->rx != 0 || path->clip->ry != 0) return false;
    if (rect) SetRect(env, rect, path->clip->rect, true);
    return true;
}
void ReservePath(JNIEnv* env, jclass, jlong id, jint count) {
    CallSafely(env, [&] {
        if (auto* path = Path(env, id); path && count > 0)
            path->verbs.reserve(std::min(path->verbs.size() + count, size_t(WA_SF_LAYER_LIST_CAPACITY / 4)));
    });
}
template <auto Operation, bool Relative = false, class... Args>
void EditPath(JNIEnv* env, jclass, jlong id, Args... args) {
    CallSafely(env, [&] {
        if (auto* path = Path(env, id)) {
            if constexpr (Relative) {
                std::array coords{args...};
                for (size_t i = 0; i < coords.size(); ++i) coords[i] += path->current[i % 2];
                std::apply([&](auto... v) { (path->*Operation)(v...); }, coords);
            } else {
                (path->*Operation)(args...);
            }
        }
    });
}
template <auto Operation>
void AddShape(JNIEnv* env, jclass, jlong id, jfloat l, jfloat t, jfloat r, jfloat b, jint direction) {
    CallSafely(env, [&] { if (auto* path = Path(env, id)) (path->*Operation)({l, t, r, b}, direction); });
}
void AddRoundRect(JNIEnv* env, jclass, jlong id, jfloat l, jfloat t, jfloat r, jfloat b,
                  jfloat rx, jfloat ry, jint direction) {
    CallSafely(env, [&] {
        if (auto* path = Path(env, id))
            path->RoundRect({l, t, r, b}, {rx, ry, rx, ry, rx, ry, rx, ry}, direction);
    });
}
void AddRoundRectArray(JNIEnv* env, jclass, jlong id, jfloat l, jfloat t, jfloat r, jfloat b,
                       jfloatArray radii, jint direction) {
    std::array<float, 8> values;
    env->GetFloatArrayRegion(radii, 0, values.size(), values.data());
    if (env->ExceptionCheck()) return;
    CallSafely(env, [&] {
        if (auto* path = Path(env, id)) path->RoundRect({l, t, r, b}, values, direction);
    });
}
void AddCircle(JNIEnv* env, jclass type, jlong id, jfloat x, jfloat y, jfloat radius, jint direction) {
    AddShape<&PathGeometry::Oval>(env, type, id, x - radius, y - radius, x + radius, y + radius, direction);
}
void ArcPath(JNIEnv* env, jclass, jlong id, jfloat l, jfloat t, jfloat r, jfloat b,
             jfloat start, jfloat sweep, jboolean move) {
    CallSafely(env, [&] { if (auto* path = Path(env, id)) path->Arc({l, t, r, b}, start, sweep, move); });
}
void AddArc(JNIEnv* env, jclass type, jlong id, jfloat l, jfloat t, jfloat r, jfloat b,
            jfloat start, jfloat sweep) {
    ArcPath(env, type, id, l, t, r, b, start, sweep, true);
}
template <bool Relative>
void AddConic(JNIEnv* env, jclass, jlong id, jfloat x, jfloat y, jfloat ex, jfloat ey, jfloat weight) {
    CallSafely(env, [&] {
        auto* path = Path(env, id);
        if (!path) return;
        const auto [sx, sy] = path->current;
        if constexpr (Relative) { x += sx; y += sy; ex += sx; ey += sy; }
        if (!(weight > 0) || !std::isfinite(weight)) { path->Line(ex, ey); return; }
        if (weight == 1) { path->Quad(x, y, ex, ey); return; }
        // The display list supports polynomial curves. Sample the rational
        // quadratic in parameter order, including its authoritative end point.
        for (int i = 1; i <= 16; ++i) {
            const double t = i / 16.0, u = 1 - t, middle = 2 * weight * t * u;
            const double denominator = u * u + middle + t * t;
            path->Line((sx * u * u + x * middle + ex * t * t) / denominator,
                       (sy * u * u + y * middle + ey * t * t) / denominator);
        }
    });
}
void OffsetPath(JNIEnv* env, jclass, jlong id, jfloat x, jfloat y) {
    if (auto* path = Path(env, id)) path->Transform(Translation(x, y));
}
void AppendPath(JNIEnv* env, jlong dst, jlong src, const Matrix9& matrix) {
    CallSafely(env, [&] {
        auto* path = Path(env, dst);
        const auto* source = Path(env, src);
        if (!path || !source || source->verbs.empty()) return;
        auto addition = *source;
        if (matrix != kIdentity) addition.Transform(matrix);
        if (path->verbs.size() + addition.verbs.size() > WA_SF_LAYER_LIST_CAPACITY / 4)
            throw std::length_error("Path exceeds the display list capacity");
        const bool first = path->verbs.empty();
        path->verbs.insert(path->verbs.end(), addition.verbs.begin(), addition.verbs.end());
        path->current = addition.current;
        path->contour = addition.contour;
        path->needs_move = addition.needs_move;
        path->clip = first ? addition.clip : std::nullopt;
        path->generation = path->generation == INT32_MAX ? 1 : path->generation + 1;
    });
}
void AddPath(JNIEnv* env, jclass, jlong dst, jlong src) { AppendPath(env, dst, src, kIdentity); }
void AddPathOffset(JNIEnv* env, jclass, jlong dst, jlong src, jfloat x, jfloat y) {
    AppendPath(env, dst, src, Translation(x, y));
}
void AddPathMatrix(JNIEnv* env, jclass, jlong dst, jlong src, jlong transform) {
    if (const auto* matrix = MatrixValues(env, transform)) AppendPath(env, dst, src, *matrix);
}
void TransformPathTo(JNIEnv* env, jclass type, jlong src, jlong transform, jlong dst) {
    if (const auto* matrix = MatrixValues(env, transform)) {
        if (src != dst) SetPath(env, type, dst, src);
        if (!env->ExceptionCheck()) if (auto* path = Path(env, dst)) path->Transform(*matrix);
    }
}
void TransformPath(JNIEnv* env, jclass type, jlong src, jlong transform) {
    TransformPathTo(env, type, src, transform, src);
}
void SetLastPoint(JNIEnv* env, jclass, jlong id, jfloat x, jfloat y) {
    CallSafely(env, [&] {
        auto* path = Path(env, id);
        if (!path) return;
        for (auto it = path->verbs.rbegin(); it != path->verbs.rend(); ++it) {
            const int count = PathCoordinates(it->command);
            if (!count) continue;
            it->points[count - 2] = x;
            it->points[count - 1] = y;
            path->current = {x, y};
            if (it->command == WA_PATH_MOVE) path->contour = path->current;
            path->clip.reset();
            path->generation = path->generation == INT32_MAX ? 1 : path->generation + 1;
            return;
        }
        path->Move(x, y);
    });
}
std::vector<interp::Value> PathValues(const PathGeometry& path) {
    std::vector<interp::Value> values;
    values.reserve(path.verbs.size() * 7);
    for (const auto& verb : path.verbs) {
        values.push_back(interp::Value::Float(verb.command));
        for (float coordinate : verb.points) values.push_back(interp::Value::Float(coordinate));
    }
    return values;
}

auto FlattenPath(const PathGeometry& path) {
    return interp::software_raster::Flatten(PathValues(path), path.verbs.size());
}

jfloatArray ApproximatePath(JNIEnv* env, jclass, jlong id, jfloat) {
    return CallSafely(env, [&]() -> jfloatArray {
        const auto* path = Path(env, id);
        if (!path) return nullptr;
        const auto contours = FlattenPath(*path);
        std::vector<float> coords;
        double distance = 0;
        for (const auto& contour : contours) {
            auto points = contour.points;
            if (contour.closed && !points.empty()) points.push_back(points.front());
            for (size_t i = 0; i < points.size(); ++i) {
                if (i > 0) distance += std::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y);
                coords.insert(coords.end(), {float(distance), float(points[i].x), float(points[i].y)});
            }
        }
        if (coords.empty()) coords = {0, 0, 0};
        if (distance == 0) coords.insert(coords.end(), {1, coords[1], coords[2]});
        else for (size_t i = 0; i < coords.size(); i += 3) coords[i] /= distance;
        auto output = env->NewFloatArray(coords.size());
        if (output) env->SetFloatArrayRegion(output, 0, coords.size(), coords.data());
        return output;
    });
}

jboolean PathConvex(JNIEnv* env, jclass, jlong id) {
    return CallSafely(env, [&]() -> jboolean {
        const auto* path = Path(env, id);
        if (!path || path->clip) return path != nullptr;
        auto contours = FlattenPath(*path);
        std::erase_if(contours, [](const auto& c) { return c.points.size() < 3; });
        if (contours.size() != 1) return contours.empty();
        const auto& points = contours.front().points;
        double sign = 0;
        for (size_t i = 0; i < points.size(); ++i) {
            const auto a = points[(i + 1) % points.size()] - points[i];
            const auto b = points[(i + 2) % points.size()] - points[(i + 1) % points.size()];
            const double cross = a.x * b.y - a.y * b.x;
            if (sign * cross < 0) return false;
            if (cross != 0) sign = cross;
        }
        return true;
    });
}

bool CanInterpolate(const PathGeometry& a, const PathGeometry& b) {
    return std::ranges::equal(a.verbs, b.verbs, {}, &wa_path_verb_t::command, &wa_path_verb_t::command);
}
jboolean PathInterpolatable(JNIEnv* env, jclass, jlong a, jlong b) {
    const auto* first = Path(env, a);
    const auto* second = Path(env, b);
    return first && second && CanInterpolate(*first, *second);
}
jboolean InterpolatePath(JNIEnv* env, jclass, jlong a, jlong b, jfloat t, jlong dst) {
    return CallSafely(env, [&]() -> jboolean {
        const auto* first = Path(env, a);
        const auto* second = Path(env, b);
        auto* output = Path(env, dst);
        if (!first || !second || !output || !CanInterpolate(*first, *second)) return false;
        auto result = *first;
        for (size_t i = 0; i < result.verbs.size(); ++i) {
            auto& part = result.verbs[i];
            for (int j = 0; j < PathCoordinates(part.command); ++j)
                part.points[j] = std::lerp(second->verbs[i].points[j], part.points[j], t);
        }
        for (size_t i = 0; i < 2; ++i) {
            result.current[i] = std::lerp(second->current[i], first->current[i], t);
            result.contour[i] = std::lerp(second->contour[i], first->contour[i], t);
        }
        result.clip.reset();
        result.generation = output->generation == INT32_MAX ? 1 : output->generation + 1;
        *output = std::move(result);
        return true;
    });
}

jboolean PathOp(JNIEnv* env, jclass, jlong a, jlong b, jint op, jlong dst) {
    return CallSafely(env, [&]() -> jboolean {
        const auto* first = Path(env, a);
        const auto* second = Path(env, b);
        auto* output = Path(env, dst);
        if (!first || !second || !output || op < 0 || op > 4) return false;
        struct Edge {
            double low, high, x, slope;
            int sign, operand;
            double At(double y) const { return x + slope * (y - low); }
        };
        std::vector<Edge> edges;
        std::vector<double> levels;
        for (const auto& [path, operand] : {std::pair{first, 0}, std::pair{second, 1}}) {
            for (const auto& contour : FlattenPath(*path)) {
                const auto& points = contour.points;
                for (size_t i = 0; i < points.size(); ++i) {
                    const auto p = points[i], q = points[(i + 1) % points.size()];
                    if (!(std::isfinite(p.x) && std::isfinite(p.y))) return false;
                    if (p.y == q.y) continue;
                    const auto [lo, hi] = p.y < q.y ? std::pair{p, q} : std::pair{q, p};
                    edges.push_back({lo.y, hi.y, lo.x, (hi.x - lo.x) / (hi.y - lo.y),
                                     p.y < q.y ? 1 : -1, operand});
                    levels.insert(levels.end(), {lo.y, hi.y});
                    if (edges.size() > 512) return false;
                }
            }
        }
        for (size_t i = 0; i < edges.size(); ++i) {
            const auto& p = edges[i];
            for (size_t j = i + 1; j < edges.size(); ++j) {
                const auto& q = edges[j];
                const double low = std::max(p.low, q.low), high = std::min(p.high, q.high);
                const double slope = p.slope - q.slope;
                if (low >= high || slope == 0) continue;
                const double crossing = low + (q.At(low) - p.At(low)) / slope;
                if (low < crossing && crossing < high) levels.push_back(crossing);
            }
        }
        std::ranges::sort(levels);
        levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
        const auto evaluate = [op](bool a, bool b) {
            switch (op) {
            case 0: return a && !b;
            case 1: return a && b;
            case 2: return a || b;
            case 3: return a != b;
            default: return b && !a;
            }
        };
        const auto inside = [](int count, jint fill) { return (fill & 1 ? count & 1 : count != 0) != (fill >= 2); };
        const bool inverse = evaluate(inside(0, first->fill), inside(0, second->fill));
        PathGeometry result;
        result.fill = inverse ? 2 : 0;
        std::vector<const Edge*> active;
        for (size_t i = 1; i < levels.size(); ++i) {
            const double low = levels[i - 1], high = levels[i], middle = (low + high) / 2;
            active.clear();
            for (const auto& edge : edges) if (edge.low <= middle && middle < edge.high)
                active.push_back(&edge);
            std::ranges::sort(active, {}, [middle](const Edge* edge) { return edge->At(middle); });
            std::array<int, 2> winding{};
            const Edge* left = nullptr;
            for (const auto* edge : active) {
                winding[edge->operand] += edge->sign;
                const bool fill = evaluate(inside(winding[0], first->fill), inside(winding[1], second->fill));
                if (fill != inverse) {
                    if (!left) left = edge;
                } else if (left) {
                    if (left->At(middle) < edge->At(middle)) {
                        if (result.verbs.size() + 5 > WA_SF_LAYER_LIST_CAPACITY / 4) return false;
                        result.Move(left->At(low), low);
                        result.Line(edge->At(low), low);
                        result.Line(edge->At(high), high);
                        result.Line(left->At(high), high);
                        result.Close();
                    }
                    left = nullptr;
                }
            }
        }
        result.generation = output->generation == INT32_MAX ? 1 : output->generation + 1;
        *output = std::move(result);
        return true;
    });
}
} // namespace
