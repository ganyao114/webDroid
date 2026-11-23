#pragma once

#include "native_graphics.h"

namespace wa::ui_jni::detail {

struct RegionRect {
    jint left = 0, top = 0, right = 0, bottom = 0;

    bool operator==(const RegionRect&) const = default;
    bool HasArea() const { return left < right && top < bottom; }
};

inline RegionRect ReadRegionRect(JNIEnv* env, jobject object) {
    const auto type = env->GetObjectClass(object);
    if (!type) return {};
    const auto field = [&](const char* name) {
        const auto id = env->GetFieldID(type, name, "I");
        return id ? env->GetIntField(object, id) : 0;
    };
    return {field("left"), field("top"), field("right"), field("bottom")};
}

struct RegionData {
    // Non-overlapping rectangles in horizontal bands, ordered by top then left.
    std::vector<RegionRect> rects;

    RegionRect Bounds() const {
        if (rects.empty()) return {};
        auto bounds = rects.front();
        for (const auto& rect : rects) {
            bounds.left = std::min(bounds.left, rect.left);
            bounds.right = std::max(bounds.right, rect.right);
        }
        bounds.bottom = rects.back().bottom;
        return bounds;
    }
};

inline jlong StoreRegion(JNIEnv* env, RegionData data = {}) {
    auto* state = GetState(env);
    return state ? Peers<RegionData, &State::regions>(*state).Insert(std::move(data)) : 0;
}

using RegionSpan = std::pair<jint, jint>;

inline void AddRegionBand(RegionData& region, jint top, jint bottom, std::span<const RegionSpan> spans,
                          size_t& previous) {
    auto& rects = region.rects;
    if (spans.empty()) return;
    const auto matches = [&](size_t index) {
        const auto& rect = rects[previous + index];
        return rect.bottom == top && rect.left == spans[index].first && rect.right == spans[index].second;
    };
    if (rects.size() - previous == spans.size()) {
        size_t index = 0;
        while (index < spans.size() && matches(index)) ++index;
        if (index == spans.size()) {
            for (size_t at = previous; at < rects.size(); ++at) rects[at].bottom = bottom;
            return;
        }
    }
    previous = rects.size();
    for (const auto& [left, right] : spans) rects.push_back({left, top, right, bottom});
}

} // namespace wa::ui_jni::detail
