struct RegionIteratorData {
    std::vector<RegionRect> rectangles;
    size_t index = 0;
};

namespace {

RegionData RectRegion(RegionRect rect) {
    RegionData region;
    if (rect.HasArea()) region.rects.push_back(rect);
    return region;
}

RegionData* GetRegion(JNIEnv* env, jlong id) {
    return Peer<RegionData, &State::regions>(env, id);
}

RegionData* GetRegion(JNIEnv* env, jobject object) {
    const auto type = env->GetObjectClass(object);
    if (!type) return nullptr;
    const auto field = env->GetFieldID(type, "mNativeRegion", "J");
    env->DeleteLocalRef(type);
    return field ? GetRegion(env, env->GetLongField(object, field)) : nullptr;
}

// The input rectangles are ordered into contiguous bands. At a band break,
// sweep the horizontal edges to compute all six SkRegion operations.
RegionData CombineRegions(const RegionData& a, const RegionData& b, jint op) {
    if (op == 5) return b;
    constexpr std::array<unsigned, 5> masks{0b0010, 0b1000, 0b1110, 0b0110, 0b0100};
    if (op < 0 || op >= jint(masks.size())) throw std::invalid_argument("Invalid Region.Op");
    const auto mask = masks[op];
    std::array<size_t, 2> at{};
    const std::array regions{std::span(a.rects), std::span(b.rects)};
    using Event = std::pair<jint, unsigned>;
    std::vector<Event> events;
    std::vector<RegionSpan> spans;
    RegionData result;
    size_t previous = 0;
    if (a.rects.empty() && b.rects.empty()) return result;
    jint y = std::min(a.rects.empty() ? INT32_MAX : a.rects.front().top,
                      b.rects.empty() ? INT32_MAX : b.rects.front().top);
    while (y < INT32_MAX) {
        jint next_y = INT32_MAX;
        events.clear();
        for (size_t i = 0; i < regions.size(); ++i) {
            const auto rects = regions[i];
            while (at[i] < rects.size() && rects[at[i]].bottom <= y) ++at[i];
            for (size_t index = at[i]; index < rects.size(); ++index) {
                const auto& rect = rects[index];
                if (rect.top > y) {
                    next_y = std::min(next_y, rect.top);
                    break;
                }
                next_y = std::min(next_y, rect.bottom);
                events.emplace_back(rect.left, 1u << i);
                events.emplace_back(rect.right, 1u << i);
            }
        }
        std::ranges::sort(events);
        spans.clear();
        unsigned state = 0;
        jint left = 0;
        for (size_t index = 0; index < events.size();) {
            const jint x = events[index].first;
            const bool inside = mask & (1u << state);
            do {
                state ^= events[index++].second;
            } while (index < events.size() && events[index].first == x);
            const bool entered = mask & (1u << state);
            if (!inside && entered) left = x;
            if (inside && !entered) spans.emplace_back(left, x);
        }
        AddRegionBand(result, y, next_y, spans, previous);
        y = next_y;
    }
    return result;
}

bool RegionsOverlap(RegionRect a, RegionRect b) {
    return std::max(a.left, b.left) < std::min(a.right, b.right) &&
           std::max(a.top, b.top) < std::min(a.bottom, b.bottom);
}

constexpr wa_rect_t PathRect(RegionRect rect) {
    return {float(rect.left), float(rect.top), float(rect.right), float(rect.bottom)};
}

PathGeometry RegionPath(const RegionData& region) {
    PathGeometry path;
    if (region.rects.size() == 1) {
        path.Rect(PathRect(region.rects.front()));
        return path;
    }
    using Vertex = std::pair<jint, jint>;
    using Event = std::pair<jint, int>;
    std::map<jint, std::vector<Event>> horizontal, vertical;
    const auto add = [](auto& lines, jint cross, jint from, jint to, int sign) {
        lines[cross].emplace_back(from, sign);
        lines[cross].emplace_back(to, -sign);
    };
    for (const auto& r : region.rects) {
        add(horizontal, r.top, r.left, r.right, 1);
        add(horizontal, r.bottom, r.left, r.right, -1);
        add(vertical, r.right, r.top, r.bottom, 1);
        add(vertical, r.left, r.top, r.bottom, -1);
    }
    std::map<Vertex, std::vector<Vertex>> edges;
    const auto emit = [&](auto& lines, bool rows) {
        for (auto& [cross, events] : lines) {
            std::ranges::sort(events);
            int state = 0;
            jint previous = events.front().first;
            for (const auto& [position, sign] : events) {
                if (position != previous && state) {
                    Vertex from = rows ? Vertex{previous, cross} : Vertex{cross, previous};
                    Vertex to = rows ? Vertex{position, cross} : Vertex{cross, position};
                    if (state < 0) std::swap(from, to);
                    edges[from].push_back(to);
                }
                state += sign;
                previous = position;
            }
        }
    };
    emit(horizontal, true);
    emit(vertical, false);
    while (!edges.empty()) {
        const auto start = edges.begin()->first;
        auto current = start;
        path.Move(start.first, start.second);
        do {
            auto edge = edges.find(current);
            if (edge == edges.end()) break;
            current = edge->second.back();
            edge->second.pop_back();
            if (edge->second.empty()) edges.erase(edge);
            if (current != start) path.Line(current.first, current.second);
        } while (current != start);
        path.Close();
    }
    return path;
}

jlong NewRegion(JNIEnv* env, jclass) {
    return CallSafely(env, [&] { return StoreRegion(env); });
}
void DeleteRegion(JNIEnv* env, jclass, jlong handle) {
    if (auto* state = GetState(env); state && state->regions) state->regions->Erase(handle);
}
void CopyRegion(JNIEnv* env, jclass, jlong dst, jlong src) {
    CallSafely(env, [&] {
        auto* target = GetRegion(env, dst);
        const auto* source = GetRegion(env, src);
        if (target && source) *target = *source;
    });
}
jboolean SetRegionRect(JNIEnv* env, jclass, jlong dst, jint l, jint t, jint r, jint b) {
    return CallSafely(env, [&]() -> jboolean {
        auto* region = GetRegion(env, dst);
        if (!region) return false;
        *region = RectRegion({l, t, r, b});
        return !region->rects.empty();
    });
}

jint RegionCoordinate(double value) {
    return static_cast<jint>(std::clamp(value, double(INT32_MIN), double(INT32_MAX)));
}

RegionData ScanRegionPath(const PathGeometry& path, const RegionData& clip) {
    using namespace interp::software_raster;
    if (clip.rects.empty()) return {};
    if (path.clip && path.clip->rx == 0 && path.clip->ry == 0 && path.fill < 2) {
        const auto r = path.clip->rect;
        return CombineRegions(RectRegion({RegionCoordinate(std::ceil(r.l - .5)),
                                          RegionCoordinate(std::ceil(r.t - .5)),
                                          RegionCoordinate(std::ceil(r.r - .5)),
                                          RegionCoordinate(std::ceil(r.b - .5))}), clip, 1);
    }
    std::vector<Edge> edges;
    for (const auto& contour : Flatten(RasterPathValues(path), path.verbs.size()))
        AppendEdges(edges, contour.points, {}, false);
    const auto bounds = path.Bounds();
    const auto top = std::max(clip.rects.front().top, RegionCoordinate(std::floor(bounds.t)));
    const auto bottom = std::min(clip.rects.back().bottom, RegionCoordinate(std::ceil(bounds.b)));
    RegionData runs;
    std::vector<std::pair<double, int>> crossings;
    std::vector<RegionSpan> spans;
    size_t previous = 0;
    for (jint y = top; y < bottom; ++y) {
        crossings.clear();
        const double scan = double(y) + 0.5;
        for (const auto& [from, to] : edges) {
            if (scan < std::min(from.y, to.y) || scan >= std::max(from.y, to.y)) continue;
            const double x = from.x + (to.x - from.x) * (scan - from.y) / (to.y - from.y);
            crossings.emplace_back(x, from.y < to.y ? 1 : -1);
        }
        std::ranges::sort(crossings);
        spans.clear();
        int winding = 0;
        jint left = 0;
        for (size_t at = 0; at < crossings.size();) {
            const auto x = RegionCoordinate(std::ceil(crossings[at].first - .5));
            const bool inside = path.fill & 1 ? winding % 2 != 0 : winding != 0;
            do {
                winding += crossings[at++].second;
            } while (at < crossings.size() && RegionCoordinate(std::ceil(crossings[at].first - .5)) == x);
            const bool entered = path.fill & 1 ? winding % 2 != 0 : winding != 0;
            if (inside && !entered) spans.emplace_back(left, x);
            if (!inside && entered) left = x;
        }
        AddRegionBand(runs, y, y + 1, spans, previous);
    }
    return CombineRegions(clip, runs, path.fill >= 2 ? 0 : 1);
}

jboolean SetRegionPath(JNIEnv* env, jclass, jlong dst, jlong geometry, jlong clip) {
    return CallSafely(env, [&]() -> jboolean {
        auto* target = GetRegion(env, dst);
        const auto* path = Path(env, geometry);
        const auto* clipping = GetRegion(env, clip);
        if (!target || !path || !clipping) return false;
        *target = ScanRegionPath(*path, *clipping);
        return !target->rects.empty();
    });
}
void WriteRegionRect(JNIEnv* env, jobject rect, RegionRect bounds) {
    const auto [l, t, r, b] = bounds;
    const auto type = env->GetObjectClass(rect);
    const auto set = type ? env->GetMethodID(type, "set", "(IIII)V") : nullptr;
    if (set) env->CallVoidMethod(rect, set, l, t, r, b);
    if (type) env->DeleteLocalRef(type);
}
jboolean GetRegionBounds(JNIEnv* env, jclass, jlong id, jobject rect) {
    const auto* region = GetRegion(env, id);
    if (!region) return false;
    WriteRegionRect(env, rect, region->Bounds());
    return !region->rects.empty();
}
jboolean GetRegionPath(JNIEnv* env, jclass, jlong id, jlong output) {
    return CallSafely(env, [&]() -> jboolean {
        const auto* region = GetRegion(env, id);
        auto* path = Path(env, output);
        if (!region || !path) return false;
        auto result = RegionPath(*region);
        result.generation = path->generation + 1;
        *path = std::move(result);
        return !region->rects.empty();
    });
}
jboolean RegionOpRect(JNIEnv* env, jclass, jlong id, jint l, jint t, jint r, jint b, jint op) {
    return CallSafely(env, [&]() -> jboolean {
        auto* region = GetRegion(env, id);
        if (!region) return false;
        *region = CombineRegions(*region, RectRegion({l, t, r, b}), op);
        return !region->rects.empty();
    });
}
jboolean RegionOpRectObject(JNIEnv* env, jclass, jlong id, jobject rect, jlong src, jint op) {
    return CallSafely(env, [&]() -> jboolean {
        auto* region = GetRegion(env, id);
        const auto* other = GetRegion(env, src);
        const auto r = ReadRegionRect(env, rect);
        if (!region || !other || env->ExceptionCheck()) return false;
        *region = CombineRegions(RectRegion(r), *other, op);
        return !region->rects.empty();
    });
}
jboolean RegionOp(JNIEnv* env, jclass, jlong id, jlong a, jlong b, jint op) {
    return CallSafely(env, [&]() -> jboolean {
        auto* region = GetRegion(env, id);
        const auto* first = GetRegion(env, a);
        const auto* second = GetRegion(env, b);
        if (!region || !first || !second) return false;
        *region = CombineRegions(*first, *second, op);
        return !region->rects.empty();
    });
}
jboolean EqualRegions(JNIEnv* env, jclass, jlong a, jlong b) {
    const auto* first = GetRegion(env, a);
    const auto* second = GetRegion(env, b);
    return first && second && first->rects == second->rects;
}
jboolean RegionContains(JNIEnv* env, jobject object, jint x, jint y) {
    const auto* region = GetRegion(env, object);
    return region && std::ranges::any_of(region->rects, [&](const auto& r) {
        return r.left <= x && x < r.right && r.top <= y && y < r.bottom;
    });
}
template <int Kind>
jboolean RegionIs(JNIEnv* env, jobject object) {
    const auto* region = GetRegion(env, object);
    if (!region) return false;
    if constexpr (Kind == 2) return region->rects.size() > 1;
    return region->rects.size() == Kind;
}
jboolean RegionQuickContains(JNIEnv* env, jobject object, jint l, jint t, jint r, jint b) {
    const auto* region = GetRegion(env, object);
    if (!region || region->rects.size() != 1) return false;
    const auto rect = region->rects.front();
    return l < r && t < b && l >= rect.left && t >= rect.top && r <= rect.right && b <= rect.bottom;
}
jboolean RegionQuickRejectRect(JNIEnv* env, jobject object, jint l, jint t, jint r, jint b) {
    const auto* region = GetRegion(env, object);
    return region && !RegionsOverlap(region->Bounds(), {l, t, r, b});
}
jboolean RegionQuickRejectRegion(JNIEnv* env, jobject object, jobject other) {
    const auto* region = GetRegion(env, object);
    const auto* src = GetRegion(env, other);
    return region && src && !RegionsOverlap(region->Bounds(), src->Bounds());
}

template <class Transform>
void TransformRegion(JNIEnv* env, jobject object, jobject output, Transform&& transform) {
    CallSafely(env, [&] {
        const auto* region = GetRegion(env, object);
        auto* dest = GetRegion(env, output ? output : object);
        if (!region || !dest) return;
        RegionData current;
        for (const auto& rect : region->rects) {
            const auto [x0, y0] = transform(rect.left, rect.top);
            const auto [x1, y1] = transform(rect.right, rect.bottom);
            current = CombineRegions(current, RectRegion({std::min(x0, x1), std::min(y0, y1),
                                                           std::max(x0, x1), std::max(y0, y1)}), 2);
        }
        *dest = std::move(current);
    });
}
void ScaleRegion(JNIEnv* env, jobject object, jfloat factor, jobject output) {
    if (!std::isfinite(factor)) return;
    TransformRegion(env, object, output, [&](jint x, jint y) {
        return std::pair{RegionCoordinate(std::floor(x * double(factor) + .5)),
                         RegionCoordinate(std::floor(y * double(factor) + .5))};
    });
}
void TranslateRegion(JNIEnv* env, jobject object, jint dx, jint dy, jobject output) {
    TransformRegion(env, object, output, [&](jint x, jint y) {
        return std::pair{RegionCoordinate(double(x) + dx), RegionCoordinate(double(y) + dy)};
    });
}
jstring RegionString(JNIEnv* env, jclass, jlong id) {
    return CallSafely(env, [&]() -> jstring {
        const auto* region = GetRegion(env, id);
        if (!region) return nullptr;
        std::string text = "SkRegion(";
        for (const auto& r : region->rects)
            text += "(" + std::to_string(r.left) + "," + std::to_string(r.top) + "," +
                    std::to_string(r.right) + "," + std::to_string(r.bottom) + ")";
        text += ")";
        return env->NewStringUTF(text.c_str());
    });
}
jboolean WriteRegionParcel(JNIEnv* env, jclass, jlong id, jobject parcel) {
    return CallSafely(env, [&]() -> jboolean {
        const auto* region = GetRegion(env, id);
        const auto type = env->GetObjectClass(parcel);
        if (!region || !type) return false;
        const auto write = env->GetMethodID(type, "writeIntArray", "([I)V");
        if (!write) return false;
        if (region->rects.size() > INT32_MAX / 4) throw std::bad_alloc();
        auto data = env->NewIntArray(region->rects.size() * 4);
        if (!data) return false;
        std::vector<jint> values;
        values.reserve(region->rects.size() * 4);
        for (const auto& r : region->rects)
            values.insert(values.end(), {r.left, r.top, r.right, r.bottom});
        env->SetIntArrayRegion(data, 0, values.size(), values.data());
        if (!env->ExceptionCheck()) env->CallVoidMethod(parcel, write, data);
        return !env->ExceptionCheck();
    });
}
jlong ReadRegionParcel(JNIEnv* env, jclass, jobject parcel) {
    return CallSafely(env, [&]() -> jlong {
        const auto type = env->GetObjectClass(parcel);
        const auto read = type ? env->GetMethodID(type, "createIntArray", "()[I") : nullptr;
        auto data = read ? static_cast<jintArray>(env->CallObjectMethod(parcel, read)) : nullptr;
        if (!data) return 0;
        const jint count = env->GetArrayLength(data);
        if (count % 4 != 0) {
            Throw(env, "android/os/BadParcelableException", "Invalid Region rectangle data");
            return 0;
        }
        std::vector<jint> values(count);
        env->GetIntArrayRegion(data, 0, count, values.data());
        if (env->ExceptionCheck()) return 0;
        RegionData result;
        for (jint index = 0; index < count; index += 4) {
            const auto* r = values.data() + index;
            result = CombineRegions(result, RectRegion({r[0], r[1], r[2], r[3]}), 2);
        }
        return StoreRegion(env, std::move(result));
    });
}

jlong OpenRegionIterator(JNIEnv* env, const RegionData& region, jobject owner = nullptr) {
    auto* state = GetState(env);
    if (!state) return 0;
    auto& store = Peers<RegionIteratorData, &State::region_iterators>(*state);
    const auto handle = store.Insert({region.rects});
    if (owner) store.Retain(env, owner, handle);
    return handle;
}
jlong NewRegionIterator(JNIEnv* env, jclass, jlong id) {
    return CallSafely(env, [&]() -> jlong {
        const auto* region = GetRegion(env, id);
        return region ? OpenRegionIterator(env, *region) : 0;
    });
}
void DeleteRegionIterator(JNIEnv* env, jclass, jlong id) {
    if (auto* state = GetState(env); state && state->region_iterators)
        state->region_iterators->Erase(id);
}
jboolean NextRegionRect(JNIEnv* env, jclass, jlong id, jobject output) {
    auto* iterator = Peer<RegionIteratorData, &State::region_iterators>(env, id);
    if (!iterator || iterator->index >= iterator->rectangles.size()) return false;
    WriteRegionRect(env, output, iterator->rectangles[iterator->index]);
    if (env->ExceptionCheck()) return false;
    ++iterator->index;
    return true;
}

void RegisterRegionIterator(interp::Vm& vm) {
    constexpr auto descriptor = "Landroid/graphics/RegionIterator;";
    if (vm.HasClass(descriptor)) {
        constexpr auto type = "android/graphics/RegionIterator";
        if (RegisterIfDeclared(vm, type, Method<NewRegionIterator>(vm, "nativeConstructor", "(J)J"))) {
            Register(vm, type,
                     Method<DeleteRegionIterator>(vm, "nativeDestructor", "(J)V"),
                     Method<NextRegionRect>(vm, "nativeNext", "(JLandroid/graphics/Rect;)Z"));
        }
        return;
    }
    // Launcher3's shape estimator also uses RegionIterator when the embedded
    // framework includes Region without its Java iterator.
    auto& linker = interp::JniBridge::From(vm.JniEnv()).Vm().linker;
    auto& type = linker.Define(descriptor, "Ljava/lang/Object;", art::kAccPublic);
    auto& handle = linker.AddField(type, "mNativeIter", "J", false);
    linker.AddMethod(type, "<init>", "(Landroid/graphics/Region;)V", art::kAccPublic,
        [&handle](interp::Runtime& runtime, interp::Object* self, std::span<const interp::Value> args) {
            auto& jni = *runtime.jni;
            auto* env = jni.Env();
            const auto source = jni.Local(args[0].ref());
            const auto owner = jni.Local(self);
            const auto id = CallSafely(env, [&]() -> jlong {
                const auto* region = GetRegion(env, source);
                return region ? OpenRegionIterator(env, *region, owner) : 0;
            });
            jni.DeleteLocal(owner);
            jni.DeleteLocal(source);
            self->fields[handle.slot] = interp::Value::Long(id);
            if (env->ExceptionCheck()) throw interp::ManagedException{};
            return interp::Value{};
        });
    linker.AddMethod(type, "next", "(Landroid/graphics/Rect;)Z", art::kAccPublic | art::kAccFinal,
        [&handle](interp::Runtime& runtime, interp::Object* self, std::span<const interp::Value> args) {
            auto& jni = *runtime.jni;
            auto* env = jni.Env();
            runtime.NonNull(args[0].ref());
            const auto rect = jni.Local(args[0].ref());
            const auto result = NextRegionRect(env, nullptr, self->fields[handle.slot].j(), rect);
            jni.DeleteLocal(rect);
            if (env->ExceptionCheck()) throw interp::ManagedException{};
            return interp::Value::Int(result);
        });
}

void RegisterRegions(interp::Vm& vm) {
    if (!vm.HasClass("Landroid/graphics/Region;")) return;
    Register(vm, "android/graphics/Region",
             Method<NewRegion>(vm, "nativeConstructor", "()J"),
             Method<DeleteRegion>(vm, "nativeDestructor", "(J)V"),
             Method<CopyRegion>(vm, "nativeSetRegion", "(JJ)V"),
             Method<SetRegionRect>(vm, "nativeSetRect", "(JIIII)Z"),
             Method<SetRegionPath>(vm, "nativeSetPath", "(JJJ)Z"),
             Method<GetRegionBounds>(vm, "nativeGetBounds", "(JLandroid/graphics/Rect;)Z"),
             Method<GetRegionPath>(vm, "nativeGetBoundaryPath", "(JJ)Z"),
             Method<RegionOpRect>(vm, "nativeOp", "(JIIIII)Z"),
             Method<RegionOpRectObject>(vm, "nativeOp", "(JLandroid/graphics/Rect;JI)Z"),
             Method<RegionOp>(vm, "nativeOp", "(JJJI)Z"),
             Method<EqualRegions>(vm, "nativeEquals", "(JJ)Z"),
             Method<ReadRegionParcel>(vm, "nativeCreateFromParcel", "(Landroid/os/Parcel;)J"),
             Method<WriteRegionParcel>(vm, "nativeWriteToParcel", "(JLandroid/os/Parcel;)Z"),
             Method<RegionString>(vm, "nativeToString", "(J)Ljava/lang/String;"),
             Method<RegionContains>(vm, "contains", "(II)Z"),
             Method<RegionIs<0>>(vm, "isEmpty", "()Z"),
             Method<RegionIs<1>>(vm, "isRect", "()Z"),
             Method<RegionIs<2>>(vm, "isComplex", "()Z"),
             Method<RegionQuickContains>(vm, "quickContains", "(IIII)Z"),
             Method<RegionQuickRejectRect>(vm, "quickReject", "(IIII)Z"),
             Method<RegionQuickRejectRegion>(vm, "quickReject", "(Landroid/graphics/Region;)Z"),
             Method<ScaleRegion>(vm, "scale", "(FLandroid/graphics/Region;)V"),
             Method<TranslateRegion>(vm, "translate", "(IILandroid/graphics/Region;)V"));
    RegisterRegionIterator(vm);
}

} // namespace
