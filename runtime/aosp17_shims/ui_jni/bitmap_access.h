#pragma once

#include <android/bitmap.h>

#include <memory>
#include <span>

namespace wa::ui_jni {

namespace detail {
struct BitmapData;
}

struct NativeBitmap {
    std::shared_ptr<detail::BitmapData> data;
    AndroidBitmapInfo info;
    bool writable;
};

NativeBitmap AccessNativeBitmap(JNIEnv* env, jobject bitmap);
void CopyNativeBitmapPixels(const NativeBitmap& bitmap, std::span<uint8_t> buffer, bool write);

} // namespace wa::ui_jni
