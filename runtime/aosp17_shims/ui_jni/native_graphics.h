#pragma once

#include "native_methods.h"

#include "jni_bridge.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <variant>

namespace wa::ui_jni::detail {

template <class T>
class GraphicsPeers : public std::enable_shared_from_this<GraphicsPeers<T>> {
  public:
    jlong Insert(T data = {}) {
        if (next_ == std::numeric_limits<jlong>::max()) {
            throw std::bad_alloc();
        }
        const jlong handle = next_++;
        peers_.try_emplace(handle, Entry{std::move(data)});
        return handle;
    }

    T* Get(JNIEnv* env, jlong handle) {
        if (auto* data = Find(handle)) return data;
        Throw(env, "java/lang/IllegalStateException", "Invalid native graphics handle");
        return nullptr;
    }

    T* Find(jlong handle) {
        const auto it = peers_.find(handle);
        return it == peers_.end() ? nullptr : &it->second.data;
    }

    void Retain(JNIEnv* env, jobject peer, jlong handle) {
        const auto it = peers_.find(handle);
        if (it == peers_.end() || it->second.retained) {
            Throw(env, "java/lang/IllegalStateException", "Invalid native graphics allocation");
            return;
        }
        auto release = [store = this->weak_from_this(), handle](void*) {
            if (auto live = store.lock()) live->Erase(handle);
        };
        interp::JniBridge::From(env).RetainNativePeer(
            peer, std::shared_ptr<void>(&it->second.data, std::move(release)));
        it->second.retained = true;
    }

    void Erase(jlong handle) { peers_.erase(handle); }

  private:
    struct Entry {
        T data;
        bool retained = false;
    };
    jlong next_ = 1;
    std::unordered_map<jlong, Entry> peers_;
};

using Matrix9 = std::array<jfloat, 9>;
inline constexpr Matrix9 kIdentity{1, 0, 0, 0, 1, 0, 0, 0, 1};

inline Matrix9 Multiply(const Matrix9& a, const Matrix9& b) {
    Matrix9 result{};
    for (size_t row = 0; row < 9; row += 3)
        for (size_t col = 0; col < 3; ++col)
            result[row + col] = a[row] * b[col] + a[row + 1] * b[3 + col] + a[row + 2] * b[6 + col];
    return result;
}
inline Matrix9 Translation(float x, float y) { return {1, 0, x, 0, 1, y, 0, 0, 1}; }
inline Matrix9 Scaling(float x, float y) { return {x, 0, 0, 0, y, 0, 0, 0, 1}; }
inline Matrix9 Rotation(float sine, float cosine) { return {cosine, -sine, 0, sine, cosine, 0, 0, 0, 1}; }
inline Matrix9 Skew(float x, float y) { return {1, x, 0, y, 1, 0, 0, 0, 1}; }
inline Matrix9 AtPivot(const Matrix9& m, float x, float y) {
    return Multiply(Translation(x, y), Multiply(m, Translation(-x, -y)));
}
inline std::array<float, 2> MapPoint(const Matrix9& m, float x, float y) {
    const float w = m[6] * x + m[7] * y + m[8];
    const float inverse = w == 0.f ? 0.f : 1.f / w;
    return {(m[0] * x + m[1] * y + m[2]) * inverse, (m[3] * x + m[4] * y + m[5]) * inverse};
}

std::optional<Matrix9> Inverse(const Matrix9& m);
Matrix9* MatrixValues(JNIEnv* env, jlong handle);
wa_rect_t MapRect(const Matrix9& m, wa_rect_t rect);
void SetRect(JNIEnv* env, jobject rect, wa_rect_t values, bool floating = false);
inline bool HasArea(wa_rect_t r) { return r.l < r.r && r.t < r.b; }

struct FontVariation {
    uint32_t weight = 0;
    std::optional<bool> italic;
};

struct PaintData {
    uint32_t color = 0xff000000u;
    jint flags = 0, style = 0, align = 0, cap = 0, join = 0;
    jint hinting = 1, elegant = 0, start_hyphen = 0, end_hyphen = 0, locale = 0, blend = 3;
    float stroke_width = 0.f, miter = 4.f;
    float text_size = 16.f, text_scale = 1.f, text_skew = 0.f;
    float letter_spacing = 0.f, word_spacing = 0.f;
    float shadow_radius = 0.f, shadow_dx = 0.f, shadow_dy = 0.f;
    uint32_t shadow_color = 0;
    jlong shader_handle = 0, color_filter = 0, mask_filter = 0, path_effect = 0, typeface = 0;
    std::shared_ptr<const ShaderData> shader;
    std::string features;
    FontVariation variation;

    uint32_t Face() const { return typeface ? static_cast<uint32_t>(typeface >> 12) & 3u : WA_FONT_SYSTEM; }
    uint32_t Weight() const {
        const uint32_t weight = variation.weight ? variation.weight
            : typeface ? static_cast<uint32_t>(typeface >> 1) & 1023u : 400u;
        return flags & 0x20 ? std::max(700u, weight) : weight;
    }
    bool Italic() const { return variation.italic.value_or(typeface & 1); }
};

struct PathGeometry {
    std::vector<wa_path_verb_t> verbs;
    std::array<float, 2> current{}, contour{};
    jint fill = 0;
    uint32_t generation = 1;
    bool needs_move = true;
    std::optional<wa_round_rect_t> clip;

    void Add(wa_path_command command, std::array<float, 2> a = {}, std::array<float, 2> b = {},
             std::array<float, 2> c = {});
    void Move(float x, float y);
    void Line(float x, float y);
    void Quad(float x, float y, float x2, float y2);
    void Cubic(float x, float y, float x2, float y2, float x3, float y3);
    void Close();
    void Rect(wa_rect_t rect, jint direction = 0);
    void Arc(wa_rect_t rect, float start, float sweep, bool move);
    void Oval(wa_rect_t rect, jint direction = 0);
    void RoundRect(wa_rect_t rect, std::array<float, 8> radii, jint direction = 0);
    void Transform(const Matrix9& matrix);
    wa_rect_t Bounds() const;
    bool Empty() const;
};

inline std::vector<interp::Value> RasterPathValues(const PathGeometry& path) {
    std::vector<interp::Value> values;
    values.reserve(path.verbs.size() * 7);
    for (const auto& verb : path.verbs) {
        values.push_back(interp::Value::Float(verb.command));
        for (float coordinate : verb.points) values.push_back(interp::Value::Float(coordinate));
    }
    return values;
}

std::optional<PathGeometry> ParseSvgPath(JNIEnv* env, jstring text);
std::optional<PathGeometry> PathFromSvgData(JNIEnv* env, jlong handle);

struct BitmapData {
    jint width = 0, height = 0, config = 5;
    bool has_alpha = true, premultiplied = true, immutable = false, mipmap = false, recycled = false;
    jint generation = 1;
    jlong source = -1;
    jlong color_space = 0;
    std::vector<jint> pixels;

    void Changed() { generation = generation == INT32_MAX ? 1 : generation + 1; }
    jint PutColor(uint32_t color) const;
};

struct NinePatchData {
    std::vector<int32_t> x_divisions;
    std::vector<int32_t> y_divisions;
};

struct PatchSpan {
    float from, to, dst_from, dst_to;
};

inline std::vector<PatchSpan> PartitionPatch(std::span<const int32_t> divisions, float size,
                                      float start, float end, float density) {
    float stretch = 0;
    for (size_t i = 0; i < divisions.size(); i += 2)
        stretch += std::min<float>(size, divisions[i + 1]) - std::min<float>(size, divisions[i]);
    const float fixed = size - stretch, extent = end - start;
    const float fixed_scale = fixed > 0 ? std::min(density, extent / fixed) : density;
    const float stretch_scale = stretch > 0 ? std::max(0.f, extent - fixed * fixed_scale) / stretch : 0;
    std::vector<PatchSpan> spans;
    spans.reserve(divisions.size() + 1);
    float from = 0, dst = start;
    for (size_t i = 0; i <= divisions.size(); ++i) {
        const float to = i == divisions.size() ? size : std::min<float>(size, divisions[i]);
        const float next = i == divisions.size() ? end :
            std::min(end, dst + (to - from) * (i % 2 ? stretch_scale : fixed_scale));
        if (to > from && next > dst) spans.push_back({from, to, dst, next});
        from = to;
        dst = next;
    }
    return spans;
}

struct GradientData {
    jint type, tile;
    std::array<float, 4> geometry;
    std::vector<jfloat> positions;
    std::vector<jint> colors;
};

struct BitmapShaderData {
    std::shared_ptr<const BitmapData> bitmap;
    jint tile_x, tile_y;
    bool filter;
};

struct ComposeShaderData {
    std::shared_ptr<const ShaderData> destination, source;
    jint mode;
};

struct ShaderData {
    std::variant<GradientData, BitmapShaderData, ComposeShaderData> content;
    std::optional<Matrix9> inverse = kIdentity;
};

struct ColorSpaceData {
    std::array<jfloat, 7> transfer;
    Matrix9 to_xyz;
};

struct FontFamilyData {
    std::string languages;
    jint variant = 0;
    std::vector<std::pair<jint, jfloat>> axes;
    std::vector<jlong> faces;
};

struct GlyphRunData {
    struct Glyph {
        jint codepoint;
        jfloat x;
    };
    std::vector<Glyph> glyphs;
    wa_text_metrics_t metrics{};
    jlong face = 0;
    FontVariation variation;
    bool fake_bold = false;
};

enum HostFinalizer : jlong {
    kFinalizeFontFamily = 1,
    kFinalizeGlyphRun = 2,
};

struct CanvasData;

template <class T, std::shared_ptr<GraphicsPeers<T>> State::*Storage>
GraphicsPeers<T>& Peers(State& state) {
    auto& store = state.*Storage;
    if (!store) store = std::make_shared<GraphicsPeers<T>>();
    return *store;
}

template <class T, std::shared_ptr<GraphicsPeers<T>> State::*Storage>
T* Peer(JNIEnv* env, jlong handle) {
    auto* state = GetState(env);
    if (!state) return nullptr;
    if (const auto& store = state->*Storage) return store->Get(env, handle);
    Throw(env, "java/lang/IllegalStateException", "Native graphics store is empty");
    return nullptr;
}

template <class T, std::shared_ptr<GraphicsPeers<T>> State::*Storage>
void RetainPeer(JNIEnv* env, jclass, jobject peer, jlong handle) {
    CallSafely(env, [&] {
        if (auto* state = GetState(env)) Peers<T, Storage>(*state).Retain(env, peer, handle);
    });
}

inline PaintData* Paint(JNIEnv* env, jlong handle) {
    return Peer<PaintData, &State::paints>(env, handle);
}
inline PathGeometry* Path(JNIEnv* env, jlong handle) {
    return Peer<PathGeometry, &State::paths>(env, handle);
}
inline BitmapData* Bitmap(JNIEnv* env, jlong handle) {
    const auto* bitmap = Peer<std::shared_ptr<BitmapData>, &State::bitmaps>(env, handle);
    return bitmap ? bitmap->get() : nullptr;
}

std::u16string Chars(JNIEnv* env, jstring string, jint start, jint end);
std::u16string Chars(JNIEnv* env, jcharArray text, jint offset, jint count);
std::string Utf8(std::u16string_view chars);
uint32_t UnpackColor(jlong color);
wa_text_metrics_t Measure(State& state, const PaintData& paint, std::u16string_view text);
void RegisterPaint(interp::Vm& vm);
void RegisterTypeface(interp::Vm& vm);

} // namespace wa::ui_jni::detail
