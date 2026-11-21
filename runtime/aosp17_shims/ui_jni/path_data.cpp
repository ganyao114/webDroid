#include "native_methods.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace wa::ui_jni::detail {
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

    std::vector<float> Parse() {
        char command = 0;
        Space();
        while (cursor_ < input_.size()) {
            const char next = Peek();
            const bool changed = IsLetter(next);
            if (changed)
                command = input_[cursor_++];
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
        return minus ? -value : value;
    }
    bool Flag() {
        Separator();
        char flag = Peek();
        if (flag != '0' && flag != '1')
            Invalid();
        ++cursor_;
        return flag == '1';
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
};

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

void RegisterPath(interp::Vm& vm) {
    Register(vm, "android/graphics/Path",
             Method<ParsePathData>(vm, "nativeParsePathData", "(Ljava/lang/String;)[F"));
}

} // namespace wa::ui_jni::detail
