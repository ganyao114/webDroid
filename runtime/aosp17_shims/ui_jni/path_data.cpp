#include "native_graphics.h"
#include "software_raster.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace wa::ui_jni::detail {

struct SvgPathData {
    struct Command {
        char verb;
        bool explicit_verb;
        uint8_t count = 0;
        std::array<float, 7> values{};
    };
    std::vector<Command> commands;
};

namespace {

struct Point {
    double x = 0;
    double y = 0;
    Point operator+(Point other) const { return {x + other.x, y + other.y}; }
    Point operator-(Point other) const { return {x - other.x, y - other.y}; }
    Point operator*(double scale) const { return {x * scale, y * scale}; }
};

constexpr double kPi = std::numbers::pi_v<double>;

bool IsSpace(char c) {
    return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f';
}
bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}
bool IsLetter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
char Upper(char c) {
    return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
}

class PathData {
  public:
    explicit PathData(std::string_view input) : input_(input) {}

    std::vector<float> Parse(SvgPathData* data = nullptr) {
        char command = 0;
        Space();
        while (cursor_ < input_.size()) {
            const char next = Peek();
            const bool changed = IsLetter(next);
            if (changed)
                command = input_[cursor_++];
            if (data) raw_ = &data->commands.emplace_back(SvgPathData::Command{command, changed});
            if (commands_.empty() && Upper(command) != 'M')
                Invalid();
            const bool relative = command != Upper(command);
            switch (Upper(command)) {
            case 'M': {
                const Point to = Coordinate(relative);
                Emit(WA_PATH_MOVE, to);
                current_ = contour_ = to;
                previous_ = 'M';
                command = relative ? 'l' : 'L';
                break;
            }
            case 'L':
                Line(Coordinate(relative));
                break;
            case 'H': {
                double x = Number();
                Line({relative ? current_.x + x : x, current_.y});
                break;
            }
            case 'V': {
                double y = Number();
                Line({current_.x, relative ? current_.y + y : y});
                break;
            }
            case 'C': {
                const Point a = Coordinate(relative);
                const Point b = Coordinate(relative);
                Cubic(a, b, Coordinate(relative));
                previous_ = 'C';
                break;
            }
            case 'S': {
                const Point a =
                    previous_ == 'C' || previous_ == 'S' ? current_ * 2 - control_ : current_;
                const Point b = Coordinate(relative);
                Cubic(a, b, Coordinate(relative));
                previous_ = 'S';
                break;
            }
            case 'Q': {
                const Point control = Coordinate(relative);
                Quadratic(control, Coordinate(relative));
                previous_ = 'Q';
                break;
            }
            case 'T': {
                const Point control =
                    previous_ == 'Q' || previous_ == 'T' ? current_ * 2 - control_ : current_;
                Quadratic(control, Coordinate(relative));
                previous_ = 'T';
                break;
            }
            case 'A': {
                const double rx = Number(), ry = Number(), rotation = Number();
                const bool large = Flag(), sweep = Flag();
                Arc(rx, ry, rotation, large, sweep, Coordinate(relative));
                previous_ = 'A';
                break;
            }
            case 'Z':
                if (!changed)
                    Invalid();
                Emit(WA_PATH_CLOSE);
                current_ = contour_;
                previous_ = 'Z';
                command = 0;
                break;
            default:
                Invalid();
            }
            Space();
        }
        return std::move(commands_);
    }

  private:
    [[noreturn]] void Invalid() const {
        throw std::invalid_argument("Invalid SVG path data at byte " + std::to_string(cursor_));
    }
    char Peek() const { return cursor_ < input_.size() ? input_[cursor_] : '\0'; }
    void Space() {
        while (IsSpace(Peek()))
            ++cursor_;
    }
    void Separator() {
        Space();
        if (Peek() == ',') {
            ++cursor_;
            Space();
        }
    }
    double Number() {
        Separator();
        bool minus = Peek() == '-';
        if (minus || Peek() == '+')
            ++cursor_;
        bool any = false;
        double value = 0;
        while (IsDigit(Peek())) {
            value = value * 10 + input_[cursor_++] - '0';
            any = true;
        }
        if (Peek() == '.') {
            ++cursor_;
            double place = 0.1;
            while (IsDigit(Peek())) {
                value += (input_[cursor_++] - '0') * place;
                place *= 0.1;
                any = true;
            }
        }
        if (!any)
            Invalid();
        if (Peek() == 'e' || Peek() == 'E') {
            ++cursor_;
            bool negative_exponent = Peek() == '-';
            if (negative_exponent || Peek() == '+')
                ++cursor_;
            if (!IsDigit(Peek()))
                Invalid();
            int exponent = 0;
            while (IsDigit(Peek()))
                exponent = std::min(1024, 10 * exponent + input_[cursor_++] - '0');
            if (value != 0)
                value *= std::pow(10.0, negative_exponent ? -exponent : exponent);
        }
        if (!std::isfinite(value))
            Invalid();
        return Record(minus ? -value : value);
    }
    double Record(double value) {
        if (raw_) raw_->values[raw_->count++] = static_cast<float>(value);
        return value;
    }
    bool Flag() {
        Separator();
        char flag = Peek();
        if (flag != '0' && flag != '1')
            Invalid();
        ++cursor_;
        return Record(flag == '1') != 0;
    }
    Point Coordinate(bool relative) {
        Point point{Number(), Number()};
        return relative ? current_ + point : point;
    }

    void Emit(wa_path_command command, Point a = {}, Point b = {}, Point c = {}) {
        if (commands_.size() / 7 >= WA_SF_LAYER_LIST_CAPACITY / sizeof(uint32_t))
            Invalid();
        commands_.push_back(static_cast<float>(command));
        for (double value : {a.x, a.y, b.x, b.y, c.x, c.y}) {
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                Invalid();
            commands_.push_back(static_cast<float>(value));
        }
    }
    void Line(Point to) {
        Emit(WA_PATH_LINE, to);
        current_ = to;
        previous_ = 'L';
    }
    void Cubic(Point a, Point b, Point to) {
        Emit(WA_PATH_CUBIC, a, b, to);
        control_ = b;
        current_ = to;
    }
    void Quadratic(Point control, Point to) {
        Emit(WA_PATH_QUAD, control, to);
        control_ = control;
        current_ = to;
    }

    void Arc(double rx, double ry, double rotation, bool large, bool sweep, Point to) {
        if (current_.x == to.x && current_.y == to.y)
            return;
        if (rx == 0 || ry == 0) {
            Line(to);
            return;
        }
        rx = std::abs(rx);
        ry = std::abs(ry);
        const double phi = std::fmod(rotation, 360.0) * kPi / 180;
        const double cosine = std::cos(phi), sine = std::sin(phi);
        const auto rotate = [&](Point p) {
            return Point{cosine * p.x - sine * p.y, sine * p.x + cosine * p.y};
        };
        const Point delta = (current_ - to) * 0.5;
        const Point mid{cosine * delta.x + sine * delta.y, -sine * delta.x + cosine * delta.y};
        double nx = mid.x / rx, ny = mid.y / ry;
        const double scale = std::hypot(nx, ny);
        if (!std::isfinite(scale) || scale == 0)
            Invalid();
        if (scale > 1) {
            rx *= scale;
            ry *= scale;
            nx /= scale;
            ny /= scale;
        }
        const double length_squared = nx * nx + ny * ny;
        const double magnitude = std::sqrt(std::max(0.0, (1.0 - length_squared) / length_squared));
        const double factor = large == sweep ? -magnitude : magnitude;
        const Point local_center{factor * ny, -factor * nx};
        const Point center =
            (current_ + to) * 0.5 + rotate({rx * local_center.x, ry * local_center.y});
        const Point a{nx - local_center.x, ny - local_center.y};
        const Point b{-nx - local_center.x, -ny - local_center.y};
        double angle = std::atan2(a.y, a.x);
        double travel = std::atan2(a.x * b.y - a.y * b.x, a.x * b.x + a.y * b.y);
        if (sweep && travel < 0)
            travel += 2 * kPi;
        if (!sweep && travel > 0)
            travel -= 2 * kPi;
        if (!std::isfinite(angle) || !std::isfinite(travel))
            Invalid();
        const int segments = std::max(1, static_cast<int>(std::ceil(std::abs(travel) / (kPi / 2))));
        const double step = travel / segments;
        const double tangent_scale = 4.0 / 3.0 * std::tan(step / 4);
        for (int i = 0; i < segments; ++i) {
            const double next = angle + step;
            const Point end = i == segments - 1
                                  ? to
                                  : center + rotate({rx * std::cos(next), ry * std::sin(next)});
            const Point first_tangent = rotate({-rx * std::sin(angle), ry * std::cos(angle)});
            const Point last_tangent = rotate({-rx * std::sin(next), ry * std::cos(next)});
            Cubic(current_ + first_tangent * tangent_scale, end - last_tangent * tangent_scale,
                  end);
            angle = next;
        }
    }

    std::string_view input_;
    size_t cursor_ = 0;
    Point current_, contour_, control_;
    char previous_ = 0;
    std::vector<float> commands_;
    SvgPathData::Command* raw_ = nullptr;
};

PathGeometry MakeGeometry(std::span<const float> commands) {
    PathGeometry path;
    path.verbs.reserve(commands.size() / 7);
    for (size_t i = 0; i < commands.size(); i += 7) {
        const auto* p = commands.data() + i + 1;
        switch (static_cast<int>(commands[i])) {
        case WA_PATH_MOVE: path.Move(p[0], p[1]); break;
        case WA_PATH_LINE: path.Line(p[0], p[1]); break;
        case WA_PATH_QUAD: path.Quad(p[0], p[1], p[2], p[3]); break;
        case WA_PATH_CUBIC: path.Cubic(p[0], p[1], p[2], p[3], p[4], p[5]); break;
        case WA_PATH_CLOSE: path.Close(); break;
        }
    }
    return path;
}

std::string SvgString(const SvgPathData& data) {
    std::string text;
    for (const auto& command : data.commands) {
        if (command.explicit_verb) text.push_back(command.verb);
        for (uint8_t i = 0; i < command.count; ++i) {
            text.push_back(' ');
            if (Upper(command.verb) == 'A' && (i == 3 || i == 4)) {
                text.push_back(command.values[i] != 0 ? '1' : '0');
            } else {
                char number[32];
                const auto end = std::to_chars(number, number + sizeof(number), command.values[i]);
                text.append(number, end.ptr);
            }
        }
    }
    return text;
}

SvgPathData* GetSvgData(JNIEnv* env, jlong id) {
    return Peer<SvgPathData, &State::svg_paths>(env, id);
}

jlong StoreSvgData(JNIEnv* env, SvgPathData data = {}) {
    auto* state = GetState(env);
    return state ? Peers<SvgPathData, &State::svg_paths>(*state).Insert(std::move(data)) : 0;
}

jlong CreateEmptySvgData(JNIEnv* env, jclass) {
    return CallSafely(env, [&] { return StoreSvgData(env); });
}
jlong CopySvgData(JNIEnv* env, jclass, jlong source) {
    return CallSafely(env, [&]() -> jlong {
        if (const auto* src = GetSvgData(env, source)) return StoreSvgData(env, *src);
        return 0;
    });
}
jlong CreateSvgData(JNIEnv* env, jclass, jstring text, jint) {
    return CallSafely(env, [&]() -> jlong {
        const auto utf8 = Utf8(env, text);
        if (env->ExceptionCheck()) return 0;
        SvgPathData data;
        try {
            PathData(utf8).Parse(&data);
        } catch (const std::invalid_argument&) {
            return 0;
        }
        return StoreSvgData(env, std::move(data));
    });
}
void SetSvgData(JNIEnv* env, jclass, jlong dest, jlong source) {
    CallSafely(env, [&] {
        auto* out = GetSvgData(env, dest);
        const auto* in = GetSvgData(env, source);
        if (out && in) *out = *in;
    });
}
bool CanMorph(const SvgPathData& from, const SvgPathData& to) {
    return std::ranges::equal(from.commands, to.commands, [](const auto& a, const auto& b) {
        return a.verb == b.verb && a.explicit_verb == b.explicit_verb && a.count == b.count;
    });
}
jboolean CanMorphSvgData(JNIEnv* env, jclass, jlong from, jlong to) {
    const auto* a = GetSvgData(env, from);
    const auto* b = GetSvgData(env, to);
    return a && b && CanMorph(*a, *b);
}
jboolean InterpolateSvgData(JNIEnv* env, jclass, jlong dest, jlong from, jlong to, jfloat fraction) {
    return CallSafely(env, [&]() -> jboolean {
        auto* out = GetSvgData(env, dest);
        const auto* a = GetSvgData(env, from);
        const auto* b = GetSvgData(env, to);
        if (!out || !a || !b || !CanMorph(*a, *b)) return false;
        if (out != a && out != b) *out = *a;
        for (size_t i = 0; i < out->commands.size(); ++i)
            for (uint8_t j = 0; j < out->commands[i].count; ++j)
                out->commands[i].values[j] = std::lerp(a->commands[i].values[j], b->commands[i].values[j], fraction);
        return true;
    });
}

jlong PathFromData(JNIEnv* env, jclass, jstring text) {
    return CallSafely(env, [&]() -> jlong {
        auto* state = GetState(env);
        if (!state) return 0;
        auto geometry = ParseSvgPath(env, text);
        return geometry ? Peers<PathGeometry, &State::paths>(*state).Insert(std::move(*geometry)) : 0;
    });
}

void ReplaceGeometry(PathGeometry& path, PathGeometry geometry) {
    geometry.fill = path.fill;
    geometry.generation = path.generation == INT32_MAX ? 1 : path.generation + 1;
    path = std::move(geometry);
}
void ParseStringForPath(JNIEnv* env, jclass, jlong id, jstring text, jint) {
    CallSafely(env, [&] {
        if (auto* path = Path(env, id)) {
            if (auto geometry = ParseSvgPath(env, text)) ReplaceGeometry(*path, std::move(*geometry));
        }
    });
}
void CreatePathFromSvgData(JNIEnv* env, jclass, jlong id, jlong data) {
    CallSafely(env, [&] {
        if (auto* path = Path(env, id)) {
            if (auto geometry = PathFromSvgData(env, data)) ReplaceGeometry(*path, std::move(*geometry));
        }
    });
}

jfloatArray ParsePathData(JNIEnv* env, jclass, jstring text) {
    return CallSafely(env, [&]() -> jfloatArray {
        const std::string utf8 = Utf8(env, text);
        if (env->ExceptionCheck())
            return nullptr;
        try {
            const auto commands = PathData(utf8).Parse();
            jfloatArray result = env->NewFloatArray(commands.size());
            if (result && !commands.empty())
                env->SetFloatArrayRegion(result, 0, commands.size(), commands.data());
            return result;
        } catch (const std::invalid_argument& error) {
            Throw(env, "java/lang/IllegalArgumentException", error.what());
            return nullptr;
        }
    });
}

} // namespace

std::optional<PathGeometry> ParseSvgPath(JNIEnv* env, jstring text) {
    auto input = Utf8(env, text);
    if (env->ExceptionCheck()) return std::nullopt;
    try {
        return MakeGeometry(PathData(input).Parse());
    } catch (const std::invalid_argument& error) {
        Throw(env, "java/lang/IllegalArgumentException", error.what());
        return std::nullopt;
    }
}

std::optional<PathGeometry> PathFromSvgData(JNIEnv* env, jlong handle) {
    const auto* data = GetSvgData(env, handle);
    if (!data) return std::nullopt;
    try {
        return MakeGeometry(PathData(SvgString(*data)).Parse());
    } catch (const std::invalid_argument& error) {
        Throw(env, "java/lang/IllegalArgumentException", error.what());
        return std::nullopt;
    }
}

#include "path_geometry.inc"

void RegisterPath(interp::Vm& vm) {
    if (vm.HasClass("Landroid/util/PathParser;")) {
        Register(vm, "android/util/PathParser",
                 Method<ParseStringForPath>(vm, "nParseStringForPath", "(JLjava/lang/String;I)V"),
                 Method<CreateSvgData>(vm, "nCreatePathDataFromString", "(Ljava/lang/String;I)J"),
                 Method<CreatePathFromSvgData>(vm, "nCreatePathFromPathData", "(JJ)V"),
                 Method<CreateEmptySvgData>(vm, "nCreateEmptyPathData", "()J"),
                 Method<CopySvgData>(vm, "nCreatePathData", "(J)J"),
                 Method<InterpolateSvgData>(vm, "nInterpolatePathData", "(JJJF)Z"),
                 Method<RetainPeer<SvgPathData, &State::svg_paths>>(vm, "nRegisterPathData",
                     "(Landroid/util/PathParser$PathData;J)V"),
                 Method<CanMorphSvgData>(vm, "nCanMorph", "(JJ)Z"),
                 Method<SetSvgData>(vm, "nSetPathData", "(JJ)V"));
    }
    constexpr const char* name = "android/graphics/Path";
    if (!RegisterIfDeclared(vm, name, Method<InitPath>(vm, "nInit", "()J"))) {
        Register(vm, name, Method<ParsePathData>(vm, "nativeParsePathData", "(Ljava/lang/String;)[F"));
        return;
    }
    Register(vm, name, Method<NewPath>(vm, "nInit", "(J)J"),
             Method<RetainPeer<PathGeometry, &State::paths>>(vm, "nRegisterNativeAllocation",
                                                             "(Landroid/graphics/Path;J)V"),
             Method<PathFinalizer>(vm, "nGetFinalizer", "()J"),
             Method<SetPath>(vm, "nSet", "(JJ)V"),
             Method<ResetPath>(vm, "nReset", "(J)V"),
             Method<ResetPath>(vm, "nRewind", "(J)V"),
             Method<IsPathEmpty>(vm, "nIsEmpty", "(J)Z"),
             Method<PathConvex>(vm, "nIsConvex", "(J)Z"),
             Method<IsPathRect>(vm, "nIsRect", "(JLandroid/graphics/RectF;)Z"),
             Method<PathBounds>(vm, "nComputeBounds", "(JLandroid/graphics/RectF;)V"),
             Method<ReservePath>(vm, "nIncReserve", "(JI)V"),
             Method<GetFillType>(vm, "nGetFillType", "(J)I"),
             Method<SetFillType>(vm, "nSetFillType", "(JI)V"),
             Method<PathGeneration>(vm, "nGetGenerationID", "(J)I"),
             Method<EditPath<&PathGeometry::Move, false, jfloat, jfloat>>(vm, "nMoveTo", "(JFF)V"),
             Method<EditPath<&PathGeometry::Move, true, jfloat, jfloat>>(vm, "nRMoveTo", "(JFF)V"),
             Method<EditPath<&PathGeometry::Line, false, jfloat, jfloat>>(vm, "nLineTo", "(JFF)V"),
             Method<EditPath<&PathGeometry::Line, true, jfloat, jfloat>>(vm, "nRLineTo", "(JFF)V"),
             Method<EditPath<&PathGeometry::Quad, false, jfloat, jfloat, jfloat, jfloat>>(vm, "nQuadTo", "(JFFFF)V"),
             Method<EditPath<&PathGeometry::Quad, true, jfloat, jfloat, jfloat, jfloat>>(vm, "nRQuadTo", "(JFFFF)V"),
             Method<AddConic<false>>(vm, "nConicTo", "(JFFFFF)V"),
             Method<AddConic<true>>(vm, "nRConicTo", "(JFFFFF)V"),
             Method<EditPath<&PathGeometry::Cubic, false, jfloat, jfloat, jfloat, jfloat, jfloat, jfloat>>(vm, "nCubicTo", "(JFFFFFF)V"),
             Method<EditPath<&PathGeometry::Cubic, true, jfloat, jfloat, jfloat, jfloat, jfloat, jfloat>>(vm, "nRCubicTo", "(JFFFFFF)V"),
             Method<ArcPath>(vm, "nArcTo", "(JFFFFFFZ)V"),
             Method<EditPath<&PathGeometry::Close>>(vm, "nClose", "(J)V"),
             Method<AddShape<&PathGeometry::Rect>>(vm, "nAddRect", "(JFFFFI)V"),
             Method<AddShape<&PathGeometry::Oval>>(vm, "nAddOval", "(JFFFFI)V"),
             Method<AddCircle>(vm, "nAddCircle", "(JFFFI)V"),
             Method<AddArc>(vm, "nAddArc", "(JFFFFFF)V"),
             Method<AddRoundRect>(vm, "nAddRoundRect", "(JFFFFFFI)V"),
             Method<AddRoundRectArray>(vm, "nAddRoundRect", "(JFFFF[FI)V"),
             Method<AddPath>(vm, "nAddPath", "(JJ)V"),
             Method<AddPathOffset>(vm, "nAddPath", "(JJFF)V"),
             Method<AddPathMatrix>(vm, "nAddPath", "(JJJ)V"),
             Method<OffsetPath>(vm, "nOffset", "(JFF)V"),
             Method<SetLastPoint>(vm, "nSetLastPoint", "(JFF)V"),
             Method<TransformPathTo>(vm, "nTransform", "(JJJ)V"),
             Method<TransformPath>(vm, "nTransform", "(JJ)V"),
             Method<PathOp>(vm, "nOp", "(JJIJ)Z"),
             Method<ApproximatePath>(vm, "nApproximate", "(JF)[F"),
             Method<InterpolatePath>(vm, "nInterpolate", "(JJFJ)Z"),
             Method<PathInterpolatable>(vm, "nIsInterpolatable", "(JJ)Z"),
             Method<PathFromData>(vm, "nCreateFromPathData", "(Ljava/lang/String;)J"));
    RegisterIfDeclared(vm, name, Method<ParsePathData>(vm, "nativeParsePathData", "(Ljava/lang/String;)[F"));
}

} // namespace wa::ui_jni::detail
