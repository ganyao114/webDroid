#pragma once

#include "boot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <tuple>
#include <vector>
#include <zlib.h>

namespace wa::interp {
namespace software_raster {

struct Point {
  double x;
  double y;
  Point operator+(Point other) const { return {x + other.x, y + other.y}; }
  Point operator-(Point other) const { return {x - other.x, y - other.y}; }
  Point operator*(double scale) const { return {x * scale, y * scale}; }
  bool operator==(const Point&) const = default;
};

struct Edge {
  Point from;
  Point to;
};

inline bool ValidCanvasState(Values state) {
  // The affine transform and clip bounds can be followed by transformed clips.
  return state.size() >= 10 && (state.size() - 10) % 12 == 0;
}

inline bool InsideClips(Values state, double x, double y) {
  for (size_t offset = 10; offset < state.size(); offset += 12) {
    const auto clip = state.subspan(offset, 12);
    const double local_x = clip[0].f() * x + clip[1].f() * y + clip[2].f();
    const double local_y = clip[3].f() * x + clip[4].f() * y + clip[5].f();
    const double l = clip[6].f(), t = clip[7].f(), r = clip[8].f(), b = clip[9].f();
    if (!(local_x >= l && local_x < r && local_y >= t && local_y < b)) return false;
    const double rx = clip[10].f(), ry = clip[11].f();
    if (rx > 0 && ry > 0) {
      const double dx = std::max({l + rx - local_x, local_x - (r - rx), 0.0}) / rx;
      const double dy = std::max({t + ry - local_y, local_y - (b - ry), 0.0}) / ry;
      if (dx * dx + dy * dy > 1) return false;
    }
  }
  return true;
}

inline uint32_t SourceOver(uint32_t source, uint32_t destination) {
  const auto alpha = source >> 24;
  if (alpha == 0 || (destination >> 24) == 0) return alpha ? source : destination;
  if (alpha == 255) return source;
  const auto dst_part = (destination >> 24) * (255 - alpha);
  const auto out_alpha = alpha * 255 + dst_part;
  uint32_t result = ((out_alpha + 127) / 255) << 24;
  for (unsigned shift : {0U, 8U, 16U}) {
    const auto channel = ((source >> shift) & 255) * alpha * 255 + ((destination >> shift) & 255) * dst_part;
    result |= ((channel + out_alpha / 2) / out_alpha) << shift;
  }
  return result;
}

struct Contour {
  std::vector<Point> points;
  bool closed = false;
};

inline std::vector<Contour> Flatten(Values data, int count) {
  std::vector<Contour> contours;
  Point cursor{0, 0};
  const auto append = [&](Point point) {
    if (contours.empty() || contours.back().closed) contours.push_back({{cursor}});
    auto& points = contours.back().points;
    if (point != points.back()) points.push_back(point);
  };
  for (int i = 0; i < count; ++i) {
    const auto command = data.subspan(static_cast<size_t>(i) * 7, 7);
    const auto point = [&](size_t j) -> Point { return {command[j].f(), command[j + 1].f()}; };
    const auto verb = command[0].f();
    if (!(verb >= 1 && verb <= 5)) continue;
    switch (static_cast<int>(verb)) {
      case 1:
        cursor = point(1);
        contours.push_back({{cursor}});
        break;
      case 2: {
        const auto end = point(1);
        append(end);
        cursor = end;
        break;
      }
      case 3:
      case 4: {
        const bool cubic = command[0].f() == 4;
        const auto control = point(1), second = point(3);
        const auto end = cubic ? point(5) : second;
        constexpr int steps = 24;
        for (int step = 1; step <= steps; ++step) {
          const double t = double(step) / steps, u = 1 - t;
          const auto coordinate = [&](double a, double b, double c, double d) {
            return cubic ? u * u * u * a + 3 * u * u * t * b + 3 * u * t * t * c + t * t * t * d
                         : u * u * a + 2 * u * t * b + t * t * d;
          };
          append({coordinate(cursor.x, control.x, second.x, end.x),
                  coordinate(cursor.y, control.y, second.y, end.y)});
        }
        cursor = end;
        break;
      }
      case 5:
        if (!contours.empty()) {
          auto& contour = contours.back();
          cursor = contour.points.front();
          if (contour.points.size() > 1 && contour.points.back() == cursor) contour.points.pop_back();
          contour.closed = true;
        }
        break;
    }
  }
  return contours;
}

inline Point Transform(Values matrix, Point point) {
  return {matrix[0].f() * point.x + matrix[1].f() * point.y + matrix[2].f(),
          matrix[3].f() * point.x + matrix[4].f() * point.y + matrix[5].f()};
}

inline void AppendEdges(std::vector<Edge>& edges, std::span<const Point> polygon, Values matrix,
                        bool positive_winding) {
  if (polygon.size() < 2) return;
  double signed_area = 0;
  if (positive_winding) {
    for (size_t i = 1; i + 1 < polygon.size(); ++i) {
      const auto a = polygon[i] - polygon[0], b = polygon[i + 1] - polygon[0];
      signed_area += a.x * b.y - a.y * b.x;
    }
  }
  const auto map = [&](Point point) { return matrix.empty() ? point : Transform(matrix, point); };
  auto from = map(polygon.back());
  for (auto point : polygon) {
    const auto to = map(point);
    if (from.y != to.y && std::isfinite(from.x) && std::isfinite(from.y) &&
        std::isfinite(to.x) && std::isfinite(to.y)) {
      edges.push_back(signed_area < 0 ? Edge{to, from} : Edge{from, to});
    }
    from = to;
  }
}

inline std::vector<Edge> StrokeEdges(std::vector<Contour> contours, Values matrix, double width,
                                     int cap, int join, double miter_limit) {
  std::vector<Edge> edges;
  const double radius = width == 0 ? 0.5 : width * 0.5;
  if (width == 0) {
    for (auto& contour : contours) {
      for (auto& point : contour.points) point = Transform(matrix, point);
    }
    matrix = {};
  }
  const auto add_round = [&](Point center) {
    std::array<Point, 24> polygon;
    for (size_t i = 0; i < polygon.size(); ++i) {
      const double angle = 2 * std::numbers::pi * i / polygon.size();
      polygon[i] = center + Point{std::cos(angle), std::sin(angle)} * radius;
    }
    AppendEdges(edges, polygon, matrix, true);
  };
  for (auto& contour : contours) {
    auto& points = contour.points;
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 2) continue;
    const size_t segments = contour.closed ? points.size() : points.size() - 1;
    std::vector<Point> normals(segments), directions(segments);
    for (size_t i = 0; i < segments; ++i) {
      auto start = points[i], end = points[(i + 1) % points.size()];
      const auto delta = end - start;
      const double length = std::hypot(delta.x, delta.y);
      if (!(length > 0 && std::isfinite(length))) continue;
      const auto direction = directions[i] = delta * (1 / length);
      const auto normal = normals[i] = Point{-direction.y, direction.x} * radius;
      if (cap == 2 && !contour.closed) {
        if (i == 0) start = start - direction * radius;
        if (i + 1 == segments) end = end + direction * radius;
      }
      AppendEdges(edges, std::array{start + normal, end + normal, end - normal, start - normal}, matrix, true);
    }
    if (cap == 1 && !contour.closed) {
      add_round(points.front());
      add_round(points.back());
    }
    for (size_t i = contour.closed ? 0 : 1; i < segments; ++i) {
      const size_t previous = (i + segments - 1) % segments;
      const auto before = directions[previous], after = directions[i];
      const double cross = before.x * after.y - before.y * after.x;
      if (cross == 0) continue;
      const auto vertex = points[i];
      if (join == 1) {
        add_round(vertex);
        continue;
      }
      const double side = cross > 0 ? -1 : 1;
      const auto first = normals[previous] * side, last = normals[i] * side;
      const double miter_denominator = 1 + before.x * after.x + before.y * after.y;
      if (join == 0 && miter_denominator > 0 && 2 <= miter_limit * miter_limit * miter_denominator) {
        const auto miter = vertex + (first + last) * (1 / miter_denominator);
        AppendEdges(edges, std::array{vertex, vertex + first, miter, vertex + last}, matrix, true);
      } else {
        AppendEdges(edges, std::array{vertex, vertex + first, vertex + last}, matrix, true);
      }
    }
  }
  return edges;
}

inline void BoxBlur(std::span<const uint8_t> input, std::span<uint8_t> output, int lines,
                    int length, int step, int stride, int radius) {
  for (int line = 0; line < lines; ++line) {
    const size_t first = static_cast<size_t>(line) * stride;
    const auto index = [first, step](int at) { return first + static_cast<size_t>(at) * step; };
    int64_t sum = 0;
    for (int i = 0; i < length && i <= radius; ++i) sum += input[index(i)];
    const int64_t window = 2LL * radius + 1;
    for (int i = 0; i < length; ++i) {
      output[index(i)] = (sum + window / 2) / window;
      if (i >= radius) sum -= input[index(i - radius)];
      if (i < length - radius - 1) sum += input[index(i + radius + 1)];
    }
  }
}

inline uint32_t SampleGradient(int type, int tile, Values geometry, Values stops, Values colors, Point point) {
  const double x = point.x - geometry[0].f(), y = point.y - geometry[1].f();
  double t;
  if (type == 1) {
    const double dx = geometry[2].f() - geometry[0].f(), dy = geometry[3].f() - geometry[1].f();
    const double length = dx * dx + dy * dy;
    t = length == 0 ? 1 : (x * dx + y * dy) / length;
  } else if (type == 2) {
    t = std::hypot(x, y) / geometry[2].f();
  } else {
    t = std::atan2(y, x) / (2 * std::numbers::pi);
    if (t < 0) t += 1;
  }
  if (!std::isfinite(t)) return 0;
  if (tile == 1) t -= std::floor(t);
  if (tile == 2) {
    t -= 2 * std::floor(t * 0.5);
    if (t > 1) t = 2 - t;
  }
  if (tile == 3 && (t < 0 || t > 1)) return 0;
  if (t <= stops.front().f()) return colors.front().i();
  if (t >= stops.back().f()) return colors.back().i();
  size_t next = 1;
  while (stops[next].f() <= t) ++next;
  const double previous = stops[next - 1].f();
  const double weight = (t - previous) / (stops[next].f() - previous);
  const uint32_t left = colors[next - 1].i(), right = colors[next].i();
  uint32_t color = 0;
  for (unsigned shift : {0U, 8U, 16U, 24U}) {
    const double a = (left >> shift) & 255, b = (right >> shift) & 255;
    color |= static_cast<uint32_t>(std::lround(a + (b - a) * weight)) << shift;
  }
  return color;
}

inline Value DrawPathWithClip(Runtime& vm, Values args, std::span<const int32_t> mask) {
  auto& pixels = vm.NonNull(args[0].ref()).elements;
  const int width = args[1].i(), height = args[2].i(), count = args[4].i(), rule = args[5].i();
  const auto& path = vm.NonNull(args[3].ref()).elements;
  const auto& transform = vm.NonNull(args[6].ref()).elements;
  const auto color = static_cast<uint32_t>(args[7].i());
  const auto blur = args[8].f();
  const auto blur_style = args[9].i();
  const bool alpha_only = args[10].i() != 0;
  const int gradient = args[11].i(), tile = args[12].i();
  const int style = args[16].i(), cap = args[18].i(), join = args[19].i();
  const auto stroke_width = args[17].f(), miter_limit = args[20].f();
  if (width < 0 || height < 0 || count < 0 || static_cast<uint64_t>(width) * height > pixels.size() ||
      static_cast<size_t>(count) > path.size() / 7 || !ValidCanvasState(transform) || rule < 0 || rule > 3 ||
      style < 0 || style > 2 || cap < 0 || cap > 2 || join < 0 || join > 2 ||
      !(stroke_width >= 0 && std::isfinite(stroke_width) && miter_limit >= 0)) {
    vm.Fail("Ljava/lang/IllegalArgumentException;", "Invalid software bitmap draw parameters");
  }
  Values geometry, stops, colors;
  if (gradient) {
    geometry = vm.NonNull(args[13].ref()).elements;
    stops = vm.NonNull(args[14].ref()).elements;
    colors = vm.NonNull(args[15].ref()).elements;
    if (gradient < 1 || gradient > 3 || tile < 0 || tile > 3 || geometry.size() != 4 ||
        stops.size() < 2 || stops.size() != colors.size()) {
      vm.Fail("Ljava/lang/IllegalArgumentException;", "Invalid software gradient parameters");
    }
    float previous = 0;
    for (const auto stop : stops) {
      if (!(stop.f() >= previous && stop.f() <= 1)) {
        vm.Fail("Ljava/lang/IllegalArgumentException;", "Invalid gradient stop order");
      }
      previous = stop.f();
    }
  }
  const double a = transform[0].f(), b = transform[1].f(), c = transform[3].f(), d = transform[4].f();
  const double det = a * d - b * c;
  const auto draw_pixel = [&](int x, int y, unsigned coverage) {
    if (!InsideClips(transform, x + 0.5, y + 0.5)) return;
    const size_t index = static_cast<size_t>(y) * width + x;
    if (!mask.empty()) {
      coverage = (coverage * (static_cast<uint32_t>(mask[index]) >> 24) + 127) / 255;
      if (coverage == 0) return;
    }
    uint32_t source = color;
    if (gradient && det != 0) {
      const double px = x + 0.5 - transform[2].f(), py = y + 0.5 - transform[5].f();
      source = SampleGradient(gradient, tile, geometry, stops, colors,
                              {(d * px - b * py) / det, (a * py - c * px) / det});
      coverage = (coverage * (color >> 24) + 127) / 255;
    }
    const uint32_t alpha = ((source >> 24) * coverage + 127) / 255;
    source = (alpha_only ? 0 : source & 0x00ffffff) | (alpha << 24);
    pixels[index] = Value::Int(static_cast<int32_t>(SourceOver(source, pixels[index].i())));
  };
  const auto boundary = [&](double edge, int limit) {
    return std::isfinite(edge) ? static_cast<int>(std::clamp(std::ceil(edge - 0.5), 0.0, double(limit))) : 0;
  };
  const int left = boundary(transform[6].f(), width), top = boundary(transform[7].f(), height);
  const int right = boundary(transform[8].f(), width), bottom = boundary(transform[9].f(), height);
  if (right <= left || bottom <= top || (color >> 24) == 0) return {};
  auto contours = Flatten(path, count);
  std::array<std::vector<Edge>, 2> edges;
  if (style != 1) {
    for (const auto& contour : contours) AppendEdges(edges[0], contour.points, transform, false);
  }
  if (style != 0) edges[1] = StrokeEdges(std::move(contours), transform, stroke_width, cap, join, miter_limit);
  const int radius = blur > 0 && std::isfinite(blur)
      ? static_cast<int>(std::min<double>(std::ceil(blur), std::max(width, height))) : 0;
  const size_t size = static_cast<size_t>(width) * height;
  std::vector<uint8_t> coverage(radius ? size : 0);
  std::vector<std::tuple<double, int, int>> crossings;
  crossings.reserve(edges[0].size() + edges[1].size());
  for (int y = top; y < bottom; ++y) {
    const double scan = y + 0.5;
    crossings.clear();
    for (int channel = 0; channel < 2; ++channel) {
      for (const auto& [from, to] : edges[channel]) {
        if (scan < std::min(from.y, to.y) || scan >= std::max(from.y, to.y)) continue;
        const auto x = from.x + (to.x - from.x) * ((scan - from.y) / (to.y - from.y));
        crossings.emplace_back(x, from.y < to.y ? 1 : -1, channel);
      }
    }
    std::ranges::sort(crossings);
    int begin = left;
    std::array<int, 2> winding{};
    const auto draw_to = [&](int end) {
      const bool inside = rule & 1 ? (winding[0] & 1) != 0 : winding[0] != 0;
      if ((style != 1 && inside == (rule < 2)) || winding[1] != 0) {
        for (int x = begin; x < end; ++x) {
          const size_t at = static_cast<size_t>(y) * width + x;
          if (radius) coverage[at] = 255;
          else draw_pixel(x, y, 255);
        }
      }
      begin = end;
    };
    for (auto [x, sign, channel] : crossings) {
      draw_to(std::clamp(boundary(x, width), left, right));
      winding[channel] += sign;
    }
    draw_to(right);
  }
  if (radius) {
    std::vector<uint8_t> horizontal(size), blurred(size);
    BoxBlur(coverage, horizontal, height, width, 1, width, radius);
    BoxBlur(horizontal, blurred, width, height, width, 1, radius);
    for (int y = top; y < bottom; ++y) {
      for (int x = left; x < right; ++x) {
        const size_t at = static_cast<size_t>(y) * width + x;
        int value = blurred[at];
        if (blur_style == 1) value = std::max(value, int(coverage[at]));
        else if (blur_style == 2 && coverage[at]) value = 0;
        else if (blur_style == 3 && !coverage[at]) value = 0;
        if (value == 0) continue;
        draw_pixel(x, y, value);
      }
    }
  }
  return {};
}

inline Value DrawPath(Runtime& vm, Object*, Values args) {
  return DrawPathWithClip(vm, args, {});
}

inline Value ExtractAlpha(Runtime& vm, Object*, Values args) {
  const auto& pixels = vm.NonNull(args[0].ref()).elements;
  const int width = args[1].i(), height = args[2].i(), radius = args[4].i();
  auto& result = vm.NonNull(args[3].ref()).elements;
  const int64_t extended_width = int64_t(width) + 2LL * radius;
  const int64_t extended_height = int64_t(height) + 2LL * radius;
  if (width <= 0 || height <= 0 || radius < 0 ||
      static_cast<uint64_t>(width) * height != pixels.size() ||
      extended_width > INT32_MAX || extended_height > INT32_MAX ||
      static_cast<uint64_t>(extended_width) * extended_height != result.size()) {
    vm.Fail("Ljava/lang/IllegalArgumentException;", "Invalid alpha bitmap sizes");
  }
  if (radius == 0) {
    for (size_t i = 0; i < pixels.size(); ++i) result[i] = Value::Int(pixels[i].i() & 0xff000000);
    return {};
  }
  std::vector<uint8_t> alpha(result.size()), horizontal(result.size()), blurred(result.size());
  for (int row = 0; row < height; ++row) {
    const auto dst_at = static_cast<size_t>(row + radius) * extended_width + radius;
    const auto src_at = static_cast<size_t>(row) * width;
    for (int x = 0; x < width; ++x) alpha[dst_at + x] = static_cast<uint32_t>(pixels[src_at + x].i()) >> 24;
  }
  BoxBlur(alpha, horizontal, extended_height, extended_width, 1, extended_width, radius);
  BoxBlur(horizontal, blurred, extended_width, extended_height, extended_width, 1, radius);
  for (size_t i = 0; i < result.size(); ++i) result[i] = Value::Int(static_cast<uint32_t>(blurred[i]) << 24);
  return {};
}

inline Value DrawBitmap(Runtime& vm, Object*, Values args) {
  auto& target = vm.NonNull(args[0].ref()).elements;
  auto& source_array = vm.NonNull(args[3].ref()).elements;
  const int width = args[1].i(), height = args[2].i();
  const int source_width = args[4].i(), source_height = args[5].i();
  const auto& rect = vm.NonNull(args[6].ref()).elements;
  const auto& transform = vm.NonNull(args[7].ref()).elements;
  const double opacity = args[8].f();
  const uint32_t tint = args[9].i();
  const bool has_alpha = args[10].i(), alpha_source = args[11].i();
  const bool alpha_target = args[12].i(), filter = args[13].i();
  const int mask = args[14].i();
  if (width <= 0 || height <= 0 || source_width <= 0 || source_height <= 0 ||
      static_cast<uint64_t>(width) * height > target.size() ||
      static_cast<uint64_t>(source_width) * source_height > source_array.size() ||
      rect.size() != 8 || !ValidCanvasState(transform) || mask < 0 || mask > 3 ||
      !(opacity >= 0 && opacity <= 1)) {
    vm.Fail("Ljava/lang/IllegalArgumentException;", "Invalid software bitmap blit parameters");
  }
  if (opacity == 0) return {};
  const double sl = rect[0].f(), st = rect[1].f(), sr = rect[2].f(), sb = rect[3].f();
  const double dl = rect[4].f(), dt = rect[5].f(), dr = rect[6].f(), db = rect[7].f();
  const double a = transform[0].f(), b = transform[1].f(), tx = transform[2].f();
  const double c = transform[3].f(), d = transform[4].f(), ty = transform[5].f();
  const double det = a * d - b * c;
  if (!(sl < sr && st < sb && dl < dr && dt < db) || !std::isfinite(det) || det == 0) return {};
  const auto range = [](double a, double b, double offset, double low_x, double high_x,
                        double low_y, double high_y) {
    const double x0 = a * low_x, x1 = a * high_x, y0 = b * low_y, y1 = b * high_y;
    return std::pair{std::min(x0, x1) + std::min(y0, y1) + offset,
                     std::max(x0, x1) + std::max(y0, y1) + offset};
  };
  auto [min_x, max_x] = range(a, b, tx, dl, dr, dt, db);
  auto [min_y, max_y] = range(c, d, ty, dl, dr, dt, db);
  const auto pixel_bound = [](double coordinate, int limit) {
    return std::isfinite(coordinate)
        ? static_cast<int>(std::clamp(std::ceil(coordinate - 0.5), 0.0, double(limit))) : 0;
  };
  const int left = pixel_bound(std::max(min_x, double(transform[6].f())), width);
  const int top = pixel_bound(std::max(min_y, double(transform[7].f())), height);
  const int right = pixel_bound(std::min(max_x, double(transform[8].f())), width);
  const int bottom = pixel_bound(std::min(max_y, double(transform[9].f())), height);
  if (right <= left || bottom <= top) return {};
  const double half_width = (dr - dl) * 0.5, half_height = (db - dt) * 0.5;
  const double corner = std::clamp(double(args[15].f()), 0.0, std::min(half_width, half_height));
  const auto in_mask = [&](double x, double y) {
    const double dx = std::abs(x - (dl + half_width)), dy = std::abs(y - (dt + half_height));
    if (mask == 1 && corner > 0) {
      return std::hypot(std::max(dx - half_width + corner, 0.0),
                        std::max(dy - half_height + corner, 0.0)) <= corner;
    }
    if (mask >= 2) {
      const double u = dx / half_width, v = dy / half_height;
      return mask == 2 ? u * u + v * v <= 1 : u * u * u * u + v * v * v * v <= 1;
    }
    return true;
  };
  const auto source_copy = &source_array == &target ? source_array : std::vector<Value>{};
  const auto& source = source_copy.empty() ? source_array : source_copy;
  const auto color_at = [&](int x, int y) {
    const auto pixel = static_cast<uint32_t>(source[static_cast<size_t>(y) * source_width + x].i());
    return has_alpha ? pixel : pixel | 0xff000000;
  };
  const auto sample = [&](double sx, double sy) {
    if (!filter) return color_at(static_cast<int>(sx), static_cast<int>(sy));
    const double x = std::clamp(sx - 0.5, 0.0, double(source_width - 1));
    const double y = std::clamp(sy - 0.5, 0.0, double(source_height - 1));
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, source_width - 1), y1 = std::min(y0 + 1, source_height - 1);
    const double fx = x - x0, fy = y - y0;
    const uint32_t pixels[] = {color_at(x0, y0), color_at(x1, y0), color_at(x0, y1), color_at(x1, y1)};
    const double weights[] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    double alpha = 0;
    for (int i = 0; i < 4; ++i) alpha += (pixels[i] >> 24) * weights[i];
    if (alpha == 0) return uint32_t{};
    uint32_t pixel = static_cast<uint32_t>(std::lround(alpha)) << 24;
    for (unsigned shift : {0U, 8U, 16U}) {
      double channel = 0;
      for (int i = 0; i < 4; ++i) channel += ((pixels[i] >> shift) & 255) * (pixels[i] >> 24) * weights[i];
      pixel |= static_cast<uint32_t>(std::lround(channel / alpha)) << shift;
    }
    return pixel;
  };
  for (int y = top; y < bottom; ++y) {
    const double py = y + 0.5 - ty;
    for (int x = left; x < right; ++x) {
      if (!InsideClips(transform, x + 0.5, y + 0.5)) continue;
      const double px = x + 0.5 - tx;
      const double local_x = (d * px - b * py) / det, local_y = (a * py - c * px) / det;
      if (!(local_x >= dl && local_x < dr && local_y >= dt && local_y < db) || !in_mask(local_x, local_y)) continue;
      const double sx = sl + (local_x - dl) * (sr - sl) / (dr - dl);
      const double sy = st + (local_y - dt) * (sb - st) / (db - dt);
      if (!(sx >= 0 && sx < source_width && sy >= 0 && sy < source_height)) continue;
      const uint32_t pixel = sample(sx, sy);
      const uint32_t alpha = std::lround((pixel >> 24) * opacity);
      const uint32_t rgb = alpha_target ? 0 : (alpha_source ? tint : pixel) & 0x00ffffff;
      const size_t index = static_cast<size_t>(y) * width + x;
      target[index] = Value::Int(SourceOver(rgb | alpha << 24, target[index].i()));
    }
  }
  return {};
}

inline void AppendNetworkInt(std::vector<uint8_t>& bytes, uint32_t value) {
  for (unsigned shift : {24U, 16U, 8U, 0U}) bytes.push_back(static_cast<uint8_t>(value >> shift));
}

inline void AppendPngChunk(std::vector<uint8_t>& png, std::string_view tag, std::span<const uint8_t> data) {
  AppendNetworkInt(png, static_cast<uint32_t>(data.size()));
  const size_t start = png.size();
  png.insert(png.end(), tag.begin(), tag.end());
  png.insert(png.end(), data.begin(), data.end());
  AppendNetworkInt(png, crc32_z(0, png.data() + start, png.size() - start));
}

inline Value EncodePng(Runtime& vm, Object*, Values args) {
  const auto& pixels = vm.NonNull(args[0].ref()).elements;
  const int width = args[1].i(), height = args[2].i();
  const bool has_alpha = args[3].i();
  const uint64_t row_bytes = 1 + 4ULL * static_cast<uint32_t>(width);
  if (width <= 0 || height <= 0 || static_cast<uint64_t>(width) * height != pixels.size() ||
      row_bytes * height > INT32_MAX) {
    vm.Fail("Ljava/lang/IllegalArgumentException;", "Invalid PNG bitmap dimensions");
  }
  std::vector<uint8_t> rows(static_cast<size_t>(row_bytes) * height);
  for (int y = 0; y < height; ++y) {
    auto* out = rows.data() + static_cast<size_t>(y) * row_bytes + 1;
    for (int x = 0; x < width; ++x) {
      const auto argb = static_cast<uint32_t>(pixels[static_cast<size_t>(y) * width + x].i());
      *out++ = static_cast<uint8_t>(argb >> 16);
      *out++ = static_cast<uint8_t>(argb >> 8);
      *out++ = static_cast<uint8_t>(argb);
      *out++ = has_alpha ? static_cast<uint8_t>(argb >> 24) : 255;
    }
  }
  uLongf length = compressBound(static_cast<uLong>(rows.size()));
  if (length > INT32_MAX) return {};
  std::vector<uint8_t> compressed(length);
  if (compress(compressed.data(), &length, rows.data(), static_cast<uLong>(rows.size())) != Z_OK) return {};
  std::vector<uint8_t> png{137, 'P', 'N', 'G', 13, 10, 26, 10};
  std::vector<uint8_t> header;
  AppendNetworkInt(header, width);
  AppendNetworkInt(header, height);
  header.insert(header.end(), {8, 6, 0, 0, 0});  // RGBA8 with no interlacing.
  AppendPngChunk(png, "IHDR", header);
  AppendPngChunk(png, "IDAT", std::span(compressed).first(length));
  AppendPngChunk(png, "IEND", {});
  if (png.size() > INT32_MAX) return {};
  auto* result = vm.NewArray(vm.linker.Resolve("[B"), png.size());
  for (size_t i = 0; i < png.size(); ++i) result->elements[i] = Value::Int(static_cast<int8_t>(png[i]));
  return Value::Ref(result);
}

inline void Install(ClassLinker& linker) {
  auto& type = linker.Define("Lorg/wasmandroid/ui/SoftwareRaster;", kObject, art::kAccFinal);
  linker.AddMethod(type, "drawPath", "([III[FII[FIFIZII[F[F[IIFIIF)V", art::kAccStatic, DrawPath);
  linker.AddMethod(type, "extractAlpha", "([III[II)V", art::kAccStatic, ExtractAlpha);
  linker.AddMethod(type, "encodePng", "([IIIZ)[B", art::kAccStatic, EncodePng);
  linker.AddMethod(type, "drawBitmap", "([III[III[F[FFIZZZZIF)V", art::kAccStatic, DrawBitmap);
}

}  // namespace software_raster
}  // namespace wa::interp
