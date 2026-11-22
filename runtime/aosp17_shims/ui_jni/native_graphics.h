#pragma once

#include "native_methods.h"

#include "jni_bridge.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>

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
        const auto it = peers_.find(handle);
        if (it != peers_.end()) return &it->second.data;
        Throw(env, "java/lang/IllegalStateException", "Invalid native graphics handle");
        return nullptr;
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
    jlong shader = 0, color_filter = 0, mask_filter = 0, path_effect = 0, typeface = 0;
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

std::optional<PathGeometry> ParseSvgPath(JNIEnv* env, jstring text);
std::optional<PathGeometry> PathFromSvgData(JNIEnv* env, jlong handle);

struct BitmapData {
    jint width = 0, height = 0, config = 5;
    bool has_alpha = true, premultiplied = true, immutable = false, mipmap = false, recycled = false;
    jint generation = 1;
    jlong source = -1;
    std::vector<jint> pixels;

    void Changed() { generation = generation == INT32_MAX ? 1 : generation + 1; }
    jint PutColor(uint32_t color) const;
};

struct NinePatchData {
    std::vector<int32_t> x_divisions;
    std::vector<int32_t> y_divisions;
};

struct GradientData {
    jint type, tile;
    std::array<float, 4> geometry;
    std::vector<jfloat> positions;
    std::vector<jint> colors;
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
    return Peer<BitmapData, &State::bitmaps>(env, handle);
}

std::u16string Chars(JNIEnv* env, jstring string, jint start, jint end);
std::u16string Chars(JNIEnv* env, jcharArray text, jint offset, jint count);
std::string Utf8(std::u16string_view chars);
uint32_t UnpackColor(jlong color);
wa_text_metrics_t Measure(State& state, const PaintData& paint, std::u16string_view text);
void RegisterPaint(interp::Vm& vm);
void RegisterTypeface(interp::Vm& vm);

} // namespace wa::ui_jni::detail
