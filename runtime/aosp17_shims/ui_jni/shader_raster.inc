using interp::software_raster::Point;

int TilePixel(double coordinate, int size, jint mode) {
    if (mode == 0) return static_cast<int>(std::clamp(coordinate, 0.0, double(size - 1)));
    if (mode == 3) return coordinate < 0 || coordinate >= size ? -1 : static_cast<int>(coordinate);
    const double period = mode == 2 ? 2.0 * size : size;
    double wrapped = std::fmod(coordinate, period);
    if (wrapped < 0) wrapped += period;
    if (mode == 2 && wrapped >= size) wrapped = period - wrapped - 1;
    return static_cast<int>(wrapped);
}

uint32_t InterpolateColors(std::span<const uint32_t> colors, std::span<const double> weights) {
    double alpha = 0;
    std::array<double, 3> channels{};
    for (size_t i = 0; i < colors.size(); ++i) {
        const double weight = (colors[i] >> 24) * weights[i];
        alpha += weight;
        for (unsigned channel = 0; channel < channels.size(); ++channel)
            channels[channel] += ((colors[i] >> (channel * 8)) & 255) * weight;
    }
    if (alpha < 0.5) return 0;
    auto result = static_cast<uint32_t>(std::lround(alpha)) << 24;
    for (unsigned channel = 0; channel < channels.size(); ++channel)
        result |= static_cast<uint32_t>(std::lround(channels[channel] / alpha)) << (channel * 8);
    return result;
}

uint32_t SampleBitmap(const BitmapData& bitmap, jint tile_x, jint tile_y, bool filter,
                      float x, float y, uint32_t paint_color) {
    if (bitmap.pixels.empty() || !std::isfinite(x) || !std::isfinite(y)) return 0;
    const auto pixel = [&](double px, double py) -> uint32_t {
        const int sx = TilePixel(px, bitmap.width, tile_x);
        const int sy = TilePixel(py, bitmap.height, tile_y);
        if (sx < 0 || sy < 0) return 0;
        const auto color = static_cast<uint32_t>(bitmap.pixels[static_cast<size_t>(sy) * bitmap.width + sx]);
        if (bitmap.config == 1) return (color & 0xff000000u) | (paint_color & 0xffffffu);
        return bitmap.has_alpha ? color : color | 0xff000000u;
    };
    if (!filter) return pixel(std::floor(x), std::floor(y));
    const double px = double(x) - 0.5, py = double(y) - 0.5;
    const double left = std::floor(px), top = std::floor(py);
    const double fx = px - left, fy = py - top;
    const std::array colors{pixel(left, top), pixel(left + 1, top),
                            pixel(left, top + 1), pixel(left + 1, top + 1)};
    const std::array weights{(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    return InterpolateColors(colors, weights);
}

uint32_t SampleGradientShader(const GradientData& gradient, float sx, float sy) {
    const auto [x0, y0, x1, y1] = gradient.geometry;
    const double dx = sx - x0, dy = sy - y0;
    double t = 0;
    if (gradient.type == 1) {
        const double vx = x1 - x0, vy = y1 - y0;
        const double length = vx * vx + vy * vy;
        t = length > 0 ? (dx * vx + dy * vy) / length : 1;
    } else if (gradient.type == 2) {
        t = std::hypot(dx, dy) / x1;
    } else {
        t = std::atan2(dy, dx) / (2 * std::numbers::pi);
        if (t < 0) t += 1;
    }
    if (!std::isfinite(t) || (gradient.tile == 3 && (t < 0 || t > 1))) return 0;
    if (gradient.tile == 1) t -= std::floor(t);
    if (gradient.tile == 2) t = 1 - std::abs(t - 2 * std::floor(t / 2) - 1);
    const auto& stops = gradient.positions;
    if (t <= stops.front()) return gradient.colors.front();
    if (t >= stops.back()) return gradient.colors.back();
    const auto index = std::ranges::upper_bound(stops, t) - stops.begin();
    const double weight = (t - stops[index - 1]) / (stops[index] - stops[index - 1]);
    const std::array colors{uint32_t(gradient.colors[index - 1]), uint32_t(gradient.colors[index])};
    const std::array weights{1 - weight, weight};
    return InterpolateColors(colors, weights);
}

uint32_t SampleShader(const ShaderData& shader, float x, float y, uint32_t paint_color) {
    if (!shader.inverse) return 0;
    const auto [sx, sy] = MapPoint(*shader.inverse, x, y);
    if (const auto* image = std::get_if<BitmapShaderData>(&shader.content))
        return SampleBitmap(*image->bitmap, image->tile_x, image->tile_y, image->filter, sx, sy, paint_color);
    if (const auto* compose = std::get_if<ComposeShaderData>(&shader.content)) {
        return BlendColor(SampleShader(*compose->source, sx, sy, paint_color),
                          SampleShader(*compose->destination, sx, sy, paint_color), compose->mode);
    }
    return SampleGradientShader(std::get<GradientData>(shader.content), sx, sy);
}

bool ContainsPath(const PathClip& path, float x, float y) {
    int winding = 0;
    for (const auto& [from, to] : path.edges) {
        if (y < std::min(from.y, to.y) || y >= std::max(from.y, to.y)) continue;
        if (x < from.x + (to.x - from.x) * (y - from.y) / (to.y - from.y))
            winding += from.y < to.y ? 1 : -1;
    }
    return path.even_odd ? winding % 2 != 0 : winding != 0;
}

float ClipOpacity(const CanvasData::Frame& frame, interp::Values transform, float x, float y) {
    if (!interp::software_raster::InsideClips(transform, x, y)) return 0;
    float alpha = 1;
    for (const auto& clip : frame.masks) {
        const auto [sx, sy] = MapPoint(clip->inverse, x, y);
        float coverage;
        if (const auto* path = std::get_if<PathClip>(&clip->source)) {
            coverage = ContainsPath(*path, sx, sy);
        } else {
            const auto& shader = std::get<std::shared_ptr<const ShaderData>>(clip->source);
            coverage = (SampleShader(*shader, sx, sy, 0xff000000u) >> 24) / 255.f;
        }
        alpha *= clip->difference ? 1 - coverage : coverage;
        if (alpha == 0) break;
    }
    return alpha;
}

uint32_t BlendWithCoverage(uint32_t source, uint32_t destination, jint blend, double coverage) {
    const auto blended = BlendColor(source, destination, blend);
    if (coverage == 1) return blended;
    const std::array colors{destination, blended};
    const std::array weights{1 - coverage, coverage};
    return InterpolateColors(colors, weights);
}

struct RasterBounds {
    int left, top, right, bottom;
    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
};

RasterBounds PathRasterBounds(const CanvasData& canvas, const PathGeometry& path, const PaintData& paint) {
    auto bounds = canvas.frame.clip;
    if (path.fill < 2) {
        auto shape = path.Bounds();
        const float stroke = paint.style == 0 ? 0.f : paint.stroke_width * std::max(1.f, paint.miter) / 2;
        shape = MapRect(canvas.frame.matrix, {shape.l - stroke, shape.t - stroke,
                                             shape.r + stroke, shape.b + stroke});
        const float margin = 1 + (paint.mask_filter ? std::bit_cast<float>(uint32_t(paint.mask_filter)) : 0.f);
        bounds = Intersect(bounds, {shape.l - margin, shape.t - margin, shape.r + margin, shape.b + margin});
    }
    if (!HasArea(bounds)) return {};
    return {int(std::max(0.f, std::floor(bounds.l))), int(std::max(0.f, std::floor(bounds.t))),
            int(std::min(float(canvas.width), std::ceil(bounds.r))),
            int(std::min(float(canvas.height), std::ceil(bounds.b)))};
}

std::vector<uint8_t> RasterizeCoverage(const PathGeometry& path, const PaintData& paint,
                                       interp::Values transform, RasterBounds bounds) {
    using namespace interp::software_raster;
    const int width = bounds.Width(), height = bounds.Height();
    std::vector<uint8_t> coverage(static_cast<size_t>(width) * height);
    auto contours = Flatten(RasterPathValues(path), path.verbs.size());
    std::array<std::vector<Edge>, 2> edges;
    if (paint.style != 1) {
        for (const auto& contour : contours) AppendEdges(edges[0], contour.points, transform, false);
    }
    if (paint.style != 0)
        edges[1] = StrokeEdges(std::move(contours), transform, paint.stroke_width, paint.cap, paint.join, paint.miter);
    std::vector<std::tuple<double, int, int>> crossings;
    crossings.reserve(edges[0].size() + edges[1].size());
    for (int row = 0; row < height; ++row) {
        const double scan = bounds.top + row + .5;
        crossings.clear();
        for (int channel = 0; channel < 2; ++channel) {
            for (const auto& [a, b] : edges[channel]) {
                if (scan < std::min(a.y, b.y) || scan >= std::max(a.y, b.y)) continue;
                const double x = a.x + (b.x - a.x) * (scan - a.y) / (b.y - a.y);
                crossings.emplace_back(x, a.y < b.y ? 1 : -1, channel);
            }
        }
        std::ranges::sort(crossings);
        auto* result = coverage.data() + static_cast<size_t>(row) * width;
        std::array<int, 2> winding{};
        int begin = 0;
        const auto fill_to = [&](int end) {
            const bool fill = path.fill & 1 ? winding[0] % 2 != 0 : winding[0] != 0;
            if ((paint.style != 1 && fill == (path.fill < 2)) || winding[1] != 0)
                std::fill(result + begin, result + end, 255);
            begin = end;
        };
        for (auto [x, sign, channel] : crossings) {
            fill_to(static_cast<int>(std::clamp(std::ceil(x - .5) - bounds.left, 0.0, double(width))));
            winding[channel] += sign;
        }
        fill_to(width);
    }
    const uint64_t mask = paint.mask_filter;
    const float blur = mask ? std::bit_cast<float>(uint32_t(mask)) : 0.f;
    if (blur > 0 && std::isfinite(blur)) {
        const int radius = std::min<double>(std::ceil(blur), std::max(width, height));
        std::vector<uint8_t> horizontal(coverage.size()), blurred(coverage.size());
        BoxBlur(coverage, horizontal, height, width, 1, width, radius);
        BoxBlur(horizontal, blurred, width, height, width, 1, radius);
        const auto style = (mask >> 32) - 1;
        for (size_t i = 0; i < coverage.size(); ++i) {
            if (style == 1) blurred[i] = std::max(blurred[i], coverage[i]);
            if ((style == 2 && coverage[i]) || (style == 3 && !coverage[i])) blurred[i] = 0;
        }
        return blurred;
    }
    return coverage;
}

// Use the same path scan conversion for both targets. A recording owns the
// sampled pixels so later bitmap, shader and paint changes cannot affect it.
template <class Sample>
void RasterizePaintedPath(const CanvasTarget& canvas, const PathGeometry& path,
                          const PaintData& paint, Sample&& sample, std::span<const uint8_t> mask = {}) {
    const auto& frame = canvas.data->frame;
    const auto inverse = Inverse(frame.matrix);
    auto bounds = PathRasterBounds(*canvas.data, path, paint);
    if (canvas.bitmap) {
        bounds.right = std::min(bounds.right, canvas.bitmap->width);
        bounds.bottom = std::min(bounds.bottom, canvas.bitmap->height);
    }
    const int width = bounds.Width(), height = bounds.Height();
    if (!inverse || width <= 0 || height <= 0 || (!canvas.bitmap && !canvas.display)) return;

    const auto clips = RasterTransform(frame);
    const auto coverage = RasterizeCoverage(path, paint, clips.elements, bounds);
    std::vector<jint> pixels(coverage.size());
    bool visible = false;
    for (int row = 0; row < height; ++row) {
        const int y = bounds.top + row;
        const auto* original = canvas.bitmap ?
            canvas.bitmap->pixels.data() + static_cast<size_t>(y) * canvas.bitmap->width + bounds.left : nullptr;
        auto* output = pixels.data() + static_cast<size_t>(row) * width;
        if (original) std::copy_n(original, width, output);
        for (int column = 0; column < width; ++column) {
            const int x = bounds.left + column;
            const size_t index = static_cast<size_t>(row) * width + column;
            const auto mask_alpha = coverage[index];
            if (!mask_alpha || x + 0.5f < frame.clip.l || x + 0.5f >= frame.clip.r ||
                y + 0.5f < frame.clip.t || y + 0.5f >= frame.clip.b) continue;
            double opacity = mask_alpha / 255.0 * ClipOpacity(frame, clips.elements, x + .5f, y + .5f);
            if (!mask.empty())
                opacity *= mask[static_cast<size_t>(y) * canvas.bitmap->width + x] / 255.0;
            if (opacity == 0) continue;
            const auto local = MapPoint(*inverse, x + .5f, y + .5f);
            const auto color = sample(local[0], local[1]);
            if (original) {
                auto& pixel = output[column];
                pixel = canvas.bitmap->PutColor(BlendWithCoverage(color, pixel, paint.blend, opacity));
                visible = true;
            } else {
                pixels[index] = Opacity(color, opacity);
                visible |= (uint32_t(pixels[index]) >> 24) != 0;
            }
        }
    }
    if (!visible) return;
    if (canvas.bitmap) {
        // Keep the source pixels stable while sampling a bitmap into itself.
        for (int row = 0; row < height; ++row) {
            auto* output = canvas.bitmap->pixels.data() +
                static_cast<size_t>(bounds.top + row) * canvas.bitmap->width + bounds.left;
            std::copy_n(pixels.data() + static_cast<size_t>(row) * width, width, output);
        }
        canvas.bitmap->Changed();
        return;
    }
    const auto image = PublishBitmapPixels(*canvas.state, pixels, width, height, 0, 0, true);
    if (!image) return;
    canvas.state->recording_images.push_back(image);
    auto& result = canvas.state->result;
    result = wa_canvas_save(canvas.display);
    if (result != WA_SF_OK) return;
    EmitMatrix(canvas, *inverse);
    if (result == WA_SF_OK) {
        const wa_canvas_bitmap_t draw{image, WA_MASK_RECT, {0, 0, float(width), float(height)},
            {float(bounds.left), float(bounds.top), float(bounds.right), float(bounds.bottom)}, 0, 1};
        result = wa_canvas_draw_bitmap(canvas.display, &draw);
    }
    const auto restored = wa_canvas_restore(canvas.display);
    if (result == WA_SF_OK) result = restored;
}

void RasterizePath(const CanvasTarget& canvas, const PathGeometry& path, const PaintData& paint,
                   const GradientData* gradient = nullptr, std::span<const uint8_t> mask = {}) {
    const float alpha = canvas.data->frame.alpha;
    const auto solid = Opacity(FilterColor(paint, paint.color), alpha);
    RasterizePaintedPath(canvas, path, paint, [&](float x, float y) {
        if (!paint.shader && !gradient) return solid;
        const auto color = gradient ? SampleGradientShader(*gradient, x, y)
                                    : SampleShader(*paint.shader, x, y, paint.color);
        return Opacity(FilterColor(paint, color), alpha * (paint.color >> 24) / 255.f);
    }, mask);
}

void RasterizeBitmap(const CanvasTarget& canvas, const BitmapData& bitmap, wa_rect_t src, wa_rect_t dst,
                     const PaintData& paint, jint mask, float radius) {
    const auto& frame = canvas.data->frame;
    const auto inverse = Inverse(frame.matrix);
    const auto area = Intersect(MapRect(frame.matrix, dst), frame.clip);
    if (!inverse || !HasArea(area)) return;
    auto& target = *canvas.bitmap;
    const auto clamp = [](float x, int limit) {
        return static_cast<int>(std::clamp(std::ceil(x - .5f), 0.f, float(limit)));
    };
    const int l = clamp(area.l, target.width), r = clamp(area.r, target.width);
    const int t = clamp(area.t, target.height), b = clamp(area.b, target.height);
    if (l >= r || t >= b) return;
    const auto snapshot = &bitmap == &target ? std::optional<BitmapData>(bitmap) : std::nullopt;
    const auto& source = snapshot ? *snapshot : bitmap;
    const auto clips = RasterTransform(frame);
    const float opacity = frame.alpha * (paint.color >> 24) / 255.f;
    const float half_width = (dst.r - dst.l) / 2, half_height = (dst.b - dst.t) / 2;
    const float corner = std::clamp(radius, 0.f, std::min(half_width, half_height));
    const auto inside = [&](float x, float y) {
        const float dx = std::abs(x - dst.l - half_width), dy = std::abs(y - dst.t - half_height);
        if (mask == WA_MASK_ROUNDED_RECT && corner > 0)
            return std::hypot(std::max(0.f, dx - half_width + corner),
                              std::max(0.f, dy - half_height + corner)) <= corner;
        const float u = dx / half_width, v = dy / half_height;
        if (mask == WA_MASK_CIRCLE) return u * u + v * v <= 1;
        if (mask == WA_MASK_SQUIRCLE) return u * u * u * u + v * v * v * v <= 1;
        return true;
    };
    bool changed = false;
    for (int y = t; y < b; ++y) {
        for (int x = l; x < r; ++x) {
            const float device_x = x + .5f, device_y = y + .5f;
            const auto [px, py] = MapPoint(*inverse, device_x, device_y);
            if (!(px >= dst.l && px < dst.r && py >= dst.t && py < dst.b) || !inside(px, py)) continue;
            const float sx = std::lerp(src.l, src.r, (px - dst.l) / (dst.r - dst.l));
            const float sy = std::lerp(src.t, src.b, (py - dst.t) / (dst.b - dst.t));
            if (!(sx >= 0 && sx < source.width && sy >= 0 && sy < source.height)) continue;
            const auto coverage = ClipOpacity(frame, clips.elements, device_x, device_y);
            if (coverage == 0) continue;
            const auto color = SampleBitmap(source, 0, 0, paint.flags & 2, sx, sy, paint.color);
            auto& out = target.pixels[static_cast<size_t>(y) * target.width + x];
            out = target.PutColor(BlendWithCoverage(Opacity(FilterColor(paint, color), opacity), out,
                                                    paint.blend, coverage));
            changed = true;
        }
    }
    if (changed) target.Changed();
}
