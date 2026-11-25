#include "hybrid_jni_bridge.h"

#include "bionic_internal.h"
#include "bitmap_access.h"

#include <cerrno>

namespace wa::system {
namespace {

class LocalFrame {
  public:
    explicit LocalFrame(JNIEnv* env) : env_(env) {
        if (env_->PushLocalFrame(24) != JNI_OK) throw interp::ManagedException{};
    }
    ~LocalFrame() { env_->PopLocalFrame(nullptr); }

  private:
    JNIEnv* env_;
};

void SetWindowError(wa_host_call_ctx_t& ctx, int error) {
    ctx.ret0 = static_cast<uint32_t>(-dbt::GuestErrno(error));
}

constexpr int kRgba8888 = ANDROID_BITMAP_FORMAT_RGBA_8888;
constexpr const char* kTexture = "Landroid/graphics/SurfaceTexture;";
constexpr const char* kBitmap = "Landroid/graphics/Bitmap;";

} // namespace

template<HybridJniBridge::WindowOp operation>
int32_t HybridJniBridge::WindowCall(wa_host_call_ctx_t* ctx, void* user) {
    auto& bridge = *static_cast<HybridJniBridge*>(user);
    auto* env = bridge.Env();
    if (!bridge.jni_.IsVmThread() || env->ExceptionCheck()) return WA_ERR_INVALID;
    try {
        LocalFrame frame(env);
        bridge.DispatchWindow(operation, *ctx);
        if (!env->ExceptionCheck()) return WA_OK;
    } catch (const interp::ManagedException&) {
    } catch (const dbt::GuestFault& fault) {
        if (fault.status != WA_ERR_NOMEM) return fault.status;
        if (operation != WindowOp::FromSurface) SetWindowError(*ctx, ENOMEM);
        return WA_OK;
    } catch (const std::bad_alloc&) {
        if (operation != WindowOp::FromSurface) SetWindowError(*ctx, ENOMEM);
        return WA_OK;
    }
    env->ExceptionClear();
    if (operation != WindowOp::FromSurface) SetWindowError(*ctx, EINVAL);
    return WA_OK;
}

void HybridJniBridge::RegisterNativeWindow() {
    const dbt::Builtin functions[] = {
        {"ANativeWindow_fromSurface", WindowCall<WindowOp::FromSurface>},
        {"ANativeWindow_acquire", WindowCall<WindowOp::Acquire>},
        {"ANativeWindow_release", WindowCall<WindowOp::Release>},
        {"ANativeWindow_getWidth", WindowCall<WindowOp::Width>},
        {"ANativeWindow_getHeight", WindowCall<WindowOp::Height>},
        {"ANativeWindow_getFormat", WindowCall<WindowOp::Format>},
        {"ANativeWindow_setBuffersGeometry", WindowCall<WindowOp::Geometry>},
        {"ANativeWindow_lock", WindowCall<WindowOp::Lock>},
        {"ANativeWindow_unlockAndPost", WindowCall<WindowOp::Post>},
    };
    for (const auto& function : functions)
        dbt::CheckGuest(wa_dbt_register_symbol(&dbt_, "libandroid.so", function.name, function.function, this));
}

void HybridJniBridge::ClearWindowLock(GuestWindow& window) {
    if (window.canvas) jni_.DeleteGlobal(std::exchange(window.canvas, nullptr));
    if (window.bitmap) jni_.DeleteGlobal(std::exchange(window.bitmap, nullptr));
    window.pixels.reset();
}

void HybridJniBridge::DispatchWindow(WindowOp operation, wa_host_call_ctx_t& ctx) {
    auto* env = Env();
    const auto read_texture = [&](jobject surface, jclass type) {
        return env->GetObjectField(surface, env->GetFieldID(type, "mTexture", kTexture));
    };
    if (operation == WindowOp::FromSurface) {
        if (ctx.args[0] != jni_env_ || !ctx.args[1]) return;
        const auto surface = Ref(ctx.args[1]);
        const auto type = env->FindClass("android/view/Surface");
        if (!type || !env->IsInstanceOf(surface, type) ||
            !env->CallBooleanMethod(surface, env->GetMethodID(type, "isValid", "()Z"))) return;
        const jvalue texture{.l = read_texture(surface, type)};
        // Keep an independent Java Surface so release() on the caller's reference
        // does not invalidate the native handle to the same producer.
        const auto constructor = env->GetMethodID(type, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
        const auto peer = env->NewObjectA(type, constructor, &texture);
        if (!peer) return;
        const auto id = NextHandle();
        auto* global = env->NewGlobalRef(peer);
        if (!global) return;
        try {
            windows_.emplace(id, GuestWindow{.surface = global});
        } catch (...) {
            jni_.DeleteGlobal(global);
            throw;
        }
        ctx.ret0 = id;
        return;
    }

    const auto found = windows_.find(ctx.args[0]);
    if (found == windows_.end()) {
        if (operation != WindowOp::Acquire && operation != WindowOp::Release) SetWindowError(ctx, EINVAL);
        return;
    }
    auto& window = found->second;
    const auto type = env->GetObjectClass(window.surface);
    switch (operation) {
    case WindowOp::Acquire: ++window.references; return;
    case WindowOp::Release:
        if (--window.references == 0) {
            if (window.canvas) {
                const auto texture = read_texture(window.surface, type);
                const auto texture_class = env->GetObjectClass(texture);
                env->SetObjectField(texture, env->GetFieldID(texture_class, "mWorkingFrame", kBitmap), nullptr);
            }
            ClearWindowLock(window);
            jni_.DeleteGlobal(window.surface);
            windows_.erase(found);
        }
        return;
    case WindowOp::Width: case WindowOp::Height: {
        const auto point = env->CallObjectMethod(window.surface,
            env->GetMethodID(type, "getDefaultSize", "()Landroid/graphics/Point;"));
        if (point) {
            const auto point_class = env->GetObjectClass(point);
            const auto component = env->GetFieldID(point_class, operation == WindowOp::Width ? "x" : "y", "I");
            ctx.ret0 = env->GetIntField(point, component);
        }
        return;
    }
    case WindowOp::Format: ctx.ret0 = kRgba8888; return;
    case WindowOp::Geometry: {
        const auto width = static_cast<int32_t>(ctx.args[1]), height = static_cast<int32_t>(ctx.args[2]);
        if (width < 0 || height < 0 || (!width != !height) || (ctx.args[3] && ctx.args[3] != kRgba8888))
            return SetWindowError(ctx, EINVAL);
        if (width && height) {
            const auto texture = read_texture(window.surface, type);
            const auto size = env->GetMethodID(env->GetObjectClass(texture), "setDefaultBufferSize", "(II)V");
            env->CallVoidMethod(texture, size, width, height);
        }
        return;
    }
    case WindowOp::Lock: {
        if (!ctx.args[1] || window.canvas) return SetWindowError(ctx, EINVAL);
        const auto lock = env->GetMethodID(type, "lockCanvas", "(Landroid/graphics/Rect;)Landroid/graphics/Canvas;");
        const auto locked_canvas = env->GetFieldID(type, "mLockedCanvas", "Landroid/graphics/Canvas;");
        const auto texture = read_texture(window.surface, type);
        const auto working_frame = env->GetFieldID(env->GetObjectClass(texture), "mWorkingFrame", kBitmap);
        std::array<jint, 4> dirty{};
        jobject rect = nullptr;
        if (ctx.args[2]) {
            memory_.Read(ctx.args[2], dirty.data(), sizeof(dirty));
            const auto rect_class = env->FindClass("android/graphics/Rect");
            const auto constructor = env->GetMethodID(rect_class, "<init>", "(IIII)V");
            rect = env->NewObject(rect_class, constructor, dirty[0], dirty[1], dirty[2], dirty[3]);
            if (!rect) return SetWindowError(ctx, EINVAL);
        }
        const auto canvas = env->CallObjectMethod(window.surface, lock, rect);
        if (!canvas) return SetWindowError(ctx, EINVAL);
        const auto cancel = [&] {
            env->SetObjectField(texture, working_frame, nullptr);
            env->SetObjectField(window.surface, locked_canvas, nullptr);
            ClearWindowLock(window);
        };
        try {
            const auto image = env->GetObjectField(texture, working_frame);
            const auto bitmap = ui_jni::AccessNativeBitmap(env, image);
            if (!bitmap.data || !bitmap.writable || bitmap.info.format != kRgba8888) {
                cancel();
                return SetWindowError(ctx, EINVAL);
            }
            std::vector<uint8_t> bytes(size_t(bitmap.info.stride) * bitmap.info.height);
            ui_jni::CopyNativeBitmapPixels(bitmap, bytes, false);
            auto pixels = AllocateMemory(bytes.size());
            if (!pixels->Write(0, bytes)) throw dbt::GuestFault{WA_ERR_FAULT};
            window.canvas = env->NewGlobalRef(canvas);
            window.bitmap = env->NewGlobalRef(image);
            window.pixels = std::move(pixels);
            if (!window.canvas || !window.bitmap) throw std::bad_alloc();
            const auto stride = bitmap.info.stride / sizeof(uint32_t);
            const uint32_t header[]{bitmap.info.width, bitmap.info.height, static_cast<uint32_t>(stride), kRgba8888};
            const uint32_t reserved[6]{};
            memory_.Write(ctx.args[1], header, sizeof(header));
            memory_.WriteWord(ctx.args[1] + sizeof(header), window.pixels->GuestAddress());
            memory_.Write(ctx.args[1] + sizeof(header) + memory_.width(), reserved, sizeof(reserved));
            if (rect) {
                const auto rect_class = env->GetObjectClass(rect);
                constexpr const char* fields[]{"left", "top", "right", "bottom"};
                for (size_t i = 0; i < dirty.size(); ++i)
                    dirty[i] = env->GetIntField(rect, env->GetFieldID(rect_class, fields[i], "I"));
                memory_.Write(ctx.args[2], dirty.data(), sizeof(dirty));
            }
        } catch (...) {
            cancel();
            throw;
        }
        return;
    }
    case WindowOp::Post: {
        if (!window.canvas) return SetWindowError(ctx, EINVAL);
        const auto bitmap = ui_jni::AccessNativeBitmap(env, window.bitmap);
        if (!bitmap.data || !bitmap.writable) return SetWindowError(ctx, EINVAL);
        std::vector<uint8_t> bytes(size_t(bitmap.info.stride) * bitmap.info.height);
        if (!window.pixels->Read(0, bytes)) return SetWindowError(ctx, EFAULT);
        ui_jni::CopyNativeBitmapPixels(bitmap, bytes, true);
        const auto post = env->GetMethodID(type, "unlockCanvasAndPost", "(Landroid/graphics/Canvas;)V");
        env->CallVoidMethod(window.surface, post, window.canvas);
        ClearWindowLock(window);
        return;
    }
    case WindowOp::FromSurface: return;
    }
}

} // namespace wa::system
