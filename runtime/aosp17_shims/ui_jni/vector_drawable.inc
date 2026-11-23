// VectorDrawable owns the Java tree and animation targets. Native peers keep the
// parsed geometry and the rendering state used by both Canvas implementations.
struct VectorNode {
    enum class Kind { Group, FullPath, ClipPath };
    struct Style {
        float stroke_width = 0;
        jint stroke_color = 0;
        float stroke_alpha = 1;
        jint fill_color = 0;
        float fill_alpha = 1;
        float trim_start = 0;
        float trim_end = 1;
        float trim_offset = 0;
        jint cap = 0;
        jint join = 0;
        float miter = 4;
        jint fill_type = 0;
    };

    explicit VectorNode(Kind kind) : kind(kind) {}
    Kind kind;
    std::string name;
    std::array<jfloat, 7> transform{0, 0, 0, 1, 1, 0, 0};
    Matrix9 group_matrix = kIdentity;
    std::weak_ptr<VectorNode> parent;
    std::vector<std::shared_ptr<VectorNode>> children;
    uint64_t revision = 1;
    PathGeometry path;
    Style style;
    std::optional<GradientData> fill_gradient, stroke_gradient;

    void Invalidate() {
        ++revision;
        if (auto group = parent.lock()) group->Invalidate();
    }

    void UpdateMatrix() {
        const auto [angle, px, py, sx, sy, tx, ty] = transform;
        group_matrix = Multiply(Translation(tx + px, ty + py),
                        Multiply(Degrees(angle), Multiply(Scaling(sx, sy), Translation(-px, -py))));
        Invalidate();
    }
};

struct VectorTree {
    std::shared_ptr<VectorNode> root;
    float width = 0, height = 0, alpha = 1;
    bool antialias = true, allow_caching = true;
    uint64_t cache_generation = 0;
    jlong cached_filter = 0;
    BitmapData cache;

    uint64_t analyzed_generation = 0;
    size_t paint_count = 0;
    bool needs_raster = false;
};

namespace {

using VectorKind = VectorNode::Kind;
using VectorNodes = std::shared_ptr<VectorNode>;

VectorNodes* GetVectorNode(JNIEnv* env, jlong id) {
    return Peer<VectorNodes, &State::vector_nodes>(env, id);
}
VectorNode* GetVectorNode(JNIEnv* env, jlong id, VectorKind kind) {
    const auto* node = GetVectorNode(env, id);
    if (!node) return nullptr;
    if ((*node)->kind == kind) return node->get();
    Throw(env, "java/lang/IllegalArgumentException", "Incorrect VectorDrawable node type");
    return nullptr;
}
VectorTree* GetVectorTree(JNIEnv* env, jlong id) {
    return Peer<VectorTree, &State::vector_trees>(env, id);
}
template <VectorKind Kind>
jlong CreateVectorNodeCopy(JNIEnv* env, jclass, jlong copy) {
    return CallSafely(env, [&]() -> jlong {
        auto* state = GetState(env);
        if (!state) return 0;
        auto node = std::make_shared<VectorNode>(Kind);
        if (copy) {
            const auto* source = GetVectorNode(env, copy, Kind);
            if (!source) return 0;
            node->name = source->name;
            node->transform = source->transform;
            node->group_matrix = source->group_matrix;
            node->path = source->path;
            node->style = source->style;
            node->fill_gradient = source->fill_gradient;
            node->stroke_gradient = source->stroke_gradient;
            // Java copies the child objects and attaches their new handles.
        }
        return Peers<VectorNodes, &State::vector_nodes>(*state).Insert(std::move(node));
    });
}
template <VectorKind Kind>
jlong CreateVectorNode(JNIEnv* env, jclass cls) { return CreateVectorNodeCopy<Kind>(env, cls, 0); }

jlong CreateVectorTreeCopy(JNIEnv* env, jclass, jlong source, jlong root) {
    return CallSafely(env, [&]() -> jlong {
        auto* state = GetState(env);
        const auto* node = GetVectorNode(env, root);
        if (!state || !node) return 0;
        VectorTree tree;
        tree.root = *node;
        if (source) {
            const auto* other = GetVectorTree(env, source);
            if (!other) return 0;
            tree.width = other->width;
            tree.height = other->height;
            tree.alpha = other->alpha;
            tree.antialias = other->antialias;
            tree.allow_caching = other->allow_caching;
        }
        return Peers<VectorTree, &State::vector_trees>(*state).Insert(std::move(tree));
    });
}
jlong CreateVectorTree(JNIEnv* env, jclass cls, jlong root) {
    return CreateVectorTreeCopy(env, cls, 0, root);
}
void ReleaseVectorTree(JNIEnv* env, jclass, jlong id) {
    if (auto* state = GetState(env); state && state->vector_trees) state->vector_trees->Erase(id);
}

void AddVectorChild(JNIEnv* env, jclass, jlong parent, jlong child) {
    CallSafely(env, [&] {
        const auto* group = GetVectorNode(env, parent);
        const auto* node = GetVectorNode(env, child);
        if (!group || !node) return;
        if ((*group)->kind != VectorKind::Group || !(*node)->parent.expired()) {
            Throw(env, "java/lang/IllegalArgumentException", "Invalid VectorDrawable child");
            return;
        }
        (*group)->children.push_back(*node);
        (*node)->parent = *group;
        (*group)->Invalidate();
    });
}
void SetVectorName(JNIEnv* env, jclass, jlong id, jstring name) {
    CallSafely(env, [&] {
        if (const auto* node = GetVectorNode(env, id)) (*node)->name = Utf8(env, name);
    });
}
void SetVectorViewport(JNIEnv* env, jclass, jlong id, jfloat width, jfloat height) {
    if (auto* tree = GetVectorTree(env, id); tree && (tree->width != width || tree->height != height)) {
        tree->width = width;
        tree->height = height;
        tree->cache_generation = 0;
    }
}
jfloat GetVectorAlpha(JNIEnv* env, jclass, jlong id) {
    if (const auto* tree = GetVectorTree(env, id)) return tree->alpha;
    return 0;
}
jboolean SetVectorAlpha(JNIEnv* env, jclass, jlong id, jfloat alpha) {
    auto* tree = GetVectorTree(env, id);
    if (!tree || !std::isfinite(alpha)) return false;
    alpha = std::clamp(alpha, 0.f, 1.f);
    return std::exchange(tree->alpha, alpha) != alpha;
}
void SetVectorAntialias(JNIEnv* env, jclass, jlong id, jboolean enabled) {
    if (auto* tree = GetVectorTree(env, id)) {
        tree->antialias = enabled;
        tree->cache_generation = 0;
    }
}
void SetVectorCaching(JNIEnv* env, jclass, jlong id, jboolean enabled) {
    if (auto* tree = GetVectorTree(env, id)) tree->allow_caching = enabled;
}

jboolean GetVectorGroup(JNIEnv* env, jclass, jlong id, jfloatArray result, jint length) {
    const auto* node = GetVectorNode(env, id, VectorKind::Group);
    if (!node || !result || length != node->transform.size() ||
        env->GetArrayLength(result) < length) return false;
    env->SetFloatArrayRegion(result, 0, length, node->transform.data());
    return !env->ExceptionCheck();
}
void SetVectorGroup(JNIEnv* env, jclass, jlong id, jfloat rotation, jfloat pivot_x,
                    jfloat pivot_y, jfloat scale_x, jfloat scale_y, jfloat translate_x, jfloat translate_y) {
    if (auto* node = GetVectorNode(env, id, VectorKind::Group)) {
        node->transform = {rotation, pivot_x, pivot_y, scale_x, scale_y, translate_x, translate_y};
        node->UpdateMatrix();
    }
}
template <size_t Index>
jfloat GetVectorTransform(JNIEnv* env, jclass, jlong id) {
    const auto* group = GetVectorNode(env, id, VectorKind::Group);
    return group ? group->transform[Index] : 0;
}
template <size_t Index>
void SetVectorTransform(JNIEnv* env, jclass, jlong id, jfloat value) {
    if (auto* group = GetVectorNode(env, id, VectorKind::Group);
        group && std::exchange(group->transform[Index], value) != value) group->UpdateMatrix();
}

jboolean GetVectorStyle(JNIEnv* env, jclass, jlong id, jbyteArray out, jint length) {
    const auto* node = GetVectorNode(env, id, VectorKind::FullPath);
    if (!node || !out || length != 48 || env->GetArrayLength(out) < length) return false;
    const auto& s = node->style;
    const std::array<uint32_t, 12> words{
        std::bit_cast<uint32_t>(s.stroke_width), uint32_t(s.stroke_color),
        std::bit_cast<uint32_t>(s.stroke_alpha), uint32_t(s.fill_color), std::bit_cast<uint32_t>(s.fill_alpha),
        std::bit_cast<uint32_t>(s.trim_start), std::bit_cast<uint32_t>(s.trim_end),
        std::bit_cast<uint32_t>(s.trim_offset), uint32_t(s.cap), uint32_t(s.join),
        std::bit_cast<uint32_t>(s.miter), uint32_t(s.fill_type)};
    std::array<jbyte, 48> bytes;
    for (size_t i = 0; i < words.size(); ++i)
        for (unsigned b = 0; b < 4; ++b) bytes[4 * i + b] = static_cast<jbyte>(words[i] >> (8 * b));
    env->SetByteArrayRegion(out, 0, length, bytes.data());
    return !env->ExceptionCheck();
}

void SetVectorStyle(JNIEnv* env, jclass, jlong id, jfloat width, jint stroke, jfloat stroke_alpha,
                    jint fill, jfloat fill_alpha, jfloat trim_start, jfloat trim_end, jfloat trim_offset,
                    jfloat miter, jint cap, jint join, jint fill_type) {
    if (auto* node = GetVectorNode(env, id, VectorKind::FullPath)) {
        node->style = {width, stroke, stroke_alpha, fill, fill_alpha, trim_start, trim_end,
                       trim_offset, cap, join, miter, fill_type};
        node->path.fill = fill_type == 1 ? WA_PATH_EVEN_ODD : WA_PATH_NONZERO;
        node->Invalidate();
    }
}
template <auto Field>
auto GetVectorStyleProperty(JNIEnv* env, jclass, jlong id) {
    const auto* path = GetVectorNode(env, id, VectorKind::FullPath);
    using Value = decltype(path->style.*Field);
    return path ? path->style.*Field : std::remove_cvref_t<Value>{};
}
template <auto Field, class T>
void SetVectorStyleProperty(JNIEnv* env, jclass, jlong id, T value) {
    if (auto* path = GetVectorNode(env, id, VectorKind::FullPath);
        path && std::exchange(path->style.*Field, value) != value) path->Invalidate();
}
template <auto Field>
void SetVectorGradient(JNIEnv* env, jclass, jlong id, jlong shader) {
    CallSafely(env, [&] {
        auto* path = GetVectorNode(env, id, VectorKind::FullPath);
        if (!path) return;
        if (shader) {
            const auto peer = GetShader(env, shader);
            const auto* gradient = peer ? std::get_if<GradientData>(&peer->content) : nullptr;
            if (!gradient) return;
            path->*Field = *gradient;
        } else {
            (path->*Field).reset();
        }
        path->Invalidate();
    });
}
void SetVectorPath(VectorNode& node, std::optional<PathGeometry> geometry) {
    if (geometry) {
        geometry->fill = node.path.fill;
        node.path = std::move(*geometry);
        node.Invalidate();
    }
}
void SetVectorPathString(JNIEnv* env, jclass, jlong id, jstring path, jint) {
    CallSafely(env, [&] {
        if (const auto* node = GetVectorNode(env, id)) SetVectorPath(**node, ParseSvgPath(env, path));
    });
}
void SetVectorPathData(JNIEnv* env, jclass, jlong id, jlong data) {
    CallSafely(env, [&] {
        if (const auto* node = GetVectorNode(env, id)) SetVectorPath(**node, PathFromSvgData(env, data));
    });
}

float VectorOpacity(float alpha) { return std::isfinite(alpha) ? std::clamp(alpha, 0.f, 1.f) : 0; }

bool HasVectorFill(const VectorNode& path) {
    return path.style.fill_alpha > 0 && (path.fill_gradient || uint32_t(path.style.fill_color) >> 24);
}
bool HasVectorStroke(const VectorNode& path) {
    return path.style.stroke_width > 0 && path.style.stroke_alpha > 0 &&
        (path.stroke_gradient || uint32_t(path.style.stroke_color) >> 24);
}
void InspectVector(VectorTree& tree, const VectorNode& node) {
    if (node.kind == VectorKind::Group) {
        for (const auto& child : node.children) InspectVector(tree, *child);
    } else if (node.kind == VectorKind::ClipPath) {
        tree.needs_raster = true;
    } else {
        const bool stroke = HasVectorStroke(node);
        tree.paint_count += HasVectorFill(node) + stroke;
        tree.needs_raster |= bool(node.fill_gradient) || (stroke &&
            (node.stroke_gradient || node.style.cap != 0 || node.style.join != 0 || node.style.miter != 4));
    }
}

PathGeometry TrimVectorPath(const VectorNode& node) {
    namespace raster = interp::software_raster;
    auto path = node.path;
    path.fill = node.style.fill_type == 1 ? WA_PATH_EVEN_ODD : WA_PATH_NONZERO;
    if (node.style.trim_start == 0 && node.style.trim_end == 1) return path;

    std::vector<Value> commands;
    commands.reserve(path.verbs.size() * 7);
    for (const auto& verb : path.verbs) {
        commands.push_back(Value::Float(verb.command));
        for (float point : verb.points) commands.push_back(Value::Float(point));
    }
    const auto contours = raster::Flatten(commands, path.verbs.size());
    if (contours.empty() || contours.front().points.size() < 2) return {};
    auto points = contours.front().points;
    if (contours.front().closed) points.push_back(points.front());
    std::vector<double> lengths(points.size(), 0);
    for (size_t i = 1; i < points.size(); ++i)
        lengths[i] = lengths[i - 1] + std::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y);
    const double length = lengths.back();
    const auto distance = [&](float part) {
        const double offset = part + node.style.trim_offset;
        return (offset - std::floor(offset)) * length;
    };
    const double start = distance(node.style.trim_start), end = distance(node.style.trim_end);
    path.verbs.clear();
    const auto segment = [&](double from, double to) {
        bool first = true;
        for (size_t i = 1; i < points.size() && lengths[i - 1] < to; ++i) {
            if (lengths[i] <= from || lengths[i] == lengths[i - 1]) continue;
            const auto at = [&](double distance) {
                return points[i - 1] + (points[i] - points[i - 1]) *
                    ((distance - lengths[i - 1]) / (lengths[i] - lengths[i - 1]));
            };
            if (first) {
                const auto p = at(std::max(from, lengths[i - 1]));
                path.Move(p.x, p.y);
                first = false;
            }
            const auto p = at(std::min(to, lengths[i]));
            path.Line(p.x, p.y);
        }
    };
    if (start < end) segment(start, end);
    if (start > end) {
        segment(start, length);
        segment(0, end);
    }
    return path;
}

// The display list can preserve solid paths directly. Complex paint and clip
// paths use the same software path renderer as a Canvas backed by a Bitmap.
void DrawVectorPath(const CanvasTarget& canvas, const VectorNode& node, jlong filter,
                    std::span<const uint8_t> clip = {}) {
    if (!HasVectorFill(node) && !HasVectorStroke(node)) return;
    std::optional<PathGeometry> adjusted;
    if (node.style.trim_start != 0 || node.style.trim_end != 1) adjusted = TrimVectorPath(node);
    const auto& geometry = adjusted ? *adjusted : node.path;
    if (geometry.Empty()) return;
    PaintData paint;
    paint.color_filter = filter;
    const auto draw = [&](uint32_t color, float alpha, const std::optional<GradientData>& gradient) {
        paint.color = Opacity(gradient ? 0xffffffffu : color, VectorOpacity(alpha));
        if (canvas.bitmap) RasterizePath(canvas, geometry, paint, gradient ? &*gradient : nullptr, clip);
        else EmitPath(canvas, geometry, paint);
    };
    if (HasVectorFill(node)) draw(node.style.fill_color, node.style.fill_alpha, node.fill_gradient);
    if (HasVectorStroke(node)) {
        paint.style = 1;
        paint.stroke_width = node.style.stroke_width;
        paint.cap = node.style.cap;
        paint.join = node.style.join;
        paint.miter = node.style.miter;
        draw(node.style.stroke_color, node.style.stroke_alpha, node.stroke_gradient);
    }
}

class VectorCanvasScope {
public:
    explicit VectorCanvasScope(const CanvasTarget& canvas)
        : canvas_(canvas), frame_(canvas.data->frame) {
        if (canvas_.display && canvas_.state->result == WA_SF_OK) {
            canvas_.state->result = wa_canvas_save(canvas_.display);
            saved_ = canvas_.state->result == WA_SF_OK;
        }
    }
    ~VectorCanvasScope() {
        canvas_.data->frame = std::move(frame_);
        if (saved_) {
            const auto result = wa_canvas_restore(canvas_.display);
            if (canvas_.state->result == WA_SF_OK) canvas_.state->result = result;
        }
    }

private:
    const CanvasTarget& canvas_;
    CanvasData::Frame frame_;
    bool saved_ = false;
};

void RecordVectorGroup(JNIEnv* env, const CanvasTarget& canvas, const VectorNode& group, jlong filter) {
    const auto& matrix = group.group_matrix;
    std::optional<VectorCanvasScope> scope;
    if (matrix != kIdentity) {
        scope.emplace(canvas);
        ConcatCanvas(canvas, matrix);
    }
    for (const auto& child : group.children) {
        if (canvas.state->result != WA_SF_OK) return;
        if (child->kind == VectorKind::Group) RecordVectorGroup(env, canvas, *child, filter);
        else DrawVectorPath(canvas, *child, filter);
    }
}

void RasterVectorGroup(const CanvasTarget& canvas, const VectorNode& group,
                       std::span<const uint8_t> clip = {}) {
    const auto previous = canvas.data->frame.matrix;
    canvas.data->frame.matrix = Multiply(previous, group.group_matrix);
    std::vector<uint8_t> mask;
    for (const auto& child : group.children) {
        if (child->kind == VectorKind::Group) {
            RasterVectorGroup(canvas, *child, clip);
        } else if (child->kind == VectorKind::ClipPath) {
            const auto transform = RasterTransform(canvas.data->frame);
            auto next = RasterizeCoverage(child->path, PaintData{}, transform.elements,
                                          {0, 0, canvas.bitmap->width, canvas.bitmap->height});
            if (!clip.empty()) {
                for (size_t i = 0; i < next.size(); ++i)
                    next[i] = (unsigned(next[i]) * clip[i] + 127) / 255;
            }
            mask = std::move(next);
            clip = mask;
        } else {
            DrawVectorPath(canvas, *child, 0, clip);
        }
    }
    canvas.data->frame.matrix = previous;
}

wa_rect_t VectorBounds(JNIEnv* env, State& state, jobject bounds) {
    auto& fields = state.vector_bounds_fields;
    if (!fields[0]) {
        const auto type = env->GetObjectClass(bounds);
        const std::array names{"left", "top", "right", "bottom"};
        for (size_t i = 0; i < names.size(); ++i) fields[i] = env->GetFieldID(type, names[i], "I");
        if (env->ExceptionCheck()) return {};
    }
    const auto read = [&](size_t i) { return static_cast<float>(env->GetIntField(bounds, fields[i])); };
    return {read(0), read(1), read(2), read(3)};
}

void RasterizeVector(JNIEnv* env, const CanvasTarget& destination, VectorTree& tree, jlong id,
                     jlong filter, wa_rect_t bounds, bool mirrored, bool reuse) {
    auto& state = *destination.state;
    auto& cache = tree.cache;
    const auto& matrix = destination.data->frame.matrix;
    const float aa = tree.antialias ? 2.f : 1.f;
    const auto size = [aa](float extent) {
        return static_cast<jint>(std::clamp(std::ceil(extent * aa), 1.f, 2048.f));
    };
    const jint width = size((bounds.r - bounds.l) * std::hypot(matrix[0], matrix[3]));
    const jint height = size((bounds.b - bounds.t) * std::hypot(matrix[1], matrix[4]));
    if (!tree.allow_caching || !reuse || cache.width != width || cache.height != height ||
        tree.cached_filter != filter || tree.cache_generation != tree.root->revision) {
        cache.width = width;
        cache.height = height;
        cache.pixels.assign(size_t(width) * height, 0);
        CanvasData data;
        data.width = width;
        data.height = height;
        data.Reset();
        data.frame.matrix = Scaling(width / tree.width, height / tree.height);
        RasterVectorGroup(CanvasTarget{&state, &data, &cache}, *tree.root);
        if (env->ExceptionCheck()) return;
        if (filter) {
            PaintData paint;
            paint.color_filter = filter;
            for (auto& pixel : cache.pixels) pixel = FilterColor(paint, pixel);
        }
        cache.Changed();
        tree.cached_filter = filter;
        tree.cache_generation = tree.root->revision;
    }
    VectorCanvasScope scope(destination);
    if (mirrored) ConcatCanvas(destination, {-1, 0, bounds.l + bounds.r, 0, 1, 0, 0, 0, 1});
    PaintData paint;
    paint.color = Opacity(0xffffffffu, tree.alpha);
    paint.flags = tree.antialias ? 2 : 0;
    PaintBitmap(destination, cache, std::numeric_limits<jlong>::min() + id,
                {0, 0, float(width), float(height)}, bounds, paint);
}

jint DrawVector(JNIEnv* env, jclass, jlong id, jlong canvas_id, jlong filter, jobject rect,
                jboolean mirrored, jboolean reuse) {
    return CallSafely(env, [&]() -> jint {
        auto* tree = GetVectorTree(env, id);
        const auto canvas = Canvas(env, canvas_id);
        if (!tree || !canvas || (!canvas.bitmap && !canvas.display) ||
            !(tree->width > 0 && tree->height > 0 && tree->alpha > 0)) return 0;
        const auto bounds = VectorBounds(env, *canvas.state, rect);
        if (!HasArea(Intersect(canvas.data->frame.clip, MapRect(canvas.data->frame.matrix, bounds)))) return 0;

        const auto generation = tree->root->revision;
        if (tree->analyzed_generation != generation) {
            tree->paint_count = 0;
            tree->needs_raster = false;
            InspectVector(*tree, *tree->root);
            tree->analyzed_generation = generation;
        }
        if (!tree->paint_count) return 0;
        const uint32_t mode = (static_cast<uint64_t>(filter) >> 32) - 1;
        const bool alpha_filter = !filter || mode == 5 || mode == 9;
        // An opaque SRC_IN tint preserves each path's coverage. Source-over
        // compositing of the tinted paths yields the same combined color.
        const bool needs_group_opacity = tree->alpha != 1 ||
            (mode == 5 && static_cast<uint32_t>(filter) >> 24 != 255);
        if (canvas.bitmap || tree->needs_raster || !tree->antialias || !alpha_filter ||
            (tree->paint_count > 1 && needs_group_opacity)) {
            RasterizeVector(env, canvas, *tree, id, filter, bounds, mirrored, reuse);
            return tree->cache.width * tree->cache.height;
        }

        const float sx = (bounds.r - bounds.l) / tree->width, sy = (bounds.b - bounds.t) / tree->height;
        VectorCanvasScope scope(canvas);
        ClipCanvas(canvas, {bounds, 0, 0});
        ConcatCanvas(canvas, {mirrored ? -sx : sx, 0, mirrored ? bounds.r : bounds.l,
                              0, sy, bounds.t, 0, 0, 1});
        canvas.data->frame.alpha *= tree->alpha;
        RecordVectorGroup(env, canvas, *tree->root, filter);
        return 0;
    });
}

} // namespace

void RegisterVectors(interp::Vm& vm) {
    constexpr auto cls = "android/graphics/drawable/VectorDrawable";
    if (!vm.HasClass("Landroid/graphics/drawable/VectorDrawable;")) return;
    Register(vm, cls,
        Method<RetainPeer<VectorNodes, &State::vector_nodes>>(vm, "nRegisterNode",
            "(Landroid/graphics/drawable/VectorDrawable$VObject;J)V"),
        Method<RetainPeer<VectorTree, &State::vector_trees>>(vm, "nRegisterTree",
            "(Landroid/graphics/drawable/VectorDrawable$NativeTree;J)V"),
        Method<ReleaseVectorTree>(vm, "nReleaseTree", "(J)V"),
        Method<DrawVector>(vm, "nDraw", "(JJJLandroid/graphics/Rect;ZZ)I"),
        Method<CreateVectorTree>(vm, "nCreateTree", "(J)J"),
        Method<CreateVectorTreeCopy>(vm, "nCreateTreeFromCopy", "(JJ)J"),
        Method<SetVectorViewport>(vm, "nSetRendererViewportSize", "(JFF)V"),
        Method<GetVectorAlpha>(vm, "nGetRootAlpha", "(J)F"),
        Method<SetVectorAlpha>(vm, "nSetRootAlpha", "(JF)Z"),
        Method<SetVectorAntialias>(vm, "nSetAntiAlias", "(JZ)V"),
        Method<SetVectorCaching>(vm, "nSetAllowCaching", "(JZ)V"),
        Method<CreateVectorNode<VectorKind::FullPath>>(vm, "nCreateFullPath", "()J"),
        Method<CreateVectorNodeCopy<VectorKind::FullPath>>(vm, "nCreateFullPath", "(J)J"),
        Method<CreateVectorNode<VectorKind::ClipPath>>(vm, "nCreateClipPath", "()J"),
        Method<CreateVectorNodeCopy<VectorKind::ClipPath>>(vm, "nCreateClipPath", "(J)J"),
        Method<CreateVectorNode<VectorKind::Group>>(vm, "nCreateGroup", "()J"),
        Method<CreateVectorNodeCopy<VectorKind::Group>>(vm, "nCreateGroup", "(J)J"),
        Method<AddVectorChild>(vm, "nAddChild", "(JJ)V"),
        Method<SetVectorName>(vm, "nSetName", "(JLjava/lang/String;)V"),
        Method<SetVectorPathString>(vm, "nSetPathString", "(JLjava/lang/String;I)V"),
        Method<SetVectorPathData>(vm, "nSetPathData", "(JJ)V"),
        Method<GetVectorGroup>(vm, "nGetGroupProperties", "(J[FI)Z"),
        Method<SetVectorGroup>(vm, "nUpdateGroupProperties", "(JFFFFFFF)V"),
        Method<GetVectorStyle>(vm, "nGetFullPathProperties", "(J[BI)Z"),
        Method<SetVectorStyle>(vm, "nUpdateFullPathProperties", "(JFIFIFFFFFIII)V"),
        Method<SetVectorGradient<&VectorNode::fill_gradient>>(vm, "nUpdateFullPathFillGradient", "(JJ)V"),
        Method<SetVectorGradient<&VectorNode::stroke_gradient>>(vm, "nUpdateFullPathStrokeGradient", "(JJ)V"),
        Method<GetVectorTransform<0>>(vm, "nGetRotation", "(J)F"),
        Method<SetVectorTransform<0>>(vm, "nSetRotation", "(JF)V"),
        Method<GetVectorTransform<1>>(vm, "nGetPivotX", "(J)F"),
        Method<SetVectorTransform<1>>(vm, "nSetPivotX", "(JF)V"),
        Method<GetVectorTransform<2>>(vm, "nGetPivotY", "(J)F"),
        Method<SetVectorTransform<2>>(vm, "nSetPivotY", "(JF)V"),
        Method<GetVectorTransform<3>>(vm, "nGetScaleX", "(J)F"),
        Method<SetVectorTransform<3>>(vm, "nSetScaleX", "(JF)V"),
        Method<GetVectorTransform<4>>(vm, "nGetScaleY", "(J)F"),
        Method<SetVectorTransform<4>>(vm, "nSetScaleY", "(JF)V"),
        Method<GetVectorTransform<5>>(vm, "nGetTranslateX", "(J)F"),
        Method<SetVectorTransform<5>>(vm, "nSetTranslateX", "(JF)V"),
        Method<GetVectorTransform<6>>(vm, "nGetTranslateY", "(J)F"),
        Method<SetVectorTransform<6>>(vm, "nSetTranslateY", "(JF)V"),
        Method<GetVectorStyleProperty<&VectorNode::Style::stroke_width>>(vm, "nGetStrokeWidth", "(J)F"),
        Method<SetVectorStyleProperty<&VectorNode::Style::stroke_width, jfloat>>(vm, "nSetStrokeWidth", "(JF)V"),
        Method<GetVectorStyleProperty<&VectorNode::Style::stroke_color>>(vm, "nGetStrokeColor", "(J)I"),
        Method<SetVectorStyleProperty<&VectorNode::Style::stroke_color, jint>>(vm, "nSetStrokeColor", "(JI)V"),
        Method<GetVectorStyleProperty<&VectorNode::Style::stroke_alpha>>(vm, "nGetStrokeAlpha", "(J)F"),
        Method<SetVectorStyleProperty<&VectorNode::Style::stroke_alpha, jfloat>>(vm, "nSetStrokeAlpha", "(JF)V"),
        Method<GetVectorStyleProperty<&VectorNode::Style::fill_color>>(vm, "nGetFillColor", "(J)I"),
        Method<SetVectorStyleProperty<&VectorNode::Style::fill_color, jint>>(vm, "nSetFillColor", "(JI)V"),
        Method<GetVectorStyleProperty<&VectorNode::Style::fill_alpha>>(vm, "nGetFillAlpha", "(J)F"),
        Method<SetVectorStyleProperty<&VectorNode::Style::fill_alpha, jfloat>>(vm, "nSetFillAlpha", "(JF)V"),
        Method<GetVectorStyleProperty<&VectorNode::Style::trim_start>>(vm, "nGetTrimPathStart", "(J)F"),
        Method<SetVectorStyleProperty<&VectorNode::Style::trim_start, jfloat>>(vm, "nSetTrimPathStart", "(JF)V"),
        Method<GetVectorStyleProperty<&VectorNode::Style::trim_end>>(vm, "nGetTrimPathEnd", "(J)F"),
        Method<SetVectorStyleProperty<&VectorNode::Style::trim_end, jfloat>>(vm, "nSetTrimPathEnd", "(JF)V"),
        Method<GetVectorStyleProperty<&VectorNode::Style::trim_offset>>(vm, "nGetTrimPathOffset", "(J)F"),
        Method<SetVectorStyleProperty<&VectorNode::Style::trim_offset, jfloat>>(vm, "nSetTrimPathOffset", "(JF)V"));
}
