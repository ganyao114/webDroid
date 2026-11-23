#include <wasmandroid/launcher_session.h>

#include "art_sf_entry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, std::string_view detail) {
    if (!condition) throw std::runtime_error(std::string(detail));
}

void Check(int status, const char* operation) {
    if (status == WA_OK) return;
    char error[4096];
    wa_system_launcher_last_error(error, sizeof(error));
    throw std::runtime_error(std::string(operation) + " (" + std::to_string(status) + ") " + error);
}

struct File {
    explicit File(const char* path) : name(std::filesystem::path(path).filename().string()) {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        Require(static_cast<bool>(stream), path);
        bytes.resize(stream.tellg());
        stream.seekg(0);
        Require(static_cast<bool>(stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size())), path);
    }
    void Install() const {
        Check(wa_system_launcher_install_apk(name.c_str(), bytes.data(), bytes.size()), "install");
    }
    std::string name;
    std::vector<uint8_t> bytes;
};

struct Position { float x, y; };

struct State {
    State() { Check(wa_system_launcher_state_json(text, sizeof(text)), "state"); }

    std::string_view Value(const char* key) const {
        const std::string field = std::string("\"") + key + "\":";
        const std::string_view json(text);
        const auto at = json.find(field);
        Require(at != json.npos, field);
        return json.substr(at + field.size());
    }
    int Integer(const char* key) const { return std::atoi(Value(key).data()); }
    double Number(const char* key) const { return std::atof(Value(key).data()); }
    bool Boolean(const char* key) const { return Value(key).starts_with("true"); }
    bool Is(const char* key, const char* value) const {
        return Value(key).starts_with(std::string("\"") + value + "\"");
    }
    Position Button(const char* key) const {
        Position center{};
        Require(std::sscanf(Value(key).data(), "{\"x\":%f,\"y\":%f}", &center.x, &center.y) == 2, text);
        return center;
    }
    wa_rect_t SearchBounds() const {
        wa_rect_t bounds{};
        Require(std::sscanf(Value("searchBounds").data(),
            "{\"left\":%f,\"top\":%f,\"right\":%f,\"bottom\":%f}",
            &bounds.l, &bounds.t, &bounds.r, &bounds.b) == 4, text);
        return bounds;
    }
    int NavigationInset() const {
        int top, bottom;
        Require(std::sscanf(Value("systemUiInsets").data(), "{\"top\":%d,\"bottom\":%d}", &top, &bottom) == 2, text);
        return bottom;
    }
    void Print(const char* locale, const char* phase) const {
        std::printf("%s %s %s\n", locale, phase, text);
    }
    char text[4096];
};

struct Transform {
    Position At(float x, float y) const { return {tx + a * x + c * y, ty + b * x + d * y}; }
    wa_rect_t Bounds(wa_rect_t rect) const {
        const auto tl = At(rect.l, rect.t), tr = At(rect.r, rect.t);
        const auto bl = At(rect.l, rect.b), br = At(rect.r, rect.b);
        return {std::min({tl.x, tr.x, bl.x, br.x}), std::min({tl.y, tr.y, bl.y, br.y}),
                std::max({tl.x, tr.x, bl.x, br.x}), std::max({tl.y, tr.y, bl.y, br.y})};
    }
    void Translate(float x, float y) {
        const auto next = At(x, y);
        tx = next.x;
        ty = next.y;
    }
    void Scale(float x, float y) { a *= x; b *= x; c *= y; d *= y; }
    void Rotate(wa_canvas_rotation_t rotation) {
        const float angle = rotation.degrees * 0.01745329252f;
        const auto center = At(rotation.px, rotation.py);
        const float cosine = std::cos(angle), sine = std::sin(angle);
        const float next_a = a * cosine + c * sine, next_b = b * cosine + d * sine;
        c = c * cosine - a * sine;
        d = d * cosine - b * sine;
        a = next_a;
        b = next_b;
        tx = center.x - a * rotation.px - c * rotation.py;
        ty = center.y - b * rotation.px - d * rotation.py;
    }
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
};

struct Path {
    Position start, position;
};

float Width(wa_rect_t bounds) { return bounds.r - bounds.l; }
float Height(wa_rect_t bounds) { return bounds.b - bounds.t; }
float CenterX(wa_rect_t bounds) { return (bounds.l + bounds.r) / 2; }
bool HasArea(wa_rect_t bounds) { return Width(bounds) > 0 && Height(bounds) > 0; }
wa_rect_t Intersect(wa_rect_t a, wa_rect_t b) {
    return {std::max(a.l, b.l), std::max(a.t, b.t), std::min(a.r, b.r), std::min(a.b, b.b)};
}
constexpr float kGeometryTolerance = 0.02f;
bool SameBounds(wa_rect_t a, wa_rect_t b, float tolerance = kGeometryTolerance) {
    return std::abs(a.l - b.l) <= tolerance && std::abs(a.t - b.t) <= tolerance &&
        std::abs(a.r - b.r) <= tolerance && std::abs(a.b - b.b) <= tolerance;
}

struct Text {
    std::string label;
    Position position;
    wa_rect_t line, clip;
};

struct Bitmap {
    uint32_t id;
    wa_rect_t bounds, clip;
};

struct Rounded {
    wa_rect_t bounds, clip;
    float rx, ry;
    uint32_t alpha, style;
};

bool MatchesLabel(std::string_view drawn, std::string_view label) {
    constexpr std::string_view ellipsis_padding = "\xef\xbb\xbf";
    while (drawn.ends_with(ellipsis_padding)) drawn.remove_suffix(ellipsis_padding.size());
    if (drawn == label) return true;
    constexpr std::string_view ellipsis = "…";
    const auto end = drawn.find_last_not_of(' ');
    if (end == drawn.npos) return false;
    const auto prefix = drawn.substr(0, end + 1);
    return (label.size() > prefix.size() && label.starts_with(prefix) && label[prefix.size()] == ' ') ||
        (prefix.size() > ellipsis.size() && prefix.ends_with(ellipsis) &&
         label.starts_with(prefix.substr(0, prefix.size() - ellipsis.size())));
}

struct Layer {
    bool Has(uint16_t opcode) const { return opcodes[opcode]; }
    Position Find(std::string_view label) const {
        for (auto it = labels.rbegin(); it != labels.rend(); ++it) {
            if (MatchesLabel(it->label, label)) return it->position;
        }
        std::string message = "Rendered label missing: " + std::string(label) + "\nDisplayed text:";
        for (const auto& text : labels) {
            message += " [" + text.label + " at " + std::to_string(text.position.x) + "," +
                std::to_string(text.position.y) + "]";
        }
        throw std::runtime_error(message);
    }
    bool HasPath(float x, float y, float top, float bottom) const {
        return std::ranges::any_of(paths, [&](const Path& path) {
            return std::abs(path.start.x - x) < 0.0001f && std::abs(path.start.y - y) < 0.0001f &&
                path.position.y >= top && path.position.y < bottom;
        });
    }
    std::array<bool, WA_CANVAS_DRAW_TEXT + 1> opcodes{};
    std::vector<Text> labels;
    std::vector<Path> paths;
    std::vector<Bitmap> bitmaps;
    std::vector<Rounded> rounds;
    bool gesture_handle = false;
    bool sheet_outline = false;
};

struct Frame {
    explicit Frame(wa_sf_t* scene) : scene(scene), packet(wa_sf_frame_acquire(scene)) {
        Require(packet != nullptr, "Published SurfaceFlinger packet");
    }
    ~Frame() { wa_sf_frame_release(scene, packet); }
    wa_sf_t* scene;
    const wa_sf_frame_packet_t* packet;
};

template<class Record>
Record ReadRecord(const uint8_t* source, size_t available) {
    Require(available >= sizeof(Record), "Truncated display list record");
    Record result;
    std::memcpy(&result, source, sizeof(result));
    return result;
}

std::array<Layer, 2> Inspect(wa_sf_t* scene, uint32_t width = 960, uint32_t height = 660, uint32_t dpi = 160) {
    Frame guard(scene);
    const auto* frame = guard.packet;
    Require(frame->magic == WA_SF_FRAME_MAGIC && frame->layer_count == 2, "Wallpaper and Android window layers");
    Require(frame->viewport_w == width && frame->viewport_h == height && frame->density_dpi == dpi, "Viewport");
    const auto* layers = wa_sf_frame_layers(frame);
    std::array<Layer, 2> result;
    for (size_t index = 0; index < result.size(); ++index) {
        const auto& layer = layers[index];
        Require(layer.desc.z_order == static_cast<int32_t>(index) &&
                layer.desc.surface_kind == WA_SF_SURFACE_DISPLAY_LIST, "Display layer stacking");
        auto& details = result[index];
        const uint8_t* list = wa_sf_frame_list(frame, &layer);
        Require(list != nullptr && layer.list_size, "Display list");
        struct CanvasState { Transform transform; wa_rect_t clip; };
        CanvasState state{{}, {0, 0, static_cast<float>(width), static_cast<float>(height)}};
        auto& [transform, clip] = state;
        std::vector<CanvasState> stack;
        for (size_t offset = 0; offset < layer.list_size;) {
            const auto* record = list + offset;
            const auto header = ReadRecord<wa_canvas_record_t>(record, layer.list_size - offset);
            Require(header.byte_size >= sizeof(header) && header.byte_size <= layer.list_size - offset &&
                    header.opcode > 0 && header.opcode <= WA_CANVAS_DRAW_TEXT, "Display list record framing");
            const auto* payload = record + sizeof(header);
            const auto length = header.byte_size - sizeof(header);
            details.opcodes[header.opcode] = true;
            switch (header.opcode) {
            case WA_CANVAS_SAVE: stack.push_back(state); break;
            case WA_CANVAS_RESTORE:
                Require(!stack.empty(), "Canvas restore");
                state = stack.back();
                stack.pop_back();
                break;
            case WA_CANVAS_TRANSLATE: {
                const auto xy = ReadRecord<wa_canvas_xy_t>(payload, length);
                transform.Translate(xy.x, xy.y);
                break;
            }
            case WA_CANVAS_SCALE: {
                const auto xy = ReadRecord<wa_canvas_xy_t>(payload, length);
                transform.Scale(xy.x, xy.y);
                break;
            }
            case WA_CANVAS_ROTATE:
                transform.Rotate(ReadRecord<wa_canvas_rotation_t>(payload, length));
                break;
            case WA_CANVAS_CLIP_RECT:
            case WA_CANVAS_CLIP_ROUND_RECT:
                clip = Intersect(clip, transform.Bounds(ReadRecord<wa_rect_t>(payload, length)));
                break;
            case WA_CANVAS_DRAW_PATH: {
                const auto path = ReadRecord<wa_canvas_path_t>(payload, length);
                const auto alpha = path.paint.argb >> 24;
                details.sheet_outline |= path.paint.style == WA_PAINT_STROKE && path.paint.stroke_width == 1 &&
                    alpha > 0 && alpha <= 0x99 && path.verb_count >= 6;
                const auto verb = ReadRecord<uint32_t>(payload + sizeof(path), length - sizeof(path));
                const auto start = ReadRecord<Position>(payload + sizeof(path) + sizeof(verb),
                    length - sizeof(path) - sizeof(verb));
                if (verb == WA_PATH_MOVE && path.paint.argb >> 24)
                    details.paths.push_back({start, transform.At(start.x, start.y)});
                break;
            }
            case WA_CANVAS_DRAW_ROUND_RECT: {
                const auto round = ReadRecord<wa_canvas_round_draw_t>(payload, length);
                const auto bounds = transform.Bounds(round.shape.rect);
                const auto alpha = round.paint.argb >> 24;
                if (alpha && HasArea(Intersect(bounds, clip))) {
                    details.rounds.push_back({bounds, clip, round.shape.rx, round.shape.ry, alpha, round.paint.style});
                }
                details.gesture_handle |= bounds.t > height - 32 && bounds.b <= height &&
                    Width(bounds) > 60 && Height(bounds) <= 8 && round.shape.rx > 0 && alpha != 0;
                break;
            }
            case WA_CANVAS_DRAW_CIRCLE: {
                const auto circle = ReadRecord<wa_canvas_circle_t>(payload, length);
                const auto [x, y, r, paint] = circle;
                const auto bounds = transform.Bounds({x - r, y - r, x + r, y + r});
                if ((paint.argb >> 24) && HasArea(Intersect(bounds, clip))) {
                    details.rounds.push_back({bounds, clip, r, r, paint.argb >> 24, paint.style});
                }
                break;
            }
            case WA_CANVAS_DRAW_BITMAP: {
                const auto bitmap = ReadRecord<wa_canvas_bitmap_t>(payload, length);
                const auto bounds = transform.Bounds(bitmap.dst);
                if (bitmap.alpha > 0 && HasArea(Intersect(bounds, clip))) {
                    details.bitmaps.push_back({bitmap.bitmap_id, bounds, clip});
                }
                break;
            }
            case WA_CANVAS_DRAW_TEXT: {
                const auto text = ReadRecord<wa_canvas_text_t>(payload, length);
                Require(text.utf8_offset <= header.byte_size &&
                        text.utf8_len <= header.byte_size - text.utf8_offset, "Display text bounds");
                const auto point = transform.At(text.x, text.y);
                if (point.x >= 0 && point.x < width && point.y >= 0 && point.y < height && (text.argb >> 24)) {
                    const auto* utf8 = reinterpret_cast<const char*>(record + text.utf8_offset);
                    const auto metrics = wa_canvas_measure_text(scene, utf8, text.utf8_len,
                        text.font_size, text.weight, text.font_face);
                    const float width = text.max_width > 0 ? std::min(metrics.advance, text.max_width)
                                                           : metrics.advance;
                    const float left = text.x - width * text.align / 2.f;
                    const auto line = transform.Bounds({left, text.y - metrics.ascent,
                        left + width, text.y + metrics.descent});
                    details.labels.push_back({std::string(utf8, text.utf8_len),
                        transform.At(left + width / 2, text.y), line, clip});
                }
                break;
            }
            }
            offset += header.byte_size;
        }
        Require(stack.empty(), "Balanced canvas state");
    }
    return result;
}

void CheckSystemUiFrame(const Layer& window, float height, bool gestural = false) {
    Require(std::ranges::any_of(window.labels, [](const auto& label) {
        const auto& text = label.label;
        return label.position.y < 24 && text.size() == 5 && text[2] == ':' &&
            text.find_first_not_of("0123456789:") == std::string::npos && std::count(text.begin(), text.end(), ':') == 1;
    }), "SystemUI Clock text in the Android window");
    Require(window.Find("100%").y < 24 && window.HasPath(3.5f, 2, 0, 24), "SystemUI battery text and perimeter");
    Require(window.HasPath(12.659f, 7.45f, 0, 24) && window.HasPath(14.749f, 0, 0, 24),
        "SystemUI Wi-Fi and full cellular signal vectors");
    const bool back = window.HasPath(15.5417f, 1.66669f, height - 48, height);
    const bool home = window.HasPath(10.0001f, 18.3334f, height - 48, height);
    const bool recents = window.HasPath(4.47634f, 2.5f, height - 48, height);
    Require(gestural ? window.gesture_handle && !back && !home && !recents : back && home && recents,
        "Upstream navigation icon geometry for the active mode");
}

void PublishBrowserFontMetrics(wa_sf_t* scene) {
    auto* fonts = wa_sf_font_metrics(scene);
    for (int i = 0; i < WA_FONT_FACE_COUNT * WA_FONT_WEIGHT_COUNT; ++i) {
        // Browser font calibration can arrive after the first frame.
        fonts[i] = {2, 35389, 65536, 60416, 15360};
    }
}

std::vector<std::string_view> CheckIconLabel(const Layer& window, const Bitmap& icon) {
    std::vector<std::string_view> labels;
    const auto& bounds = icon.bounds;
    for (const auto& text : window.labels) {
        const auto [x, y] = text.position;
        if (std::abs(x - CenterX(bounds)) >= 1 || y <= bounds.b || y >= bounds.b + 36 ||
                x < text.clip.l || x >= text.clip.r) continue;
        Require(text.line.t >= bounds.b - kGeometryTolerance &&
                text.line.t >= text.clip.t - kGeometryTolerance &&
                text.line.b <= text.clip.b + kGeometryTolerance,
            "Clipped or misplaced app label " + text.label + " at baseline " + std::to_string(y) +
                " with line bottom " + std::to_string(text.line.b) + " and clip bottom " + std::to_string(text.clip.b));
        labels.push_back(text.label);
    }
    Require(!labels.empty(), "Visible label centered below each app icon");
    return labels;
}

std::vector<Rounded> PaginationDots(const Layer& window, int width, float hotseat_top) {
    std::vector<Rounded> dots;
    for (const auto& shape : window.rounds) {
        const auto bounds = shape.bounds;
        if (shape.style == WA_PAINT_FILL && Height(bounds) >= 4 && Height(bounds) <= 10 &&
                Width(bounds) <= 24 && bounds.b <= hotseat_top && bounds.b >= hotseat_top - 48 &&
                std::abs(CenterX(bounds) - width / 2.f) < 24) {
            dots.push_back(shape);
        }
    }
    std::ranges::sort(dots, {}, [](const Rounded& dot) { return dot.bounds.l; });
    return dots;
}

struct HomeGeometry {
    float hotseat_top;
    std::vector<Rounded> dots;
};

void CheckAppLabels(const Layer& window, const std::vector<Bitmap>& icons,
                    std::span<const std::string_view> expected) {
    std::vector<std::vector<std::string_view>> drawn;
    for (const auto& icon : icons) drawn.push_back(CheckIconLabel(window, icon));
    Require(drawn.size() == expected.size(), "All expected APK icons draw");
    for (const auto name : expected) {
        Require(std::ranges::count_if(drawn, [&](const auto& labels) {
            return std::ranges::any_of(labels, [&](auto label) { return MatchesLabel(label, name); });
        }) == 1, "Expected one app icon with label " + std::string(name));
    }
}

HomeGeometry CheckHome(const Layer& window, int width, int height, int page, int navigation_inset,
                       std::span<const std::string_view> expected) {
    const auto qsb = std::ranges::find_if(window.rounds, [&](const Rounded& shape) {
        return shape.style == WA_PAINT_FILL && Width(shape.bounds) > width / 2.f &&
            shape.bounds.t > height * .65f && shape.bounds.b <= height - navigation_inset;
    });
    Require(qsb != window.rounds.end() && SameBounds(qsb->bounds, Intersect(qsb->bounds, qsb->clip)),
        "Entire Hotseat Search pill is visible above SystemUI");
    Require(std::ranges::any_of(window.rounds, [&](const Rounded& shape) {
        return shape.style == WA_PAINT_STROKE && SameBounds(shape.bounds, qsb->bounds);
    }), "Hotseat Search pill has a matching outline");
    const auto label = std::ranges::find_if(window.labels, [](const Text& text) {
        return text.label == "Search" || text.label == "搜索";
    });
    Require(label != window.labels.end(), "Visible QSB label");
    Require(std::abs((label->line.t + label->line.b) - (qsb->bounds.t + qsb->bounds.b)) <= 2.f,
        "Hotseat Search label is vertically centered");

    float bottom = 0;
    for (const auto& icon : window.bitmaps) bottom = std::max(bottom, icon.bounds.b);
    float hotseat_top = bottom;
    int hotseat_count = 0;
    for (const auto& icon : window.bitmaps) {
        if (std::abs(icon.bounds.b - bottom) > kGeometryTolerance) continue;
        ++hotseat_count;
        hotseat_top = std::min(hotseat_top, icon.bounds.t);
        Require(qsb->bounds.t - icon.bounds.b >= 4.f - kGeometryTolerance,
            "Hotseat icon has less than 4 px QSB clearance");
    }
    Require(hotseat_count == 5, "All five upstream Hotseat icons draw");

    auto dots = PaginationDots(window, width, hotseat_top);
    Require(dots.size() == 2, "Both workspace page dots draw on Home");
    for (const auto& dot : dots)
        Require(SameBounds(dot.bounds, Intersect(dot.bounds, dot.clip)), "Page indicator dots fit their clips");
    Require(dots[page].alpha > dots[1 - page].alpha &&
        Width(dots[page].bounds) > Width(dots[1 - page].bounds), "Current page dot is active");

    std::vector<Bitmap> workspace;
    for (const auto& icon : window.bitmaps) {
        if (icon.bounds.b <= dots[0].bounds.t) workspace.push_back(icon);
    }
    CheckAppLabels(window, workspace, expected);
    return {hotseat_top, std::move(dots)};
}

uint32_t IconFingerprint(wa_sf_t* scene, uint32_t id) {
    auto* store = wa_sf_bitmaps(scene);
    const auto* view = wa_bitmap_acquire(store, id);
    Require(view != nullptr, "Bitmap pixels are available");
    const auto* pixels = wa_bitmap_pixels(view);
    uint32_t hash = 2166136261;
    size_t visible = 0;
    for (size_t i = 0; i + 3 < view->byte_size; i += 4) {
        uint32_t rgba;
        std::memcpy(&rgba, pixels + i, sizeof(rgba));
        visible += pixels[i + 3] != 0;
        hash = (hash ^ rgba) * 16777619;
    }
    wa_bitmap_release(store, view);
    Require(visible > 0, "APK icon has visible pixels");
    return hash;
}

void CheckDrawer(wa_sf_t* scene, const Layer& window, wa_rect_t search, std::span<const std::string_view> expected) {
    Require(window.sheet_outline, "All Apps sheet has a subtle outline");
    const Rounded* fill = nullptr;
    const Rounded* stroke = nullptr;
    for (const auto& shape : window.rounds) {
        if (!SameBounds(shape.bounds, search, 1.f)) continue;
        if (shape.style == WA_PAINT_FILL) fill = &shape;
        if (shape.style == WA_PAINT_STROKE) stroke = &shape;
    }
    Require(fill && stroke, "All Apps Search fill and stroke draw");
    for (const auto* shape : {fill, stroke}) {
        Require(std::abs(shape->rx - shape->ry) <= kGeometryTolerance &&
            std::abs(shape->rx - Height(shape->bounds) / 2) <= kGeometryTolerance &&
            SameBounds(shape->bounds, fill->bounds), "Search pill has concentric circular ends");
    }

    std::vector<Bitmap> icons;
    std::set<uint32_t> fingerprints;
    for (const auto& icon : window.bitmaps) {
        if (icon.clip.t < search.t || icon.bounds.t < search.b) continue;
        icons.push_back(icon);
        fingerprints.insert(IconFingerprint(scene, icon.id));
    }
    CheckAppLabels(window, icons, expected);
    Require(fingerprints.size() == expected.size(), "Every app draws a distinct APK icon");
}

struct Session {
    Session(int width, int height, const char* locale) : width(width), height(height) {
        Check(wa_system_launcher_create(width, height, 160, locale), "create");
        scene = wa_system_launcher_sf_ctx();
        Require(scene != nullptr, "Launcher scene");
    }
    ~Session() { wa_system_launcher_destroy(); }
    std::array<Layer, 2> ReadFrame() const { return Inspect(scene, width, height); }
    void Resize(int w, int h) {
        Check(wa_system_launcher_resize(w, h, 160), "resize");
        width = w;
        height = h;
        Tick();
    }
    void Tick(int count = 1) {
        for (int i = 0; i < count; i++) {
            time += 16'666'667;
            Check(wa_system_launcher_tick(time), "tick");
        }
    }
    void Push(int kind, int action, double delta, float x, float y, std::string_view text = {}) {
        Require(text.size() <= WA_INPUT_TEXT_CAPACITY, "Test text buffer");
        wa_input_event_t event{};
        event.kind = kind;
        event.action = action;
        event.event_time_ms = time / 1e6 + delta;
        event.x = x;
        event.y = y;
        event.text_len = text.size();
        if (!text.empty()) std::memcpy(event.text, text.data(), text.size());
        Check(wa_input_push(wa_sf_input(scene), &event), "push input");
    }
    void Tap(float x, float y) {
        Push(WA_INPUT_MOTION, WA_ACTION_DOWN, 1, x, y);
        Push(WA_INPUT_MOTION, WA_ACTION_UP, 3, x, y);
    }
    void TapSystemUi(const char* button, int code, int frames = 1) {
        const State before;
        const auto center = before.Button(button);
        Require(center.y > height - 48 && center.y < height, "Navigation button center within the bar");
        Tap(center.x, center.y);
        Tick(frames);
        const State after;
        Require(after.Integer("systemUiKeyEvents") == before.Integer("systemUiKeyEvents") + 2 &&
                after.Integer("systemUiLastKeyCode") == code &&
                after.Integer("systemUiLastKeyFlags") == 0x48, after.text);
    }
    void TapSystemUiWhilePaused(const char* button, int code) {
        const State before;
        const auto point = before.Button(button);
        const int64_t ms = time / 1'000'000;
        Require(wa_system_launcher_touch_navigation(ms + 1, WA_ACTION_DOWN, point.x, point.y) == 0 &&
                wa_system_launcher_touch_navigation(ms + 3, WA_ACTION_UP, point.x, point.y) == code,
                "SystemUI handles navigation over the foreground app");
        const State after;
        Require(after.Integer("systemUiKeyEvents") == before.Integer("systemUiKeyEvents") + 2 &&
                after.Integer("systemUiLastKeyCode") == code &&
                after.Integer("systemUiLastKeyFlags") == 0x48, after.text);
    }
    void Drag(float x, float y, float dx, float dy) {
        Push(WA_INPUT_MOTION, WA_ACTION_DOWN, 1, x, y);
        Push(WA_INPUT_MOTION, WA_ACTION_MOVE, 5, x + dx * .2f, y + dy * .2f);
        Push(WA_INPUT_MOTION, WA_ACTION_MOVE, 10, x + dx * .75f, y + dy * .75f);
        Push(WA_INPUT_MOTION, WA_ACTION_UP, 15, x + dx, y + dy);
    }
    void Swipe(int from, int to, const HomeGeometry& home) {
        const State before;
        Require(before.Integer("workspacePage") == from, before.text);
        const int start = before.Integer("workspaceScrollX");
        const float x = width * (to > from ? .82f : .18f);
        const float y = height * .38f;
        const float dx = width * (to > from ? -.64f : .64f);
        Push(WA_INPUT_MOTION, WA_ACTION_DOWN, 1, x, y);
        Tick();
        bool dots_moved = false;
        for (int step = 1; step <= 4; ++step) {
            Push(WA_INPUT_MOTION, WA_ACTION_MOVE, 1, x + dx * step / 4, y);
            Tick();
            const auto dots = PaginationDots(ReadFrame()[1], width, home.hotseat_top);
            dots_moved |= dots.size() == 2 && dots[0].alpha != home.dots[0].alpha &&
                dots[1].alpha != home.dots[1].alpha &&
                std::abs(Width(dots[0].bounds) - Width(home.dots[0].bounds)) > kGeometryTolerance &&
                std::abs(Width(dots[1].bounds) - Width(home.dots[1].bounds)) > kGeometryTolerance;
        }
        Push(WA_INPUT_MOTION, WA_ACTION_UP, 1, x + dx, y);
        std::array<int, 36> scrolls;
        for (auto& scroll : scrolls) {
            Tick();
            scroll = State().Integer("workspaceScrollX");
        }
        const State after;
        Require(after.Integer("workspacePage") == to, after.text);
        const int end = scrolls.back();
        Require(std::abs(start - end) > width * .65f, "Actual workspace page travel");
        Require(std::ranges::any_of(scrolls, [&](int x) {
            return x > std::min(start, end) && x < std::max(start, end);
        }), "Intermediate animation positions");
        Require(dots_moved, "Page indicator widths and opacity animate with the swipe");
    }
    int width, height;
    wa_sf_t* scene;
    int64_t time = 0;
};

void LongPressWorkspaceIcon(Session& session, const Layer& home, const HomeGeometry& geometry,
                            std::string_view label) {
    const auto title = home.Find(label);
    const auto icon = std::ranges::find_if(home.bitmaps, [&](const auto& bitmap) {
        const auto r = bitmap.bounds;
        return r.b < geometry.dots.front().bounds.t && std::abs(CenterX(r) - title.x) < 1 &&
               r.b < title.y && title.y - r.b < 36;
    });
    Require(icon != home.bitmaps.end(), "Workspace icon is visible for the long press");
    const float x = CenterX(icon->bounds), y = (icon->bounds.t + icon->bounds.b) / 2;
    const float dx = session.width * (x < session.width / 2.f ? .18f : -.18f);
    const float dy = session.height * (y < geometry.hotseat_top / 2 ? .25f : -.25f);
    const int launches = State().Integer("launchCount");

    session.Push(WA_INPUT_MOTION, WA_ACTION_DOWN, 1, x, y);
    session.Tick(36);
    const auto popup = session.ReadFrame()[1];
    const bool app_info = std::ranges::any_of(popup.labels, [](const auto& text) {
        return text.label == "App info";
    });
    const bool pickup = std::ranges::any_of(popup.bitmaps, [&](const auto& bitmap) {
        const auto r = bitmap.bounds;
        return std::abs(CenterX(r) - x) < 2 && std::abs((r.t + r.b) / 2 - y) < 2 &&
               std::ranges::none_of(home.bitmaps, [&](const auto& previous) { return previous.id == bitmap.id; });
    });
    Require(app_info && pickup, "Launcher3 rendered App info and the lifted icon");
    session.Push(WA_INPUT_MOTION, WA_ACTION_MOVE, 1, x + dx / 2, y + dy / 2);
    session.Tick();
    session.Push(WA_INPUT_MOTION, WA_ACTION_MOVE, 1, x + dx, y + dy);
    session.Tick(12);
    const auto dragging = session.ReadFrame()[1];
    const auto at_drag_location = [&](const auto& path) {
        return std::abs(path.position.x - (x + dx)) < Width(icon->bounds) / 3 &&
               std::abs(path.position.y - (y + dy)) < Height(icon->bounds) / 3;
    };
    // The preview switches from the pickup bitmap to the adaptive icon paths.
    Require(std::ranges::any_of(dragging.paths, at_drag_location) &&
            std::ranges::none_of(home.paths, at_drag_location), "Launcher3 rendered the moved icon preview");

    session.Push(WA_INPUT_MOTION, WA_ACTION_CANCEL, 1, x + dx, y + dy);
    session.Tick(24);
    const State after;
    Require(after.Integer("launchCount") == launches && after.Integer("workspacePage") == 0 &&
            !after.Boolean("drawerOpen"), after.text);
    std::puts("Launcher3 workspace icon long press opened App info and rendered a drag preview");
}

void Run(int width, int height, const char* locale, const char* composition, const char* query,
         const char* cube_label, const File& dex, const File& fallback, const File& resources,
         const File& launcher, const File& systemui, const std::vector<File>& apps) {
    using namespace std::string_view_literals;
    const bool english = locale == "en-US"sv;
    const std::array app_labels{
        english ? "Settings"sv : "设置"sv, english ? "Clock"sv : "时钟"sv, std::string_view(cube_label),
        english ? "Gallery"sv : "图库"sv, english ? "Music"sv : "音乐"sv, "WebView Browser Tester"sv,
    };
    const auto first_labels = std::span(app_labels).first<3>();
    const auto second_labels = std::span(app_labels).last<3>();
    Session session(width, height, locale);
    Check(wa_system_launcher_load_framework_dex(dex.bytes.data(), dex.bytes.size()), "load framework");
    Check(wa_system_launcher_load_framework_fallback_dex(fallback.bytes.data(), fallback.bytes.size()),
        "load framework compatibility");
    Check(wa_system_launcher_set_framework_res_apk(resources.bytes.data(), resources.bytes.size()), "mount framework");
    for (const auto& app : apps) app.Install();
    Require(wa_system_launcher_boot_apk(launcher.name.c_str(), launcher.bytes.data(), launcher.bytes.size()) ==
        WA_ERR_INVALID && !State().Boolean("systemUiMounted"), "Launcher requires SystemUI");
    Require(wa_system_launcher_load_systemui_apk(launcher.name.c_str(), launcher.bytes.data(), launcher.bytes.size()) ==
        WA_ERR_INVALID, "Reject a different APK package for SystemUI");
    Check(wa_system_launcher_load_systemui_apk(systemui.name.c_str(), systemui.bytes.data(), systemui.bytes.size()),
        "mount SystemUI");
    Check(wa_system_launcher_boot_apk(launcher.name.c_str(), launcher.bytes.data(), launcher.bytes.size()), "boot");

    const State boot;
    boot.Print(locale, "boot");
    Require(boot.Boolean("booted") && boot.Is("package", "com.android.launcher3") &&
            boot.Is("activity", "com.android.launcher3.Launcher"), boot.text);
    Require(boot.Boolean("launcherUpstream") && boot.Integer("launcherDexCount") == 4 &&
            boot.Integer("launcherClassCount") >= 37050, boot.text);
    Require(boot.Boolean("systemUiMounted") &&
        boot.Is("systemUiClass", "com.android.systemui.statusbar.phone.PhoneStatusBarView"), boot.text);
    Require(boot.Integer("installedApps") == 6 && boot.Integer("pageCount") == 2 &&
            boot.Integer("launcherDrawCommands") > 0, boot.text);
    for (const char* cls : {"LauncherApplication", "Launcher", "LauncherModel", "Workspace",
            "CellLayout", "Hotseat", "BubbleTextView", "DeviceProfile", "InvariantDeviceProfile",
            "ActivityAllAppsContainerView", "DaggerLauncherAppComponent", "PhoneStatusBarView",
            "NavigationBarView", "KeyButtonView", "Clock", "BatteryMeterView"}) {
        Require(boot.Integer(cls) > 0, cls);
    }
    const auto layers = session.ReadFrame();
    Require(layers[0].Has(WA_CANVAS_DRAW_LINEAR_GRADIENT) && layers[0].Has(WA_CANVAS_DRAW_PATH), "Wallpaper");
    Require(layers[1].Has(WA_CANVAS_DRAW_ROUND_RECT) && layers[1].Has(WA_CANVAS_DRAW_TEXT) &&
            layers[1].Has(WA_CANVAS_DRAW_BITMAP), "Launcher view tree");
    CheckSystemUiFrame(layers[1], height);
    const int navigation_inset = boot.NavigationInset();
    CheckHome(layers[1], width, height, 0, navigation_inset, first_labels);
    PublishBrowserFontMetrics(session.scene);

    session.Tick(120);
    State().Print(locale, "home-120");
    const auto first_page = session.ReadFrame()[1];
    const auto first_home = CheckHome(first_page, width, height, 0, navigation_inset, first_labels);
    session.Swipe(0, 1, first_home);
    const auto second_page = session.ReadFrame()[1];
    const auto second_home = CheckHome(second_page, width, height, 1, navigation_inset, second_labels);
    session.Swipe(1, 0, second_home);
    session.Swipe(0, 1, first_home);
    session.Drag(width * .54f, height * .78f, 0, -height * .42f);
    session.Tick(36);
    const State drawer;
    Require(drawer.Boolean("drawerOpen") && drawer.Number("drawerProgress") > 0.9 &&
            drawer.Integer("searchResults") == static_cast<int>(apps.size()), drawer.text);

    const auto search = drawer.SearchBounds();
    CheckDrawer(session.scene, session.ReadFrame()[1], search, app_labels);
    session.Tap(CenterX(search), (search.t + search.b) / 2);
    session.Push(WA_INPUT_IME, WA_IME_COMPOSING, 8, 0, 0, composition);
    session.Tick(12);
    const State composing;
    Require(composing.Is("searchQuery", composition), composing.text);
    session.Push(WA_INPUT_IME, WA_IME_COMMIT, 8, 0, 0, query);
    session.Tick(24);
    const State found;
    Require(found.Is("searchQuery", query) && found.Integer("searchResults") == 1, found.text);
    const Position cube = session.ReadFrame()[1].Find(cube_label);
    session.Tap(cube.x, cube.y - 5);
    session.Tick(30);
    char launch[512];
    Require(wa_system_launcher_consume_launch(launch, 2) == WA_ERR_OVERFLOW, "Launch survives small buffer");
    Require(wa_system_launcher_consume_launch(launch, sizeof(launch)) > 0, "Pending launch");
    Require(std::string_view(launch).find("\"pending\":true,\"package\":\"org.wasmandroid.cube\","
            "\"activity\":\"org.wasmandroid.cube.CubeActivity\",\"apkName\":\"wa_cube_app.apk\",\"label\":\""
            + std::string(cube_label) + "\"") != std::string_view::npos, launch);
    Require(wa_system_launcher_consume_launch(launch, sizeof(launch)) == 0, "Launch is consumed once");

    Check(wa_system_launcher_pause(), "pause");
    const int paused_at = State().Integer("frames");
    session.Tick();
    Require(State().Integer("frames") == paused_at, "Paused frames are not presented");
    session.TapSystemUiWhilePaused("back", 4);
    session.TapSystemUiWhilePaused("home", 3);
    Check(wa_system_launcher_resume(), "resume");
    Check(wa_system_launcher_send_home(), "send home");
    session.Tick(36);
    const State before_gc;
    Check(wa_system_art_gc_collect(), "explicit GC");
    const State final;
    final.Print(locale, "interactive");
    Require(!final.Boolean("drawerOpen") &&
            final.Number("drawerProgress") == 0 && final.Is("searchQuery", "") &&
            final.Integer("workspacePage") == 0 && final.Integer("launchCount") == 1, final.text);
    Require(final.Integer("gcCollections") > before_gc.Integer("gcCollections") &&
            final.Integer("gcReclaimed") > 0 && final.Integer("launcherDrawCommands") > 0 &&
            final.Integer("inputEventsProcessed") >= 18, final.text);
    const int frames = final.Integer("frames");
    {
        const Frame held(session.scene);
        session.Tick();
        session.time += 16'666'667;
        Require(wa_system_launcher_tick(session.time) == WA_ERR_BUSY && State().Integer("frames") == frames + 1,
                "Held frame blocks reusing its packet");
    }
    Check(wa_system_launcher_tick(session.time), "retry frame");
    Require(State().Integer("frames") == frames + 2, "Retried frame publishes once");
    session.Resize(width == 960 ? 448 : 960, height == 660 ? 896 : 660);
    CheckSystemUiFrame(session.ReadFrame()[1], session.height);
    Check(wa_system_launcher_open_all_apps(), "open All Apps");
    session.Tick(36);
    Require(State().Boolean("drawerOpen"), "All Apps opened before Back key");
    session.TapSystemUi("back", 4, 36);
    Require(!State().Boolean("drawerOpen"), "SystemUI Back closed All Apps");
    session.TapSystemUi("recents", 187);
    Require(wa_system_launcher_consume_recents() == 1 && wa_system_launcher_consume_recents() == 0,
        "SystemUI Recents request is delivered once");
    Check(wa_system_launcher_set_gesture_navigation(1), "enable gestural SystemUI");
    session.Tick();
    Require(State().Boolean("systemUiGestural"), "Gestural mode active");
    CheckSystemUiFrame(session.ReadFrame()[1], session.height, true);
    Check(wa_system_launcher_set_gesture_navigation(0), "restore three-button SystemUI");
    session.Tick();
    CheckSystemUiFrame(session.ReadFrame()[1], session.height);
    session.TapSystemUi("home", 3);
    if (english) {
        session.Tick(36);
        const auto home = session.ReadFrame()[1];
        const auto geometry = CheckHome(home, session.width, session.height, 0,
                                        State().NavigationInset(), first_labels);
        LongPressWorkspaceIcon(session, home, geometry, first_labels.front());
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Require(argc == 12, "Expected two framework DEX files, framework resources, Launcher3, SystemUI and six apps");
        const File dex(argv[1]), fallback(argv[2]), resources(argv[3]), launcher(argv[4]), systemui(argv[5]);
        std::vector<File> apps;
        constexpr std::array names{"wa_cube_app.apk", "Settings-upstream.apk", "DeskClock-upstream.apk",
            "Browser2-upstream.apk", "Gallery2-upstream.apk", "Music-upstream.apk"};
        for (int i = 6; i < argc; ++i) {
            apps.emplace_back(argv[i]);
            Require(apps.back().name == names[i - 6], "Unexpected Launcher3 APK");
        }
        Run(960, 660, "en-US", "C", "Cube", "WasmAndroid 3D Cube", dex, fallback, resources, launcher, systemui, apps);
        Run(448, 896, "zh-CN", "立", "立方体", "WasmAndroid 3D 立方体", dex, fallback, resources, launcher, systemui, apps);
        Require(wa_system_launcher_sf_ctx() == nullptr, "Destroyed session");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
