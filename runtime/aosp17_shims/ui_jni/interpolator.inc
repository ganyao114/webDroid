struct KeyframeInterpolator {
    struct Keyframe {
        jint time = 0;
        std::vector<jfloat> values;
        std::array<float, 4> cubic{0, 0, 1, 1};
    };
    jint value_count;
    std::vector<Keyframe> frames;
    float repeat = 1;
    bool mirror = false;
};

struct NativeInterpolatorData {
    enum class Curve {
        Linear, Accelerate, Decelerate, AccelerateDecelerate, Anticipate,
        AnticipateOvershoot, Bounce, Cycle, Overshoot, Lut, Path
    };
    Curve curve;
    float parameter = 0;
    std::vector<float> x, y;
};

namespace {

std::optional<KeyframeInterpolator> InterpolatorWithSize(JNIEnv* env, jint values, jint frames) {
    if (values <= 0 || frames <= 0) {
        Throw(env, "java/lang/IllegalArgumentException", "Interpolator counts must be positive");
        return std::nullopt;
    }
    return KeyframeInterpolator{values, std::vector<KeyframeInterpolator::Keyframe>(frames)};
}
jlong NewInterpolator(JNIEnv* env, jclass, jint values, jint frames) {
    return CallSafely(env, [&]() -> jlong {
        auto* state = GetState(env);
        auto data = InterpolatorWithSize(env, values, frames);
        return state && data ? Peers<KeyframeInterpolator, &State::keyframe_interpolators>(*state)
                                   .Insert(std::move(*data)) : 0;
    });
}
void DeleteInterpolator(JNIEnv* env, jclass, jlong id) {
    if (auto* state = GetState(env); state && state->keyframe_interpolators)
        state->keyframe_interpolators->Erase(id);
}
KeyframeInterpolator* Interpolator(JNIEnv* env, jlong id) {
    return Peer<KeyframeInterpolator, &State::keyframe_interpolators>(env, id);
}
void ResetInterpolator(JNIEnv* env, jclass, jlong id, jint values, jint frames) {
    CallSafely(env, [&] {
        auto* interp = Interpolator(env, id);
        if (interp)
            if (auto data = InterpolatorWithSize(env, values, frames)) *interp = std::move(*data);
    });
}
void SetKeyframe(JNIEnv* env, jclass, jlong id, jint index, jint time,
                 jfloatArray values, jfloatArray blend) {
    CallSafely(env, [&] {
        auto* interp = Interpolator(env, id);
        if (!interp) return;
        auto& frames = interp->frames;
        if (index < 0 || static_cast<size_t>(index) >= frames.size()) {
            Throw(env, "java/lang/IndexOutOfBoundsException", "Interpolator frame index");
            return;
        }
        if ((index > 0 && !frames[index - 1].values.empty() && time <= frames[index - 1].time) ||
            (static_cast<size_t>(index + 1) < frames.size() && !frames[index + 1].values.empty() &&
             time >= frames[index + 1].time)) return;
        KeyframeInterpolator::Keyframe frame{time, std::vector<jfloat>(interp->value_count)};
        env->GetFloatArrayRegion(values, 0, interp->value_count, frame.values.data());
        if (blend && !env->ExceptionCheck()) env->GetFloatArrayRegion(blend, 0, 4, frame.cubic.data());
        if (!env->ExceptionCheck()) frames[index] = std::move(frame);
    });
}
void SetInterpolatorRepeat(JNIEnv* env, jclass, jlong id, jfloat repeat, jboolean mirror) {
    if (auto* interp = Interpolator(env, id); interp && repeat >= 0) {
        interp->repeat = std::min(repeat, 32000.f);
        interp->mirror = mirror;
    }
}
float CubicWeight(const std::array<float, 4>& cubic, double time) {
    if (cubic[0] == cubic[1] && cubic[2] == cubic[3]) return time;
    const auto coordinate = [](double t, float a, float b) {
        const double u = 1 - t;
        return t * (3 * u * (u * a + t * b) + t * t);
    };
    double low = 0, high = 1;
    for (int i = 0; i < 20; ++i) {
        const double mid = (low + high) / 2;
        if (coordinate(mid, cubic[0], cubic[2]) < time) low = mid;
        else high = mid;
    }
    return coordinate((low + high) / 2, cubic[1], cubic[3]);
}
jint SampleInterpolator(JNIEnv* env, jclass, jlong id, jint time_ms, jfloatArray output) {
    return CallSafely(env, [&]() -> jint {
        const auto* interp = Interpolator(env, id);
        if (!interp) return 1;
        const auto& frames = interp->frames;
        if (std::ranges::any_of(frames, [](const auto& f) { return f.values.empty(); })) return 1;
        const double first = frames.front().time, last = frames.back().time, duration = last - first;
        double time = first;
        jint status = time_ms < first ? 1 : 0;
        if (status == 0 && duration > 0) {
            const double elapsed = time_ms - first;
            const double active = duration * interp->repeat;
            if (elapsed > active) status = 2;
            const double iterations = std::min(elapsed, active) / duration;
            double cycle = std::floor(iterations);
            double position = iterations - cycle;
            if (position == 0 && cycle > 0 && elapsed >= active) {
                position = 1;
                --cycle;
            }
            if (interp->mirror && static_cast<int>(cycle) % 2) position = 1 - position;
            time += position * duration;
        } else if (time_ms > last) {
            status = 2;
        }
        if (!output) return status;
        const auto upper = std::ranges::upper_bound(frames, time, {}, &KeyframeInterpolator::Keyframe::time);
        if (upper == frames.end()) {
            env->SetFloatArrayRegion(output, 0, interp->value_count, frames.back().values.data());
        } else {
            const auto& lower = *(upper - 1);
            const auto blend = CubicWeight(lower.cubic, (time - lower.time) / (double(upper->time) - lower.time));
            std::vector<float> values(interp->value_count);
            for (size_t i = 0; i < values.size(); ++i) values[i] = std::lerp(lower.values[i], upper->values[i], blend);
            env->SetFloatArrayRegion(output, 0, values.size(), values.data());
        }
        return status;
    });
}

using Curve = NativeInterpolatorData::Curve;
jlong SaveNativeInterpolator(JNIEnv* env, NativeInterpolatorData data) {
    auto* state = GetState(env);
    return state ? Peers<NativeInterpolatorData, &State::native_interpolators>(*state)
                       .Insert(std::move(data)) : 0;
}
template <Curve Kind>
jlong SimpleNativeInterpolator(JNIEnv* env, jclass) {
    return CallSafely(env, [&] { return SaveNativeInterpolator(env, {Kind}); });
}
template <Curve Kind>
jlong ScalarNativeInterpolator(JNIEnv* env, jclass, jfloat parameter) {
    return CallSafely(env, [&] { return SaveNativeInterpolator(env, {Kind, parameter}); });
}
template <Curve Kind>
jlong TableNativeInterpolator(JNIEnv* env, jclass, jfloatArray points, jfloatArray values = nullptr) {
    return CallSafely(env, [&]() -> jlong {
        const jint count = env->GetArrayLength(points);
        if (env->ExceptionCheck() || count < 2) return 0;
        NativeInterpolatorData data{Kind};
        auto& first = values ? data.x : data.y;
        first.resize(count);
        env->GetFloatArrayRegion(points, 0, count, first.data());
        if (values && !env->ExceptionCheck()) {
            data.y.resize(count);
            env->GetFloatArrayRegion(values, 0, count, data.y.data());
        }
        return env->ExceptionCheck() ? 0 : SaveNativeInterpolator(env, std::move(data));
    });
}
jlong LutInterpolator(JNIEnv* env, jclass type, jfloatArray values) {
    return TableNativeInterpolator<Curve::Lut>(env, type, values);
}

void RegisterInterpolators(interp::Vm& vm) {
    if (vm.HasClass("Landroid/graphics/Interpolator;"))
        Register(vm, "android/graphics/Interpolator",
                 Method<NewInterpolator>(vm, "nativeConstructor", "(II)J"),
                 Method<DeleteInterpolator>(vm, "nativeDestructor", "(J)V"),
                 Method<ResetInterpolator>(vm, "nativeReset", "(JII)V"),
                 Method<SetKeyframe>(vm, "nativeSetKeyFrame", "(JII[F[F)V"),
                 Method<SetInterpolatorRepeat>(vm, "nativeSetRepeatMirror", "(JFZ)V"),
                 Method<SampleInterpolator>(vm, "nativeTimeToValues", "(JI[F)I"));
    if (vm.HasClass("Landroid/graphics/animation/NativeInterpolatorFactory;"))
        Register(vm, "android/graphics/animation/NativeInterpolatorFactory",
                 Method<SimpleNativeInterpolator<Curve::Linear>>(vm, "createLinearInterpolator", "()J"),
                 Method<SimpleNativeInterpolator<Curve::AccelerateDecelerate>>(vm, "createAccelerateDecelerateInterpolator", "()J"),
                 Method<SimpleNativeInterpolator<Curve::Bounce>>(vm, "createBounceInterpolator", "()J"),
                 Method<ScalarNativeInterpolator<Curve::Accelerate>>(vm, "createAccelerateInterpolator", "(F)J"),
                 Method<ScalarNativeInterpolator<Curve::Decelerate>>(vm, "createDecelerateInterpolator", "(F)J"),
                 Method<ScalarNativeInterpolator<Curve::Anticipate>>(vm, "createAnticipateInterpolator", "(F)J"),
                 Method<ScalarNativeInterpolator<Curve::AnticipateOvershoot>>(vm, "createAnticipateOvershootInterpolator", "(F)J"),
                 Method<ScalarNativeInterpolator<Curve::Cycle>>(vm, "createCycleInterpolator", "(F)J"),
                 Method<ScalarNativeInterpolator<Curve::Overshoot>>(vm, "createOvershootInterpolator", "(F)J"),
                 Method<LutInterpolator>(vm, "createLutInterpolator", "([F)J"),
                 Method<TableNativeInterpolator<Curve::Path>>(vm, "createPathInterpolator", "([F[F)J"));
}

} // namespace
