#include "art_interp.h"
#include "base/mem_map.h"
#include "dex/art_dex_file_loader.h"
#include "dex/class_accessor-inl.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_verifier.h"
#include "dex/dex_instruction-inl.h"
#include "native_methods.h"

#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace android {
int register_android_content_res_ApkAssets(JNIEnv*);
int register_android_content_XmlBlock(JNIEnv*);
int register_android_content_StringBlock(JNIEnv*);
}

namespace {

using wa::interp::ScopedHandleScope;
using wa::interp::Value;
using wa::interp::Vm;
using wa::ui_jni::detail::Method;
using wa::ui_jni::detail::Register;

void Require(bool condition, std::string_view explanation) {
    if (!condition) {
        throw std::runtime_error(std::string(explanation));
    }
}

struct Counts {
    size_t methods = 0;
    size_t instructions = 0;
};

using Minimum = std::pair<std::string_view, size_t>;
constexpr Minimum kFrameworkClasses[] = {
    {"Landroid/content/ComponentName;", 20},
    {"Landroid/content/IntentFilter;", 80},
    {"Landroid/content/res/ApkAssets;", 35},
    {"Landroid/content/res/XmlBlock$Parser;", 45},
    {"Landroid/graphics/BaseCanvas;", 70},
    {"Landroid/graphics/Canvas;", 110},
    {"Landroid/graphics/Color;", 40},
    {"Landroid/graphics/Matrix;", 60},
    {"Landroid/graphics/Paint;", 175},
    {"Landroid/graphics/Path;", 50},
    {"Landroid/graphics/Rect;", 40},
    {"Landroid/graphics/RectF;", 35},
    {"Landroid/os/BaseBundle;", 70},
    {"Landroid/os/Bundle;", 70},
    {"Landroid/os/Handler;", 55},
    {"Landroid/os/Looper;", 25},
    {"Landroid/os/Message;", 25},
    {"Landroid/os/MessageQueue;", 40},
    {"Landroid/view/Gravity;", 10},
    {"Landroid/view/LayoutInflater;", 30},
    {"Lcom/android/internal/R$styleable;", 1},
    {"Llibcore/util/HexEncoding;", 10},
};
constexpr Minimum kServiceClasses[] = {
    {"Lcom/android/server/IntentResolver;", 30},
    {"Lcom/android/server/am/AppErrorResult;", 3},
    {"Lcom/android/server/pm/PackageKeySetData;", 8},
    {"Lcom/android/server/wm/utils/RotationCache;", 2},
};

void Verify(const char* path, std::span<const Minimum> minimums) {
    std::ifstream input(path, std::ios::binary);
    Require(input.is_open(), std::string("Cannot open ") + path);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>()};
    art::ArtDexFileLoader loader(bytes.data(), bytes.size(), path);
    std::string error;
    art::MemMap::Init();
    std::vector<std::unique_ptr<const art::DexFile>> dexes;
    const bool opened = loader.Open(/*verify=*/true, /*verify_checksum=*/true, &error, &dexes);
    Require(opened && !dexes.empty(), error);
    std::map<std::string_view, Counts> classes;
    Counts total;
    size_t method_ids = 0;
    for (const auto& dex : dexes) {
        method_ids += dex->NumMethodIds();
        for (const art::ClassAccessor cls : dex->GetClasses()) {
            auto& count = classes[cls.GetDescriptorView()];
            for (const auto& method : cls.GetMethods()) {
                ++count.methods;
                const auto code = method.GetInstructionsAndData();
                if (!code.HasCodeItem()) {
                    continue;
                }
                size_t offset = 0;
                for (auto insn = code.begin(); insn != code.end(); ++insn) {
                    Require(insn.DexPc() == offset, "Noncontiguous DEX instruction offsets");
                    offset += insn->SizeInCodeUnits();
                    if (insn->Opcode() != art::Instruction::NOP || insn->VRegA_10x() == 0) {
                        ++count.instructions;
                    }
                }
                Require(offset == code.InsnsSizeInCodeUnits(), "DEX instruction sizes disagree");
            }
            total.methods += count.methods;
            total.instructions += count.instructions;
        }
    }
    std::cout << "ART verified " << path << " with 0 errors, " << bytes.size()
              << " bytes in " << dexes.size() << " DEX files, " << classes.size() << " classes, "
              << total.methods << " defined methods, " << method_ids << " method IDs, "
              << total.instructions << " instructions\n";
    for (const auto& [descriptor, minimum] : minimums) {
        const auto it = classes.find(descriptor);
        Require(it != classes.end() && it->second.methods >= minimum,
                "Missing upstream class or methods for " + std::string(descriptor));
        const auto [methods, instructions] = it->second;
        std::cout << "  " << descriptor << ' ' << methods << " methods, "
                  << instructions << " instructions\n";
    }
}

jlong FixtureTime(JNIEnv*, jclass) {
    return 100'000'000;
}

jlong LocalIdentity(JNIEnv*, jclass) {
    return 0;
}

jlong InitQueue(JNIEnv*, jclass) {
    return 1;
}

void QueueSignal(JNIEnv*, jclass, jlong) {}

// All due work is posted before entering the loop. A blocking poll would
// indicate that dispatch failed to deliver the message that quits the queue.
void PollQueue(JNIEnv* env, jobject, jlong, jint timeout) {
    if (timeout != 0) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), "Unexpected blocking poll");
    }
}

void Dispatch(Vm& vm) {
    Register(vm, "android/os/SystemClock",
             Method<FixtureTime>(vm, "elapsedRealtimeNanos", "()J"));
    Register(vm, "android/os/Looper",
             Method<LocalIdentity>(vm, "nativeClearCallingIdentity", "()J"));
    Register(vm, "android/os/MessageQueue",
             Method<InitQueue>(vm, "nativeInit", "()J"),
             Method<QueueSignal>(vm, "nativeDestroy", "(J)V"),
             Method<QueueSignal>(vm, "nativeWake", "(J)V"),
             Method<PollQueue>(vm, "nativePollOnce", "(JI)V"));
    Require(vm.CallStatic("Lorg/wasmandroid/test/FrameworkProbe;", "dispatchMessages", "()I").i()
                == 2134, "Looper dispatch order or quitSafely failed");
    Require(vm.GetMethodStats("Landroid/os/Looper;", "loop", "()V").interpreted == 1,
            "The interpreter did not run Looper.loop");
    Require(vm.NativeCalls("Landroid/os/MessageQueue;") >= 6,
            "MessageQueue did not use its native poll interface");
    std::cout << "Executed upstream Looper, Handler and MessageQueue, delivered 2, 1, 3, 4\n";
}

// Test peer state uses the upstream graphics JNI signatures.
thread_local std::vector<wa_canvas_xy_t> translations;
thread_local std::vector<std::vector<wa_path_verb_t>> paths;
struct PaintPeer {
    uint32_t argb = 0xff000000;
    jint flags = 0;
    float text_size = 0;
};
thread_local std::vector<PaintPeer> paints;
thread_local wa_canvas_t* active_canvas = nullptr;
thread_local wa_bitmap_store_t* active_bitmaps = nullptr;
constexpr uint32_t kBitmapId = 1;

jlong InitMatrix(JNIEnv*, jclass, jlong source) {
    const auto translation = source ? translations.at(source - 1) : wa_canvas_xy_t{};
    translations.push_back(translation);
    return translations.size();
}

void SetTranslate(JNIEnv*, jclass, jlong handle, jfloat dx, jfloat dy) {
    translations.at(handle - 1) = {dx, dy};
}

void MapPoints(JNIEnv* env, jclass, jlong handle, jfloatArray dest, jint dest_index,
               jfloatArray source, jint source_index, jint count, jboolean points) {
    const auto [dx, dy] = translations.at(handle - 1);
    std::vector<jfloat> xy(2 * count);
    env->GetFloatArrayRegion(source, source_index, xy.size(), xy.data());
    if (env->ExceptionCheck()) {
        return;
    }
    if (points) {
        for (size_t i = 0; i < xy.size(); i += 2) {
            xy[i] += dx;
            xy[i + 1] += dy;
        }
    }
    env->SetFloatArrayRegion(dest, dest_index, xy.size(), xy.data());
}

jlong InitPath(JNIEnv*, jclass) {
    paths.emplace_back();
    return paths.size();
}

void MoveTo(JNIEnv*, jclass, jlong handle, jfloat x, jfloat y) {
    paths.at(handle - 1).push_back({WA_PATH_MOVE, {x, y}});
}

void LineTo(JNIEnv*, jclass, jlong handle, jfloat x, jfloat y) {
    paths.at(handle - 1).push_back({WA_PATH_LINE, {x, y}});
}

void ClosePath(JNIEnv*, jclass, jlong handle) {
    paths.at(handle - 1).push_back({WA_PATH_CLOSE, {}});
}

void RetainPeer(JNIEnv*, jclass, jobject, jlong) {
    // The fixture retains its native peers until this test's VM is destroyed.
}

jlong InitCanvas(JNIEnv*, jclass, jlong) {
    return reinterpret_cast<intptr_t>(active_canvas);
}

jobject RetainCanvas(JNIEnv*, jclass, jobject, jlong) {
    // The recording is finalized by the SurfaceFlinger transaction in Draw.
    return nullptr;
}

void DrawColor(JNIEnv* env, jclass, jlong handle, jint argb, jint mode) {
    auto* canvas = reinterpret_cast<wa_canvas_t*>(static_cast<intptr_t>(handle));
    if (canvas != active_canvas || mode != 3 // SkBlendMode::kSrcOver
            || wa_canvas_draw_color(canvas, static_cast<uint32_t>(argb)) != WA_SF_OK) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), "Canvas recording failed");
    }
}

jlong InitPaint(JNIEnv*, jclass) {
    paints.emplace_back();
    return paints.size();
}

void SetPaintFlags(JNIEnv*, jclass, jlong handle, jint flags) {
    paints.at(handle - 1).flags = flags;
}

jint PaintFlags(JNIEnv*, jclass, jlong handle) {
    return paints.at(handle - 1).flags;
}

void SetPaintColor(JNIEnv*, jclass, jlong handle, jint argb) {
    paints.at(handle - 1).argb = static_cast<uint32_t>(argb);
}

void SetTextSize(JNIEnv*, jclass, jlong handle, jfloat size) {
    paints.at(handle - 1).text_size = size;
}

jint FixtureLocaleId(JNIEnv*, jclass, jlong, jstring) {
    return 1;
}

void SetPaintOption(JNIEnv*, jclass, jlong, jint) {}

jboolean UiFontChangeEnabled(JNIEnv*, jclass, jlong) {
    return JNI_FALSE;
}

void DrawPath(JNIEnv* env, jclass, jlong canvas_handle, jlong path_handle, jlong paint_handle) {
    auto* canvas = reinterpret_cast<wa_canvas_t*>(static_cast<intptr_t>(canvas_handle));
    const auto& verbs = paths.at(path_handle - 1);
    const auto color = paints.at(paint_handle - 1).argb;
    if (canvas != active_canvas || wa_canvas_draw_path(canvas, verbs.data(), verbs.size(),
            WA_PATH_NONZERO, {color, WA_PAINT_FILL, 0}) != WA_SF_OK) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), "Path recording failed");
    }
}

void DrawRoundRect(JNIEnv* env, jclass, jlong handle, jfloat left, jfloat top,
                   jfloat right, jfloat bottom, jfloat rx, jfloat ry, jlong paint_handle) {
    auto* canvas = reinterpret_cast<wa_canvas_t*>(static_cast<intptr_t>(handle));
    const wa_paint_t paint{paints.at(paint_handle - 1).argb, WA_PAINT_FILL, 0};
    if (canvas != active_canvas
            || wa_canvas_draw_round_rect(canvas, {{left, top, right, bottom}, rx, ry}, paint) != WA_SF_OK) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), "Round rect recording failed");
    }
}

void DrawText(JNIEnv* env, jclass, jlong handle, jstring text, jint start, jint end,
              jfloat x, jfloat y, jint, jlong paint_handle) {
    auto* canvas = reinterpret_cast<wa_canvas_t*>(static_cast<intptr_t>(handle));
    if (canvas != active_canvas || start != 0 || end != env->GetStringLength(text)) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), "Unexpected text draw arguments");
        return;
    }
    const char* chars = env->GetStringUTFChars(text, nullptr);
    if (!chars) return;
    const auto& paint = paints.at(paint_handle - 1);
    const wa_canvas_text_t draw{0, static_cast<uint32_t>(std::strlen(chars)), x, y, 0,
        paint.text_size, 400, 0, paint.argb, 0};
    const auto result = wa_canvas_draw_text(canvas, &draw, chars);
    env->ReleaseStringUTFChars(text, chars);
    if (result != WA_SF_OK) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), "Text recording failed");
    }
}

void DrawPixels(JNIEnv* env, jclass, jlong handle, jintArray colors, jint offset, jint stride,
                jfloat x, jfloat y, jint width, jint height, jboolean has_alpha, jlong paint_handle) {
    auto* canvas = reinterpret_cast<wa_canvas_t*>(static_cast<intptr_t>(handle));
    if (canvas != active_canvas || width != 2 || height != 2 || paint_handle != 0) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), "Unexpected bitmap draw arguments");
        return;
    }
    std::array<uint8_t, 16> rgba{};
    for (jint row = 0; row < height; ++row) {
        std::array<jint, 2> argb{};
        env->GetIntArrayRegion(colors, offset + row * stride, width, argb.data());
        if (env->ExceptionCheck()) return;
        for (jint col = 0; col < width; ++col) {
            const auto pixel = static_cast<uint32_t>(argb[col]);
            const auto index = 4 * (row * width + col);
            rgba[index] = pixel >> 16;
            rgba[index + 1] = pixel >> 8;
            rgba[index + 2] = pixel;
            rgba[index + 3] = has_alpha ? pixel >> 24 : 255;
        }
    }
    const auto w = static_cast<float>(width);
    const auto h = static_cast<float>(height);
    const wa_canvas_bitmap_t draw{kBitmapId, WA_MASK_RECT, {0, 0, w, h}, {x, y, x + w, y + h}, 0, 1};
    if (wa_bitmap_put(active_bitmaps, kBitmapId, width, height, width * 4, rgba.data()) != WA_SF_OK
            || wa_canvas_draw_bitmap(canvas, &draw) != WA_SF_OK) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), "Bitmap recording failed");
    }
}

template <typename T>
T ReadRecord(const uint8_t* bytes, size_t size, size_t offset) {
    Require(offset <= size && size - offset >= sizeof(T), "Truncated SurfaceFlinger record");
    T value{};
    std::memcpy(&value, bytes + offset, sizeof(T));
    return value;
}

void Draw(Vm& vm, uint32_t argb) {
    using namespace wa::ui_jni::detail;
    Register(vm, "android/graphics/Matrix$ExtraNatives", Method<InitMatrix>(vm, "nCreate", "(J)J"));
    Register(vm, "android/graphics/Matrix",
             Method<RetainPeer>(vm, "nRegisterNativeAllocation", "(Landroid/graphics/Matrix;J)V"),
             Method<SetTranslate>(vm, "nSetTranslate", "(JFF)V"),
             Method<MapPoints>(vm, "nMapPoints", "(J[FI[FIIZ)V"));
    Register(vm, "android/graphics/Path",
             Method<RetainPeer>(vm, "nRegisterNativeAllocation", "(Landroid/graphics/Path;J)V"),
             Method<InitPath>(vm, "nInit", "()J"),
             Method<MoveTo>(vm, "nMoveTo", "(JFF)V"),
             Method<LineTo>(vm, "nLineTo", "(JFF)V"),
             Method<ClosePath>(vm, "nClose", "(J)V"));
    Register(vm, "android/graphics/Canvas",
             Method<InitCanvas>(vm, "nInitRaster", "(J)J"),
             Method<RetainCanvas>(vm, "nRegisterNativeAllocation",
                                  "(Landroid/graphics/Canvas;J)Ljava/lang/Runnable;"));
    Register(vm, "android/graphics/BaseCanvas",
             Method<DrawColor>(vm, "nDrawColor", "(JII)V"),
             Method<DrawPath>(vm, "nDrawPath", "(JJJ)V"),
             Method<DrawRoundRect>(vm, "nDrawRoundRect", "(JFFFFFFJ)V"),
             Method<DrawText>(vm, "nDrawText", "(JLjava/lang/String;IIFFIJ)V"),
             Method<DrawPixels>(vm, "nDrawBitmap", "(J[IIIFFIIZJ)V"));
    Register(vm, "android/graphics/Paint",
             Method<InitPaint>(vm, "nInit", "()J"),
             Method<RetainPeer>(vm, "nRegisterNativeAllocation", "(Landroid/graphics/Paint;J)V"),
             Method<SetPaintFlags>(vm, "nSetFlags", "(JI)V"),
             Method<PaintFlags>(vm, "nGetFlags", "(J)I"),
             Method<SetPaintColor>(vm, "nSetColor", "(JI)V"),
             Method<SetTextSize>(vm, "nSetTextSize", "(JF)V"),
             Method<FixtureLocaleId>(vm, "nSetTextLocales", "(JLjava/lang/String;)I"),
             Method<SetPaintOption>(vm, "nSetElegantTextHeight", "(JI)V"),
             Method<UiFontChangeEnabled>(vm, "isUiFontChangeEnabled", "(J)Z"));
    RegisterPath(vm);
    ScopedHandleScope scope(vm);
    const auto svg = scope.Hold(vm.NewString("M1,2 L3,2 L3,6 Z"));
    Require(vm.CallStatic("Landroid/graphics/Path;", "nativeParsePathData",
                          "(Ljava/lang/String;)[F", std::array{Value::Ref(svg)}).ref(),
            "The UI JNI did not parse path data");
    const auto path = scope.Hold(vm.CallStatic("Lorg/wasmandroid/test/FrameworkProbe;",
        "translatedPath", "()Landroid/graphics/Path;"));
    const auto& verbs = paths.at(vm.ReadField(path.ref(), "mNativePath", "J").j() - 1);
    Require(verbs.size() == 4, "Upstream path did not record four native verbs");

    const std::unique_ptr<wa_sf_t, decltype(&wa_sf_destroy)> surface(wa_sf_create(), wa_sf_destroy);
    Require(surface != nullptr, "Cannot create a SurfaceFlinger scene");
    Require(wa_sf_begin(surface.get(), 16, 16, 160) == WA_SF_OK, "Cannot begin frame");
    const wa_sf_layer_desc_t layer{.layer_id = 1, .width = 16, .height = 16,
        .scale_x = 1, .scale_y = 1, .alpha = 1, .flags = WA_SF_VISIBLE,
        .surface_kind = WA_SF_SURFACE_DISPLAY_LIST};
    Require(wa_sf_set_layer(surface.get(), &layer) == WA_SF_OK, "Cannot set layer");
    auto* canvas = wa_canvas_begin(surface.get(), layer.layer_id);
    Require(canvas != nullptr, "Cannot record the display list");
    active_canvas = canvas;
    active_bitmaps = wa_sf_bitmaps(surface.get());
    const auto java_canvas = scope.Hold(vm.NewObject("Landroid/graphics/Canvas;"));
    vm.CallVirtual(java_canvas, "drawARGB", "(IIII)V",
        std::array{Value::Int(0xff), Value::Int(0x24), Value::Int(0x31), Value::Int(0x47)});
    const auto paint = scope.Hold(vm.NewObject("Landroid/graphics/Paint;"));
    vm.CallVirtual(paint, "setColor", "(I)V", std::array{Value::Bits(argb)});
    vm.CallVirtual(java_canvas, "drawPath", "(Landroid/graphics/Path;Landroid/graphics/Paint;)V",
                   std::array{path, Value::Ref(paint)});
    vm.CallStatic("Lorg/wasmandroid/test/FrameworkProbe;", "drawPrimitives",
                  "(Landroid/graphics/Canvas;Landroid/graphics/Paint;)V",
                  std::array{Value::Ref(java_canvas), Value::Ref(paint)});
    active_canvas = nullptr;
    active_bitmaps = nullptr;
    Require(wa_canvas_end(canvas) == WA_SF_OK && wa_sf_commit(surface.get()) == WA_SF_OK,
            "SurfaceFlinger could not publish the display list");
    const auto release = [&surface](const wa_sf_frame_packet_t* frame) {
        wa_sf_frame_release(surface.get(), frame);
    };
    const std::unique_ptr<const wa_sf_frame_packet_t, decltype(release)> frame(
        wa_sf_frame_acquire(surface.get()), release);
    Require(frame && frame->layer_count == 1 && frame->viewport_w == 16,
            "Unexpected SurfaceFlinger frame metadata");
    const auto* entry = wa_sf_frame_layers(frame.get());
    Require(entry && entry->desc.layer_id == layer.layer_id, "Unexpected SurfaceFlinger layer");
    const uint8_t* list = wa_sf_frame_list(frame.get(), entry);
    Require(list != nullptr, "Missing SurfaceFlinger display list");
    const auto background = ReadRecord<wa_canvas_record_t>(list, entry->list_size, 0);
    Require(background.opcode == WA_CANVAS_DRAW_COLOR
                && background.byte_size == sizeof(background) + sizeof(uint32_t)
                && ReadRecord<uint32_t>(list, entry->list_size, sizeof(background)) == 0xff243147,
            "Upstream Canvas lost the background color");
    list += background.byte_size;
    const size_t remaining = entry->list_size - background.byte_size;
    const auto record = ReadRecord<wa_canvas_record_t>(list, remaining, 0);
    Require(record.opcode == WA_CANVAS_DRAW_PATH && record.byte_size <= remaining
                && record.byte_size >= sizeof(record) + sizeof(wa_canvas_path_t) + sizeof(uint32_t),
            "The frame does not contain the path after the background");
    const auto header = ReadRecord<wa_canvas_path_t>(list, record.byte_size, sizeof(record));
    Require(header.paint.argb == argb && header.verb_count == 4 && header.fill_rule == WA_PATH_NONZERO,
            "The path record lost drawing state");
    const size_t point_start = sizeof(record) + sizeof(header);
    const std::array coordinates{wa_canvas_xy_t{6, 9}, wa_canvas_xy_t{8, 9}, wa_canvas_xy_t{8, 13}};
    for (size_t index = 0; index < coordinates.size(); ++index) {
        const size_t offset = point_start + index * (sizeof(uint32_t) + sizeof(wa_canvas_xy_t));
        const auto command = ReadRecord<uint32_t>(list, record.byte_size, offset);
        const auto xy = ReadRecord<wa_canvas_xy_t>(list, record.byte_size, offset + sizeof(command));
        Require(command == (index == 0 ? WA_PATH_MOVE : WA_PATH_LINE)
                    && xy.x == coordinates[index].x && xy.y == coordinates[index].y,
                "The frame lost an upstream Path vertex or its Matrix translation");
    }
    Require(ReadRecord<uint32_t>(list, record.byte_size, record.byte_size - sizeof(uint32_t))
                == WA_PATH_CLOSE, "The path did not close");
    size_t offset = record.byte_size;
    const auto round = ReadRecord<wa_canvas_record_t>(list, remaining, offset);
    Require(round.opcode == WA_CANVAS_DRAW_ROUND_RECT
                && round.byte_size == sizeof(round) + sizeof(wa_canvas_round_draw_t),
            "Missing round rect record");
    const auto shape = ReadRecord<wa_canvas_round_draw_t>(list, remaining, offset + sizeof(round));
    Require(shape.shape.rect.l == 1 && shape.shape.rect.t == 2
                && shape.shape.rect.r == 14 && shape.shape.rect.b == 15
                && shape.shape.rx == 2 && shape.shape.ry == 3 && shape.paint.argb == argb,
            "The round rect record lost geometry or paint state");
    offset += round.byte_size;

    const auto text = ReadRecord<wa_canvas_record_t>(list, remaining, offset);
    Require(text.opcode == WA_CANVAS_DRAW_TEXT && text.byte_size <= remaining - offset,
            "Missing text record");
    const auto run = ReadRecord<wa_canvas_text_t>(list + offset, text.byte_size, sizeof(text));
    Require(run.x == 2 && run.y == 10 && run.font_size == 8 && run.argb == argb
                && run.utf8_len == 4 && run.utf8_offset <= text.byte_size
                && run.utf8_len <= text.byte_size - run.utf8_offset
                && std::memcmp(list + offset + run.utf8_offset, "AOSP", 4) == 0,
            "The text record lost its content, paint state or baseline");
    offset += text.byte_size;

    const auto bitmap = ReadRecord<wa_canvas_record_t>(list, remaining, offset);
    Require(bitmap.opcode == WA_CANVAS_DRAW_BITMAP && bitmap.byte_size == remaining - offset
                && bitmap.byte_size == sizeof(bitmap) + sizeof(wa_canvas_bitmap_t),
            "Missing bitmap record");
    const auto blit = ReadRecord<wa_canvas_bitmap_t>(list, remaining, offset + sizeof(bitmap));
    Require(blit.bitmap_id == kBitmapId && blit.src.r == 2 && blit.src.b == 2
                && blit.dst.l == 8 && blit.dst.t == 9 && blit.dst.r == 10 && blit.dst.b == 11,
            "The bitmap record lost the source and destination geometry");
    auto* store = wa_sf_bitmaps(surface.get());
    const auto release_bitmap = [store](const wa_bitmap_view_t* image) { wa_bitmap_release(store, image); };
    const std::unique_ptr<const wa_bitmap_view_t, decltype(release_bitmap)> image(
        wa_bitmap_acquire(store, blit.bitmap_id), release_bitmap);
    constexpr std::array<uint8_t, 16> expected{
        0x12, 0x34, 0x56, 0xff, 0x32, 0x63, 0x94, 0x80, 0x65, 0x43, 0x21, 0xff, 0x10, 0x20, 0x30, 0xff};
    Require(image && image->byte_size == expected.size()
                && std::memcmp(wa_bitmap_pixels(image.get()), expected.data(), expected.size()) == 0,
            "The bitmap upload lost pixels, stride or alpha");
    std::cout << "Executed upstream Canvas, Matrix, Path and Paint with verified path, round rect, text and bitmap records\n";
}

void Execute(Vm& vm) {
    ScopedHandleScope scope(vm);
    constexpr auto rect_type = "Landroid/graphics/Rect;";
    const auto rect = scope.Hold(vm.NewObject(rect_type, "(IIII)V",
        std::array{Value::Int(0), Value::Int(0), Value::Int(20), Value::Int(30)}));
    Require(vm.CallVirtual(rect, "intersect", "(IIII)Z",
        std::array{Value::Int(5), Value::Int(7), Value::Int(25), Value::Int(20)}).i(),
        "Rect intersection failed");
    Require(vm.CallVirtual(rect, "width", "()I").i() == 15
            && vm.CallVirtual(rect, "height", "()I").i() == 13
            && vm.ReadField(rect, "left", "I").i() == 5
            && vm.ReadField(rect, "top", "I").i() == 7, "Incorrect Rect intersection");
    const auto out = scope.Hold(vm.NewObject(rect_type));
    vm.CallStatic("Landroid/view/Gravity;", "apply", "(IIILandroid/graphics/Rect;Landroid/graphics/Rect;)V",
        std::array{Value::Int(17), Value::Int(3), Value::Int(5), Value::Ref(rect), Value::Ref(out)});
    Require(vm.ReadField(out, "left", "I").i() == 11 && vm.ReadField(out, "top", "I").i() == 11,
            "Gravity.CENTER did not position the child in the intersection");

    constexpr auto component_type = "Landroid/content/ComponentName;";
    const auto flattened = scope.Hold(vm.NewString("org.example/.Home"));
    const auto component = scope.Hold(vm.CallStatic(component_type, "unflattenFromString",
        "(Ljava/lang/String;)Landroid/content/ComponentName;", std::array{Value::Ref(flattened)}));
    Require(component.ref() != nullptr && vm.StringUtf8(vm.CallVirtual(component.ref(), "getClassName",
        "()Ljava/lang/String;").ref()) == "org.example.Home", "ComponentName unflatten failed");
    Require(vm.StringUtf8(vm.CallVirtual(component.ref(), "flattenToShortString",
        "()Ljava/lang/String;").ref()) == "org.example/.Home", "ComponentName roundtrip failed");

    const auto argb = vm.CallStatic("Landroid/graphics/Color;", "argb", "(IIII)I",
        std::array{Value::Int(0x99), Value::Int(0x12), Value::Int(0x34), Value::Int(0x56)});
    Require(static_cast<uint32_t>(argb.i()) == 0x99123456, "Color.argb channel packing failed");
    std::cout << "Executed upstream Rect, Gravity, ComponentName and Color\n";

    const auto bundle = scope.Hold(vm.NewObject("Landroid/os/Bundle;"));
    const auto key = scope.Hold(vm.NewString("component"));
    vm.CallVirtual(bundle, "putString", "(Ljava/lang/String;Ljava/lang/String;)V",
                   std::array{Value::Ref(key), Value::Ref(flattened)});
    const auto copy = scope.Hold(vm.NewObject("Landroid/os/Bundle;", "(Landroid/os/Bundle;)V",
                                               std::array{Value::Ref(bundle)}));
    Require(vm.CallVirtual(copy, "size", "()I").i() == 1
                && vm.StringUtf8(vm.CallVirtual(copy, "getString", "(Ljava/lang/String;)Ljava/lang/String;",
                                               std::array{Value::Ref(key)}).ref()) == "org.example/.Home",
            "Bundle did not preserve the component string across a copy");
    std::cout << "Executed upstream Bundle typed put, copy and get\n";

    const auto keysets = scope.Hold(vm.NewObject("Lcom/android/server/pm/PackageKeySetData;"));
    const auto alias = scope.Hold(vm.NewString("verified"));
    vm.CallVirtual(keysets, "addDefinedKeySet", "(JLjava/lang/String;)V",
                   std::array{Value::Long(41), Value::Ref(alias)});
    vm.CallVirtual(keysets, "addUpgradeKeySet", "(Ljava/lang/String;)V",
                   std::array{Value::Ref(alias)});
    Require(vm.CallVirtual(keysets, "isUsingUpgradeKeySets", "()Z").i(),
            "Package Manager did not recognize an upgrade keyset");
    vm.CallVirtual(keysets, "removeAllUpgradeKeySets", "()V");
    Require(!vm.CallVirtual(keysets, "isUsingUpgradeKeySets", "()Z").i(),
            "Package Manager did not clear the upgrade keysets");
    std::cout << "Executed upstream PackageKeySetData alias and upgrade-keyset operations\n";
    Require(vm.CallStatic("Lorg/wasmandroid/test/FrameworkProbe;", "indexFilters", "()I").i() == 0,
            "IntentResolver indexing, MIME matching or filter removal failed");
    std::cout << "Executed upstream IntentResolver and IntentFilter MIME matching\n";

    Dispatch(vm);
    Draw(vm, static_cast<uint32_t>(argb.i()));
}

void ReadFrameworkResources(Vm& vm, const char* framework_res) {
    auto* env = vm.JniEnv();
    Require(env->PushLocalFrame(32) == JNI_OK, "Cannot allocate a resource JNI frame");
    for (auto register_natives : {android::register_android_content_StringBlock,
                                 android::register_android_content_XmlBlock,
                                 android::register_android_content_res_ApkAssets}) {
        Require(register_natives(env) == JNI_OK, "Cannot register upstream resource JNI");
    }
    env->PopLocalFrame(nullptr);

    ScopedHandleScope scope(vm);
    constexpr auto apk_type = "Landroid/content/res/ApkAssets;";
    constexpr auto inflater_type = "Landroid/view/LayoutInflater;";
    constexpr auto scan_layout = "(Lorg/xmlpull/v1/XmlPullParser;)V";
    constexpr auto load_apk = "(Ljava/lang/String;)Landroid/content/res/ApkAssets;";
    constexpr auto open_xml = "(Ljava/lang/String;)Landroid/content/res/XmlResourceParser;";
    const auto path = scope.Hold(Value::Ref(vm.NewString(framework_res)));
    const auto assets = scope.Hold(vm.CallStatic(apk_type, "loadFromPath", load_apk, std::array{path}));
    Require(vm.StringUtf8(vm.CallVirtual(assets.ref(), "getAssetPath", "()Ljava/lang/String;").ref())
                == framework_res, "ApkAssets loaded an unexpected file");
    const auto layout = scope.Hold(Value::Ref(vm.NewString("res/layout/simple_list_item_1.xml")));
    const auto parser = scope.Hold(vm.CallVirtual(assets.ref(), "openXml", open_xml, std::array{layout}));
    const std::array parser_args{parser};
    vm.CallStatic(inflater_type, "advanceToRootNode", scan_layout, parser_args);
    Require(vm.CallStatic("Lorg/wasmandroid/test/FrameworkProbe;", "checkLayoutAttributes",
                          "(Landroid/content/res/XmlResourceParser;)Z", parser_args).i(),
            "The framework binary layout disagrees with its generated resource IDs");
    vm.CallStatic(inflater_type, "consumeChildElements", scan_layout, parser_args);
    Require(vm.CallVirtual(parser.ref(), "getEventType", "()I").i() == 3 // END_TAG
                && vm.CallVirtual(parser.ref(), "next", "()I").i() == 1, // END_DOCUMENT
            "LayoutInflater did not consume the complete layout element");
    vm.CallVirtual(parser.ref(), "close", "()V");
    vm.CallVirtual(assets.ref(), "close", "()V");
    Require(vm.GetMethodStats(apk_type, "loadFromPath", load_apk).interpreted == 1
            && vm.GetMethodStats(inflater_type, "advanceToRootNode", scan_layout).interpreted == 1
            && vm.GetMethodStats(inflater_type, "consumeChildElements", scan_layout).interpreted == 1
            && vm.NativeCalls(apk_type) >= 4
            && vm.NativeCalls("Landroid/content/res/XmlBlock;") >= 4
            && vm.NativeCalls("Landroid/content/res/StringBlock;") > 0,
            "The framework layout did not use upstream resource JNI and Java bytecode");
    std::cout << "Traversed upstream simple_list_item_1 binary XML with ApkAssets, XmlBlock and LayoutInflater\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Require(argc >= 4, "Expected mode, framework DEX and services DEX");
#ifdef __EMSCRIPTEN__
        static_assert(sizeof(void*) == 4);
#endif
        const std::string_view mode(argv[1]);
        if (mode == "verify") {
            Verify(argv[2], kFrameworkClasses);
            Verify(argv[3], kServiceClasses);
        } else if (mode == "execute" || mode == "resources") {
            Require(argc == (mode == "resources" ? 6 : 5), "Expected runtime fixture inputs");
            Vm vm;
            try {
                vm.LoadDexFile(argv[2]);
                vm.LoadDexFile(argv[3]);
                vm.LoadDexFile(argv[4]);
                if (mode == "resources") {
                    ReadFrameworkResources(vm, argv[5]);
                } else {
                    Execute(vm);
                }
            } catch (const wa::interp::ManagedException&) {
                throw std::runtime_error(vm.ExceptionSummary());
            }
        } else {
            throw std::runtime_error("Unknown mode");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
