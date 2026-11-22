bool Affine(const Matrix9& m) { return m[6] == 0 && m[7] == 0 && m[8] == 1; }
bool RectStaysRect(const Matrix9& m) {
    return Affine(m) && ((m[1] == 0 && m[3] == 0) || (m[0] == 0 && m[4] == 0));
}
jboolean MatrixIsAffine(JNIEnv* env, jclass, jlong id) {
    const auto* m = MatrixValues(env, id);
    return m && Affine(*m);
}
jboolean MatrixRectStaysRect(JNIEnv* env, jclass, jlong id) {
    const auto* m = MatrixValues(env, id);
    return m && RectStaysRect(*m);
}
enum class MatrixOp { Set, Pre, Post };
template <MatrixOp Op> void ModifyMatrix(JNIEnv* env, jlong id, const Matrix9& rhs) {
    if (auto* m = MatrixValues(env, id)) {
        if constexpr (Op == MatrixOp::Set) *m = rhs;
        else if constexpr (Op == MatrixOp::Pre) *m = Multiply(*m, rhs);
        else *m = Multiply(rhs, *m);
    }
}
Matrix9 Degrees(float value) {
    const auto radians = value * (std::numbers::pi_v<double> / 180);
    const auto snap = [](double n) { return std::abs(n) < 1e-10 ? 0.f : static_cast<float>(n); };
    return Rotation(snap(std::sin(radians)), snap(std::cos(radians)));
}
template <MatrixOp Op, auto Function>
void MatrixPair(JNIEnv* env, jclass, jlong id, jfloat x, jfloat y) {
    ModifyMatrix<Op>(env, id, Function(x, y));
}
template <MatrixOp Op, auto Function>
void MatrixPivot(JNIEnv* env, jclass, jlong id, jfloat x, jfloat y, jfloat px, jfloat py) {
    ModifyMatrix<Op>(env, id, AtPivot(Function(x, y), px, py));
}
template <MatrixOp Op> void MatrixRotate(JNIEnv* env, jclass, jlong id, jfloat degrees) {
    ModifyMatrix<Op>(env, id, Degrees(degrees));
}
template <MatrixOp Op> void MatrixPivotRotate(JNIEnv* env, jclass, jlong id, jfloat degrees, jfloat x, jfloat y) {
    ModifyMatrix<Op>(env, id, AtPivot(Degrees(degrees), x, y));
}
void MatrixSinCos(JNIEnv* env, jclass, jlong id, jfloat s, jfloat c) {
    ModifyMatrix<MatrixOp::Set>(env, id, Rotation(s, c));
}
void MatrixPivotSinCos(JNIEnv* env, jclass, jlong id, jfloat s, jfloat c, jfloat x, jfloat y) {
    ModifyMatrix<MatrixOp::Set>(env, id, AtPivot(Rotation(s, c), x, y));
}
template <MatrixOp Op> void MatrixConcat(JNIEnv* env, jclass, jlong id, jlong rhs) {
    if (const auto* m = MatrixValues(env, rhs)) ModifyMatrix<Op>(env, id, *m);
}
void MatrixSetConcat(JNIEnv* env, jclass, jlong id, jlong a, jlong b) {
    const auto* left = MatrixValues(env, a);
    const auto* right = MatrixValues(env, b);
    if (left && right) ModifyMatrix<MatrixOp::Set>(env, id, Multiply(*left, *right));
}
wa_rect_t ReadRectF(JNIEnv* env, jobject rect) {
    const auto type = env->GetObjectClass(rect);
    const auto get = [&](const char* field) {
        const auto id = env->GetFieldID(type, field, "F");
        return id ? env->GetFloatField(rect, id) : 0.f;
    };
    return {get("left"), get("top"), get("right"), get("bottom")};
}
jboolean MatrixMapRect(JNIEnv* env, jclass, jlong id, jobject dst, jobject src) {
    const auto* m = MatrixValues(env, id);
    if (!m) return false;
    SetRect(env, dst, MapRect(*m, ReadRectF(env, src)), true);
    return RectStaysRect(*m);
}
jboolean MatrixRectToRect(JNIEnv* env, jclass, jlong id, jobject src, jobject dst, jint mode) {
    auto* m = MatrixValues(env, id);
    if (!m) return false;
    const auto a = ReadRectF(env, src), b = ReadRectF(env, dst);
    if (!HasArea(a)) {
        *m = kIdentity;
        return false;
    }
    if (!HasArea(b)) {
        *m = Scaling(0, 0);
        return true;
    }
    const float dx = b.r - b.l, dy = b.b - b.t, sx = a.r - a.l, sy = a.b - a.t;
    const float scale_x = mode == 0 ? dx / sx : std::min(dx / sx, dy / sy);
    const float scale_y = mode == 0 ? dy / sy : scale_x;
    const float align = mode <= 1 ? 0.f : mode == 2 ? 0.5f : 1.f;
    *m = {scale_x, 0, b.l - a.l * scale_x + (dx - sx * scale_x) * align,
          0, scale_y, b.t - a.t * scale_y + (dy - sy * scale_y) * align, 0, 0, 1};
    return true;
}
jfloat MatrixMapRadius(JNIEnv* env, jclass, jlong id, jfloat radius) {
    const auto* m = MatrixValues(env, id);
    if (!m) return 0.f;
    const auto [x, y] = MapPoint(*m, 0, 0);
    const auto a = MapPoint(*m, radius, 0), b = MapPoint(*m, 0, radius);
    return std::sqrt(std::hypot(a[0] - x, a[1] - y) * std::hypot(b[0] - x, b[1] - y));
}
jboolean MatrixEquals(JNIEnv* env, jclass, jlong lhs, jlong rhs) {
    const auto* a = MatrixValues(env, lhs);
    const auto* b = MatrixValues(env, rhs);
    return a && b && *a == *b;
}
jboolean MatrixPolyToPoly(JNIEnv* env, jclass, jlong id, jfloatArray src, jint src_index,
                          jfloatArray dst, jint dst_index, jint count) {
    auto* m = MatrixValues(env, id);
    if (!m || count < 0 || count > 4) return false;
    if (count == 0) { *m = kIdentity; return true; }
    std::array<float, 8> from{}, to{};
    env->GetFloatArrayRegion(src, src_index, count * 2, from.data());
    env->GetFloatArrayRegion(dst, dst_index, count * 2, to.data());
    if (env->ExceptionCheck()) return false;
    if (count == 1) { *m = Translation(to[0] - from[0], to[1] - from[1]); return true; }
    if (count == 2) {
        from = {from[0], from[1], from[2], from[3], from[0] - from[3] + from[1], from[1] + from[2] - from[0]};
        to = {to[0], to[1], to[2], to[3], to[0] - to[3] + to[1], to[1] + to[2] - to[0]};
        count = 3;
    }
    // Solve for the six affine coefficients or eight homography coefficients.
    const int n = count * 2;
    double equations[8][9]{};
    for (int i = 0; i < n; i += 2) {
        const double x = from[i], y = from[i + 1], u = to[i], v = to[i + 1];
        equations[i][0] = equations[i + 1][3] = x;
        equations[i][1] = equations[i + 1][4] = y;
        equations[i][2] = equations[i + 1][5] = 1;
        if (n == 8) {
            equations[i][6] = -u * x; equations[i][7] = -u * y;
            equations[i + 1][6] = -v * x; equations[i + 1][7] = -v * y;
        }
        equations[i][n] = u;
        equations[i + 1][n] = v;
    }
    for (int i = 0; i < n; ++i) {
        int pivot = i;
        for (int j = i + 1; j < n; ++j)
            if (std::abs(equations[j][i]) > std::abs(equations[pivot][i])) pivot = j;
        const double factor = equations[pivot][i];
        if (!factor || !std::isfinite(factor)) return false;
        std::swap(equations[pivot], equations[i]);
        for (int j = i; j <= n; ++j) equations[i][j] /= factor;
        for (int k = 0; k < n; ++k) {
            if (k == i) continue;
            const double multiple = equations[k][i];
            for (int j = i; j <= n; ++j) equations[k][j] -= multiple * equations[i][j];
        }
    }
    auto result = kIdentity;
    for (int i = 0; i < n; ++i) {
        result[i] = equations[i][n];
        if (!std::isfinite(result[i])) return false;
    }
    *m = result;
    return true;
}
