#include "foreground_test.h"
#include "foreground_test_access.h"
#include "hybrid_native_runtime.h"
#include "runtime_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <utility>

namespace {

constexpr auto kTinker = "Lcom/tencent/tinker/loader/app/TinkerApplication;";
constexpr auto kAppLike = "Lcom/tencent/mm/app/MMApplicationLike;";
constexpr auto kFence = "(Landroid/app/Application;ILjava/lang/String;ZJJLandroid/content/Intent;)Landroid/os/Handler;";
constexpr auto kAppLikeConstructor = "(Landroid/app/Application;IZJJLandroid/content/Intent;)V";
constexpr auto kLauncher = "Lcom/tencent/mm/ui/LauncherUI;";
constexpr auto kWelcome = "Lcom/tencent/mm/plugin/account/ui/WelcomeActivity;";
constexpr auto kLanguage = "Lcom/tencent/mm/plugin/setting/ui/setting/SettingsLanguageUI;";
constexpr auto kLogin = "Lcom/tencent/mm/plugin/account/ui/MobileInputUI;";
constexpr auto kReport = "Lcom/tencent/mm/plugin/report/service/o0;";
constexpr auto kHttpClient = "Lorg/apache/http/impl/client/DefaultHttpClient;";
constexpr auto kMatrix = "Lcom/tencent/matrix/hook/jnihook/JniHookJni;";
constexpr auto kPackageManager = "Landroid/content/pm/PackageManager;";
constexpr auto kFirebaseProvider = "Lcom/google/firebase/provider/FirebaseInitProvider;";
constexpr auto kRemoteProviders = std::array{
    "Landroidx/core/content/FileProvider;",
    "Lcom/tencent/xweb/XWebCoreContentProvider;",
    "Lcom/tencent/shadow/core/runtime/container/PluginContainerContentProvider;",
};
constexpr std::string_view kMatrixLibrary = "libmatrix-jnihook.so";
constexpr auto kV8Library = "libmmj2v8.so";
constexpr auto kPayMarsContext = "Lcom/tencent/paymars/app/Context;";
constexpr auto kWallet = "Lcom/tenpay/ndk/FitScUtil;";
constexpr auto kWalletInit = "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z";
constexpr auto kPool = "Ljava/util/concurrent/ForkJoinPool;";
constexpr auto kPoolConstructor =
    "(ILjava/util/concurrent/ForkJoinPool$ForkJoinWorkerThreadFactory;Ljava/lang/Thread$UncaughtExceptionHandler;"
    "ZIIILjava/util/function/Predicate;JLjava/util/concurrent/TimeUnit;)V";
constexpr int kSteadyWelcomeFrames = 65;
constexpr int64_t kFrameNanos = 16'666'667;

using hybrid_test::Check;
using hybrid_test::Require;
using wa::interp::Object;
using wa::interp::ScopedHandleScope;
using wa::interp::Value;
using wa::interp::Vm;

std::string JsonString(std::string_view value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') out << '\\';
        if (ch >= 32) out << ch;
        else out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(ch);
    }
    out << '"';
    return out.str();
}

uint64_t Metric(const std::string& state, std::string_view name) {
    const auto key = JsonString(name) + ':';
    const auto offset = state.find(key);
    Require(offset != state.npos, "Missing foreground metric " + std::string(name));
    return std::stoull(state.substr(offset + key.size()));
}

void PrintLibraryRequest(std::string_view name) {
    std::cout << "{\"event\":\"native_library_request\",\"name\":" << JsonString(name) << "}\n";
}

void PrintNativeImages(Vm& vm) {
    for (const auto& image : vm.GetGuestNativeImages()) {
        std::cout << "{\"event\":\"native_image\",\"name\":" << JsonString(image.name)
                  << ",\"mapped_base\":" << image.mapped_base
                  << ",\"JNI_OnLoad\":" << JsonString(image.jni_on_load)
                  << ",\"load_host_calls\":" << image.host_calls
                  << ",\"load_callbacks\":" << image.callbacks
                  << ",\"error\":" << JsonString(image.error) << "}\n";
    }
}

void PrintStartupMethods(Vm& vm) {
    const auto method = [&](std::string_view owner, std::string_view name, std::string_view signature) {
        const auto stats = vm.GetLinkedDeclaredMethodStats(owner, name, signature);
        std::cout << "{\"event\":\"startup_method\",\"owner\":" << JsonString(owner)
                  << ",\"name\":" << JsonString(name) << ",\"entered\":" << stats.invocations
                  << ",\"returned\":" << stats.returned << "}\n";
    };
    method(kTinker, "onBaseContextAttached", "(Landroid/content/Context;JJ)V");
    method(kTinker, "createInlineFence", kFence);
    method(kAppLike, "<init>", kAppLikeConstructor);
    method(kAppLike, "onBaseContextAttached", "(Landroid/content/Context;)V");
    method(kAppLike, "onCreate", "()V");
    method(kTinker, "onCreate", "()V");
    method(kPool, "<init>", kPoolConstructor);
    method(kMatrix, "jniPlaceHolder", "()V");
    method(kMatrix, "initJniHookByPlaceHolderMethod", "(Ljava/lang/reflect/Method;)I");
    method(kPackageManager, "installEmbeddedProviders", "(Landroid/content/Context;)V");
    method(kPayMarsContext, "OnJniCreateContext", "(Ljava/lang/String;)V");
    method(kWallet, "init", kWalletInit);
    method(kFirebaseProvider, "onCreate", "()Z");
    method(kLauncher, "onCreate", "(Landroid/os/Bundle;)V");
    method(kLauncher, "onStart", "()V");
    method(kLauncher, "onResume", "()V");
    method(kWelcome, "onCreate", "(Landroid/os/Bundle;)V");
    method(kWelcome, "onResume", "()V");
}

void ProbePackageMetadata(Vm& vm) {
    ScopedHandleScope roots(vm);
    constexpr auto kResources = "Landroid/content/res/Resources;";
    constexpr auto kUiRuntime = "Landroid/app/UiRuntime;";
    auto dex = vm.DexLocation(kLauncher);
    auto* path = roots.Hold(vm.NewString(dex.substr(0, dex.rfind('!'))));
    auto* locale = roots.Hold(vm.NewString("en-US"));
    auto* activity = roots.Hold(vm.NewString("com.tencent.mm.ui.LauncherUI"));
    auto* resources = roots.Hold(vm.CallStatic(kResources, "forApk",
        "(Ljava/lang/String;Ljava/lang/String;I)Landroid/content/res/Resources;",
        std::array{Value::Ref(path), Value::Ref(locale), Value::Int(160)}).ref());
    auto* manifest = roots.Hold(vm.CallStatic(kUiRuntime, "readManifest",
        "(Landroid/content/res/Resources;Ljava/lang/String;Ljava/lang/String;I)Landroid/app/UiRuntime$Manifest;",
        std::array{Value::Ref(resources), Value::Ref(path), Value::Ref(activity), Value::Int(0)}).ref());
    auto info = roots.Hold(vm.ReadField(manifest, "info", "Landroid/content/pm/ApplicationInfo;"));
    Require(vm.ReadField(info.ref(), "targetSdkVersion", "I").i() == 34,
            "ApplicationInfo reports the WeChat target SDK");
    auto* context = roots.Hold(vm.NewObject("Landroid/app/UiRuntime$AppContext;",
        "(Landroid/content/res/Resources;Landroid/content/pm/ApplicationInfo;Landroid/app/UiRuntime$Manifest;)V",
        std::array{Value::Ref(resources), info, Value::Ref(manifest)}));
    auto service = roots.Hold(Value::Ref(vm.NewString("activity")));
    auto* activities = roots.Hold(vm.CallVirtual(context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;",
        std::array{service}).ref());
    Require(activities, "Context exposes ActivityManager");
    auto* processes = roots.Hold(vm.CallVirtual(activities, "getRunningAppProcesses", "()Ljava/util/List;").ref());
    Require(processes && vm.CallVirtual(processes, "size", "()I").i() == 1, "ActivityManager reports the app process");
    auto* process = roots.Hold(vm.CallVirtual(processes, "get", "(I)Ljava/lang/Object;", std::array{Value::Int(0)}).ref());
    Require(vm.StringUtf8(vm.ReadField(process, "processName", "Ljava/lang/String;").ref()) == "com.tencent.mm" &&
            vm.ReadField(process, "pid", "I").i() == vm.CallStatic("Landroid/os/Process;", "myPid", "()I").i() &&
            vm.ReadField(process, "uid", "I").i() == vm.CallStatic("Landroid/os/Process;", "myUid", "()I").i() &&
            vm.ReadField(process, "importance", "I").i() == 100, "ActivityManager and /proc report the same process");
    auto* manager = roots.Hold(vm.CallVirtual(context, "getPackageManager", "()Landroid/content/pm/PackageManager;").ref());
    auto name = roots.Hold(Value::Ref(vm.NewString("com.tencent.mm")));
    const auto package = [&](int flags) {
        return roots.Hold(vm.CallVirtual(manager, "getPackageInfo",
            "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", std::array{name, Value::Int(flags)}).ref());
    };
    auto* version = package(0);
    Require(vm.CallVirtual(version, "getLongVersionCode", "()J").j() == 3220 &&
            vm.StringUtf8(vm.ReadField(version, "versionName", "Ljava/lang/String;").ref()) ==
                "8.0.80", "PackageManager returns the APK version from its manifest");

    constexpr auto kSignatures = "[Landroid/content/pm/Signature;";
    constexpr auto kSigningInfo = "Landroid/content/pm/SigningInfo;";
    auto* legacy = roots.Hold(vm.ReadField(package(0x40), "signatures", kSignatures).ref());
    auto* signing = roots.Hold(vm.ReadField(package(0x08000000), "signingInfo", kSigningInfo).ref());
    Require(signing && vm.CallVirtual(signing, "getSchemeVersion", "()I").i() == 2 &&
            !vm.CallVirtual(signing, "hasMultipleSigners", "()Z").i(),
            "PackageManager reports the v2 signer through GET_SIGNING_CERTIFICATES");
    auto* current = roots.Hold(vm.CallVirtual(signing, "getApkContentsSigners",
        "()[Landroid/content/pm/Signature;").ref());
    auto* history = roots.Hold(vm.CallVirtual(signing, "getSigningCertificateHistory",
        "()[Landroid/content/pm/Signature;").ref());
    constexpr std::string_view expected =
        "0fe4ff85c215918396dadc7cd8ce6963339af33d37751a56e54c7206b63a3c7c";
    for (auto* signers : {legacy, current, history}) {
        Require(signers && signers->elements.size() == 1, "PackageManager returns the single APK signer");
        auto certificate = roots.Hold(vm.CallVirtual(signers->elements[0].ref(), "toByteArray", "()[B"));
        constexpr auto kDigest = "Ljava/security/MessageDigest;";
        auto algorithm = roots.Hold(Value::Ref(vm.NewString("SHA-256")));
        auto* digest = roots.Hold(vm.CallStatic(kDigest, "getInstance",
            "(Ljava/lang/String;)Ljava/security/MessageDigest;", std::array{algorithm}).ref());
        auto hash = roots.Hold(vm.CallVirtual(digest, "digest", "([B)[B", std::array{certificate}));
        auto* hex = roots.Hold(vm.CallStatic("Llibcore/util/HexEncoding;", "encodeToString",
            "([BZ)Ljava/lang/String;", std::array{hash, Value::Int(0)}).ref());
        Require(vm.StringUtf8(hex) == expected, "PackageManager signer differs from apksigner's verified certificate");
    }
    vm.CallVirtual(resources, "close", "()V");
    std::cout << "{\"event\":\"apk_signing\",\"scheme\":2,\"sha256\":" << JsonString(expected) << "}\n";
}

void ProbeInstalledProviders(Vm& vm) {
    ScopedHandleScope roots(vm);
    auto* application = roots.Hold(vm.CallStatic("Landroid/app/ActivityThread;", "currentApplication",
        "()Landroid/app/Application;").ref());
    auto* manager = roots.Hold(vm.CallVirtual(application, "getPackageManager",
        "()Landroid/content/pm/PackageManager;").ref());
    auto* installed = roots.Hold(vm.ReadField(manager, "installedProviders", "Ljava/util/ArrayList;").ref());
    const int32_t count = vm.CallVirtual(installed, "size", "()I").i();
    Require(count == 27, "WeChat must install its 27 main-process manifest providers");
    for (int32_t index = 0; index < count; ++index) {
        const auto* instance = vm.CallVirtual(installed, "get", "(I)Ljava/lang/Object;",
            std::array{Value::Int(index)}).ref();
        Require(instance && std::ranges::find(kRemoteProviders, instance->type->descriptor) == kRemoteProviders.end(),
                "WeChat installed a provider assigned to a named process");
    }
    std::cout << "{\"event\":\"manifest_providers\",\"main_process_installed\":" << count << "}\n";
}

size_t CountBitmapPixels(wa_bitmap_store_t* store, const wa_canvas_bitmap_t& draw) {
    if (!(draw.alpha > 0 && draw.dst.l < draw.dst.r && draw.dst.t < draw.dst.b)) return 0;
    const auto release = [store](const wa_bitmap_view_t* bitmap) { wa_bitmap_release(store, bitmap); };
    const std::unique_ptr<const wa_bitmap_view_t, decltype(release)> bitmap(
        wa_bitmap_acquire(store, draw.bitmap_id), release);
    Require(bitmap && bitmap->stride >= uint64_t(bitmap->width) * 4 &&
            uint64_t(bitmap->stride) * bitmap->height == bitmap->byte_size,
            "Invalid drawable bitmap in the SurfaceFlinger frame");
    const auto clamp = [](float position, uint32_t limit) {
        Require(std::isfinite(position), "Invalid drawable bitmap bounds");
        return static_cast<uint32_t>(std::clamp(position, 0.f, static_cast<float>(limit)));
    };
    const auto first_x = clamp(std::floor(draw.src.l), bitmap->width);
    const auto first_y = clamp(std::floor(draw.src.t), bitmap->height);
    const auto end_x = clamp(std::ceil(draw.src.r), bitmap->width);
    const auto end_y = clamp(std::ceil(draw.src.b), bitmap->height);
    if (first_x >= end_x || first_y >= end_y) return 0;
    const auto* pixels = wa_bitmap_pixels(bitmap.get());
    const auto* first = pixels + size_t(first_y) * bitmap->stride + size_t(first_x) * 4;
    size_t count = 0;
    for (auto y = first_y; y < end_y; ++y) {
        const auto* row = pixels + size_t(y) * bitmap->stride;
        for (auto x = first_x; x < end_x; ++x) {
            const auto* pixel = row + size_t(x) * 4;
            if (pixel[3] && std::memcmp(pixel, first, 4) != 0) ++count;
        }
    }
    return count;
}

struct FrameEvidence {
    struct Label {
        float x, y;
        uint32_t color;
    };
    uint32_t sequence = 0;
    size_t draws = 0;
    size_t bitmap_pixels = 0;
    std::map<std::string, Label, std::less<>> labels;
    std::set<uint32_t> rounded_fills;

    bool ShowsWelcome() const {
        return draws && bitmap_pixels && labels.contains("Log In") && labels.contains("Sign Up") &&
               labels.contains("语言");
    }

    bool ShowsSettledWelcome() const {
        return ShowsWelcome() && rounded_fills.contains(0xff07c160) && rounded_fills.contains(0xffffffff) &&
               labels.at("Log In").color == 0xffffffff && labels.at("语言").color == 0xffffffff &&
               labels.at("Sign Up").color == 0xb4000000;
    }
};

FrameEvidence ReadFrame(const std::filesystem::path& capture = {}) {
    auto* scene = wa_system_app_sf_ctx();
    const auto release = [scene](const wa_sf_frame_packet_t* frame) { wa_sf_frame_release(scene, frame); };
    const std::unique_ptr<const wa_sf_frame_packet_t, decltype(release)> frame(
        wa_sf_frame_acquire(scene), release);
    if (!frame) return {};
    FrameEvidence evidence{.sequence = frame->sequence};
    std::set<uint32_t> bitmap_ids;
    auto* bitmaps = wa_sf_bitmaps(scene);
    const auto* layers = wa_sf_frame_layers(frame.get());
    for (uint32_t i = 0; i < frame->layer_count; ++i) {
        const auto& layer = layers[i];
        if (!(layer.desc.flags & WA_SF_VISIBLE) || layer.desc.alpha <= 0 ||
            layer.desc.surface_kind != WA_SF_SURFACE_DISPLAY_LIST) continue;
        const auto* list = wa_sf_frame_list(frame.get(), &layer);
        Require(list || !layer.list_size, "Missing display list");
        struct Transform { float x, y, sx, sy; };
        Transform transform{layer.desc.x - layer.desc.scroll_x, layer.desc.y - layer.desc.scroll_y,
                            layer.desc.scale_x, layer.desc.scale_y};
        std::vector<Transform> stack;
        for (size_t offset = 0; offset < layer.list_size;) {
            wa_canvas_record_t record{};
            const auto remaining = layer.list_size - offset;
            Require(remaining >= sizeof(record), "Truncated display list record");
            std::memcpy(&record, list + offset, sizeof(record));
            Require(record.byte_size >= sizeof(record) && record.byte_size <= remaining,
                    "Invalid display list record size");
            const auto* payload = list + offset + sizeof(record);
            if (record.opcode >= WA_CANVAS_DRAW_COLOR && record.opcode <= WA_CANVAS_DRAW_TEXT) ++evidence.draws;
            switch (record.opcode) {
            case WA_CANVAS_SAVE: stack.push_back(transform); break;
            case WA_CANVAS_RESTORE:
                Require(!stack.empty(), "Unbalanced canvas restore");
                transform = stack.back();
                stack.pop_back();
                break;
            case WA_CANVAS_TRANSLATE:
            case WA_CANVAS_SCALE: {
                wa_canvas_xy_t xy{};
                Require(record.byte_size >= sizeof(record) + sizeof(xy), "Truncated canvas transform");
                std::memcpy(&xy, payload, sizeof(xy));
                if (record.opcode == WA_CANVAS_TRANSLATE) {
                    transform.x += transform.sx * xy.x;
                    transform.y += transform.sy * xy.y;
                } else {
                    transform.sx *= xy.x;
                    transform.sy *= xy.y;
                }
                break;
            }
            case WA_CANVAS_DRAW_BITMAP: {
                wa_canvas_bitmap_t draw{};
                Require(record.byte_size >= sizeof(record) + sizeof(draw), "Truncated drawable bitmap");
                std::memcpy(&draw, payload, sizeof(draw));
                evidence.bitmap_pixels += CountBitmapPixels(bitmaps, draw);
                if (!capture.empty()) bitmap_ids.insert(draw.bitmap_id);
                break;
            }
            case WA_CANVAS_DRAW_ROUND_RECT: {
                wa_canvas_round_draw_t draw{};
                Require(record.byte_size >= sizeof(record) + sizeof(draw), "Truncated rounded rectangle");
                std::memcpy(&draw, payload, sizeof(draw));
                if (draw.paint.style == WA_PAINT_FILL && (draw.paint.argb >> 24))
                    evidence.rounded_fills.insert(draw.paint.argb);
                break;
            }
            case WA_CANVAS_DRAW_TEXT: {
                wa_canvas_text_t text{};
                Require(record.byte_size >= sizeof(record) + sizeof(text), "Truncated draw text");
                std::memcpy(&text, payload, sizeof(text));
                Require(text.utf8_offset >= sizeof(record) + sizeof(text) &&
                        text.utf8_offset <= record.byte_size &&
                        text.utf8_len <= record.byte_size - text.utf8_offset,
                        "Invalid display list text bounds");
                if (!text.utf8_len || !(text.argb >> 24)) break;
                const auto* label = reinterpret_cast<const char*>(list + offset + text.utf8_offset);
                const auto metrics = wa_canvas_measure_text(scene, label, text.utf8_len, text.font_size,
                                                            text.weight, text.font_face);
                const float width = text.max_width > 0 ? std::min(metrics.advance, text.max_width) : metrics.advance;
                const FrameEvidence::Label appearance{
                    transform.x + transform.sx * (text.x + width * (1.f - text.align) / 2.f),
                    transform.y + transform.sy * (text.y + (metrics.descent - metrics.ascent) / 2.f),
                    text.argb};
                evidence.labels.try_emplace(std::string(label, text.utf8_len), appearance);
                break;
            }
            default: break;
            }
            offset += record.byte_size;
        }
    }
    if (!capture.empty()) {
        std::ofstream output(capture, std::ios::binary);
        output.write(reinterpret_cast<const char*>(frame.get()), frame->byte_size);
        for (const auto id : bitmap_ids) {
            const auto* bitmap = wa_bitmap_acquire(bitmaps, id);
            Require(bitmap != nullptr, "Missing captured bitmap");
            output.write(reinterpret_cast<const char*>(bitmap), sizeof(*bitmap));
            output.write(reinterpret_cast<const char*>(wa_bitmap_pixels(bitmap)), bitmap->byte_size);
            wa_bitmap_release(bitmaps, bitmap);
        }
        Require(static_cast<bool>(output), "Write SurfaceFlinger capture " + capture.string());
    }
    return evidence;
}

void PrintFrame(const FrameEvidence& evidence, int welcome_frames) {
    std::cout << "{\"event\":\"surface_frame\",\"sequence\":" << evidence.sequence
              << ",\"welcome_frames\":" << welcome_frames << ",\"draw_commands\":" << evidence.draws
              << ",\"non_blank_bitmap_pixels\":" << evidence.bitmap_pixels << ",\"drawn_text\":[";
    for (auto label = evidence.labels.begin(); label != evidence.labels.end(); ++label) {
        if (label != evidence.labels.begin()) std::cout << ',';
        std::cout << JsonString(label->first);
    }
    std::cout << "]}\n";
}

void Tap(const FrameEvidence& frame, std::string_view label, int ticks) {
    const auto found = frame.labels.find(label);
    Require(found != frame.labels.end(), "Missing tap target " + std::string(label));
    const auto& target = found->second;
    auto* ring = wa_sf_input(wa_system_app_sf_ctx());
    for (auto action : {WA_ACTION_DOWN, WA_ACTION_UP}) {
        const wa_input_event_t event{.kind = WA_INPUT_MOTION, .action = action,
            .event_time_ms = ticks * kFrameNanos / 1e6 + 2 * action, .x = target.x, .y = target.y};
        Check(wa_input_push(ring, &event), label);
    }
}

void Advance(int& ticks, int count, std::string_view operation) {
    for (int i = 0; i < count; ++i) Check(wa_system_app_tick(++ticks * kFrameNanos), operation);
}

std::string ResumedActivity() {
    std::array<char, 256> observed{};
    Check(wa::system::RunForegroundProbe([](Vm& vm, Object* activity) {
        const auto& descriptor = activity->type->descriptor;
        Require(vm.GetLinkedVirtualMethodStats(descriptor, "onCreate", "(Landroid/os/Bundle;)V").returned > 0 &&
                vm.GetLinkedVirtualMethodStats(descriptor, "onStart", "()V").returned > 0 &&
                vm.GetLinkedVirtualMethodStats(descriptor, "onResume", "()V").returned > 0 &&
                vm.CallVirtual(activity, "isResumed", "()Z").i(),
                "Visible activity did not complete its lifecycle " + descriptor);
        std::cout << "{\"event\":\"resumed_activity\",\"class\":" << JsonString(descriptor) << "}\n";
        return descriptor;
    }, observed.data(), observed.size()), "Check the resumed WeChat activity");
    return observed.data();
}

std::string FocusedEditorText() {
    std::array<char, 256> observed{};
    Check(wa::system::RunForegroundProbe([](Vm& vm, Object* activity) {
        ScopedHandleScope roots(vm);
        auto* focused = roots.Hold(vm.CallVirtual(activity, "getCurrentFocus", "()Landroid/view/View;").ref());
        const auto* type = focused ? focused->type : nullptr;
        while (type && type->descriptor != "Landroid/widget/EditText;") type = type->super;
        Require(type && vm.CallVirtual(focused, "isFocused", "()Z").i(), "The login editor did not take focus");
        auto* editable = roots.Hold(vm.CallVirtual(focused, "getText", "()Landroid/text/Editable;").ref());
        return vm.StringUtf8(vm.CallVirtual(editable, "toString", "()Ljava/lang/String;").ref());
    }, observed.data(), observed.size()), "Read the focused login editor");
    return observed.data();
}

void VerifyPhoneInput(const FrameEvidence& frame, std::string_view hint, int& ticks) {
    Tap(frame, hint, ticks);
    Advance(ticks, 6, "Focus WeChat's phone number field");
    Require(FocusedEditorText().empty(), "WeChat's phone number field starts empty");
    constexpr std::string_view number = "2025550123";
    wa_input_event_t input{.kind = WA_INPUT_IME, .action = WA_IME_COMMIT,
        .text_len = number.size(), .event_time_ms = ticks * kFrameNanos / 1e6};
    std::ranges::copy(number, input.text);
    Check(wa_input_push(wa_sf_input(wa_system_app_sf_ctx()), &input), "Send phone digits to the login editor");
    Advance(ticks, 6, "Draw the entered phone number");
    const auto digits = [](std::string_view value) {
        std::string result;
        std::ranges::copy_if(value, std::back_inserter(result), [](char c) { return c >= '0' && c <= '9'; });
        return result;
    };
    Require(digits(FocusedEditorText()) == number, "WeChat's login editor did not accept the typed digits");
    const auto typed_frame = ReadFrame();
    Require(std::ranges::any_of(typed_frame.labels, [&](const auto& label) { return digits(label.first) == number; }),
            "WeChat did not render the typed phone number");
    std::cout << "{\"event\":\"login_input\",\"digits\":" << number.size() << "}\n";
}

std::string_view Phase(const std::string& state) {
    if (Metric(state, "frames")) return "frame_rendered";
    if (Metric(state, "onResumeCalls")) return "activity_resumed";
    if (Metric(state, "onStartCalls")) return "activity_started";
    if (Metric(state, "onCreateCalls")) return "activity_created";
    if (Metric(state, "applicationCreateReturns")) return "application_onCreate_returned";
    if (Metric(state, "applicationCreateCalls")) return "application_onCreate_entered";
    if (Metric(state, "providersCreated")) return "providers_created";
    if (Metric(state, "attachBaseContextReturns")) return "attachBaseContext_returned";
    if (Metric(state, "attachBaseContextCalls")) return "attachBaseContext_entered";
    if (Metric(state, "dexFiles")) return "dex_loaded";
    return "apk_open";
}

void ProbeNativeLibraries(const char* framework, const char* fallback, const char* apk, bool core) {
    struct TemporaryDirectory {
        char path[sizeof("/tmp/wa-wechat-mmkv-XXXXXX")] = "/tmp/wa-wechat-mmkv-XXXXXX";
        TemporaryDirectory() { Require(mkdtemp(path) != nullptr, "Create MMKV store"); }
        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } store;

    Vm vm({.capture_exception_stack = true});
    vm.SetGuestLibraryObserver(PrintLibraryRequest);
    vm.LoadDexFile(framework);
    vm.LoadFallbackDexFile(fallback);
    const auto apk_path = std::filesystem::absolute(apk).string();
    vm.LoadApkDexFiles(apk_path);
    wa::system::MountGuestApkLibraries(vm, apk_path, "com.tencent.mm");
    try {
        constexpr auto mmkv = "Lcom/tencent/mmkv/MMKV;";
        ScopedHandleScope roots(vm);
        if (core) {
            for (const auto name : {"stlport_shared", "cso", "wechatxlog"}) {
                ScopedHandleScope load_roots(vm);
                const auto library = std::array{load_roots.Hold(Value::Ref(vm.NewString(name)))};
                vm.CallStatic("Ljava/lang/System;", "loadLibrary", "(Ljava/lang/String;)V", library);
            }
        }
        const auto directory = roots.Hold(Value::Ref(vm.NewString(store.path)));
        const auto key = roots.Hold(Value::Ref(vm.NewString("phase10a")));
        const auto initialize = std::array{directory};
        Require(vm.StringUtf8(vm.CallStatic(mmkv, "initialize",
            "(Ljava/lang/String;)Ljava/lang/String;", initialize).ref()) == store.path, "Initialize real MMKV");
        const auto open = [&] {
            return roots.Hold(vm.CallStatic(mmkv, "defaultMMKV", "()Lcom/tencent/mmkv/MMKV;").ref());
        };
        auto* kv = open();
        Require(kv && vm.CallVirtual(kv, "encode", "(Ljava/lang/String;I)Z",
                                      std::array{key, Value::Int(325)}).i(), "Write real MMKV value");
        vm.CallVirtual(kv, "sync", "()V");
        vm.CallVirtual(kv, "close", "()V");
        kv = open();
        Require(kv && vm.CallVirtual(kv, "decodeInt", "(Ljava/lang/String;I)I",
                                      std::array{key, Value::Int(-1)}).i() == 325, "Reopen persisted MMKV value");
        vm.CallVirtual(kv, "close", "()V");
        const auto stats = vm.GetGuestNativeStats();
        Require(stats.abi == "arm64-v8a" && stats.libraries >= 2, "Run MMKV and its libc++ dependency");
        std::cout << "{\"event\":\"isolated_mmkv\",\"reopened_value\":325,\"native_host_calls\":" << stats.calls
                  << ",\"native_callbacks\":" << stats.callbacks << "}\n";
        PrintNativeImages(vm);
    } catch (const wa::interp::ManagedException&) {
        PrintNativeImages(vm);
        auto error = vm.ExceptionSummary();
        for (const auto& frame : vm.ExceptionStackTrace()) error += '\n' + frame;
        vm.ClearException();
        throw std::runtime_error(error);
    }
}

} // namespace

int main(int argc, char** argv) try {
    std::cout << std::unitbuf;
    const bool require_ui = argc == 6 && std::string_view(argv[5]) == "--require-ui";
    const bool probe_mmkv = argc == 6 && std::string_view(argv[5]) == "--probe-mmkv";
    const bool probe_core = argc == 6 && std::string_view(argv[5]) == "--probe-core";
    Require(argc == 5 || require_ui || probe_mmkv || probe_core,
            "Expected framework DEX, fallback DEX, framework resources, WeChat APK and optional mode");
    if (!std::filesystem::exists(argv[4])) {
        Require(!require_ui, "The WeChat APK is required for a UI verification run");
        std::cout << "{\"event\":\"skip\",\"reason\":\"WeChat APK is absent\"}\n";
        return 0;
    }
    if (probe_mmkv || probe_core) {
        ProbeNativeLibraries(argv[1], argv[2], argv[4], probe_core);
        return 0;
    }
    hybrid_test::Foreground foreground(argv[1], argv[2], argv[3]);
    Check(wa::system::RunForegroundVmProbe([](Vm& vm) {
        vm.SetGuestLibraryObserver(PrintLibraryRequest);
    }), "Observe Java native library requests");

    auto status = foreground.Boot(argv[4]);
    std::string error = status < 0 ? hybrid_test::LastError() : "";
    FrameEvidence frame;
    FrameEvidence early_welcome;
    int ticks = 0;
    int welcome_frames = 0;
    int steady_welcome_frames = 0;
    const auto* capture_dir = std::getenv("WA_WECHAT_FRAME_DIR");
    const auto capture = [&](const char* name) {
        if (capture_dir) ReadFrame(std::filesystem::path(capture_dir) / name);
    };
    // Allow a delayed redirect and count steady frames after the control fade.
    for (int i = 0; status >= 0 && i <= 180; ++i) {
        const auto state = hybrid_test::State();
        if (state.contains("\"activity\":\"com.tencent.mm.plugin.account.ui.WelcomeActivity\"") &&
            Metric(state, "frames")) {
            auto next_frame = ReadFrame();
            if (next_frame.sequence != frame.sequence) {
                welcome_frames = next_frame.ShowsWelcome() ? welcome_frames + 1 : 0;
                steady_welcome_frames = next_frame.ShowsSettledWelcome() ? steady_welcome_frames + 1 : 0;
                frame = std::move(next_frame);
                if (welcome_frames == 3) {
                    early_welcome = frame;
                    PrintFrame(frame, welcome_frames);
                    capture("welcome-early.waf");
                }
            }
            if (steady_welcome_frames == kSteadyWelcomeFrames) break;
        } else {
            welcome_frames = 0;
            steady_welcome_frames = 0;
        }
        if (i == 180) break;
        status = wa_system_app_tick(++ticks * kFrameNanos);
        if (status < 0) error = hybrid_test::LastError();
    }
    const auto state = hybrid_test::State();
    std::cout << "{\"event\":\"boot\",\"phase\":" << JsonString(Phase(state))
              << ",\"status\":" << status << ",\"error\":" << JsonString(error)
              << ",\"state\":" << state << "}\n";
    Check(wa::system::RunForegroundVmProbe([](Vm& vm) {
        PrintStartupMethods(vm);
        PrintNativeImages(vm);
    }), "Describe startup methods and native images");
    const bool published = Metric(state, "frames") != 0;
    if (published) frame = ReadFrame();
    PrintFrame(frame, welcome_frames);

    Require(state.contains("\"package\":\"com.tencent.mm\"") &&
            state.contains("\"launchActivity\":\"com.tencent.mm.ui.LauncherUI\"") &&
            Metric(state, "dexFiles") == 18 && Metric(state, "dexClasses") == 255001,
            "WeChat manifest or DEX set did not match 8.0.80");
    Require(Metric(state, "attachBaseContextCalls") == 1 && Metric(state, "attachBaseContextReturns") == 1,
            "WeChat did not finish Application.attachBaseContext");
    Require(Metric(state, "providersCreated") == 28 && Metric(state, "applicationCreateCalls") == 1 &&
            Metric(state, "applicationCreateReturns") == 1,
            "WeChat did not initialize its main-process providers and finish Application.onCreate");
    Require(Metric(state, "nativeLoadReturns") >= 8 && Metric(state, "nativeLibraries") >= 95 &&
            Metric(state, "nativeHostCalls") > 0 &&
            Metric(state, "nativeFileFaults") > 0 && state.contains("\"nativeAbi\":\"arm64-v8a\""),
            "WeChat did not execute its ARM64 startup libraries in process");
    if (status >= 0 || published)
        Check(wa::system::RunForegroundVmProbe(ProbeInstalledProviders), "Check installed manifest providers");
    Check(wa::system::RunForegroundVmProbe([](Vm& vm) {
        Require(vm.GetLinkedDeclaredMethodStats(kAppLike, "<init>", kAppLikeConstructor).returned > 0 &&
                vm.GetLinkedDeclaredMethodStats(kTinker, "createInlineFence", kFence).returned > 0,
                "WeChat did not finish the ApplicationLike constructor and createInlineFence");
        Require(vm.GetLinkedDeclaredMethodStats(kAppLike, "onBaseContextAttached", "(Landroid/content/Context;)V")
                    .returned == 1, "WeChat did not finish MMApplicationLike.onBaseContextAttached");
        Require(vm.GetLinkedDeclaredMethodStats(kFirebaseProvider, "onCreate", "()Z").returned == 1,
                "FirebaseInitProvider did not finish onCreate");
        Require(vm.GetLinkedDeclaredMethodStats(kPackageManager, "installEmbeddedProviders",
                    "(Landroid/content/Context;)V").returned == 1,
                "PackageManager did not finish manifest provider installation");
        for (const auto provider : {kRemoteProviders[1], kRemoteProviders[2]})
            Require(vm.GetLinkedDeclaredMethodStats(provider, "<init>", "()V").invocations == 0,
                    "WeChat constructed a provider assigned to a named process");
        Require(vm.GetLinkedDeclaredMethodStats(kAppLike, "onCreate", "()V").returned == 1 &&
                vm.GetLinkedDeclaredMethodStats(kTinker, "onCreate", "()V").returned == 1,
                "WeChat did not finish both Application and ApplicationLike onCreate methods");
        Require(vm.GetLinkedDeclaredMethodStats(kPool, "<init>", kPoolConstructor).returned > 0,
                "WeChat did not finish constructing its ForkJoinPool");
        const auto pid = vm.CallStatic("Landroid/os/Process;", "myPid", "()I").i();
        const auto self = vm.ReadGuestFile("/proc/self/cmdline");
        const auto by_pid = vm.ReadGuestFile("/proc/" + std::to_string(pid) + "/cmdline");
        constexpr char process[] = "com.tencent.mm";
        const auto expected = std::span(reinterpret_cast<const uint8_t*>(process), sizeof(process));
        Require(self && by_pid && std::ranges::equal(*self, expected) && std::ranges::equal(*by_pid, expected),
                "The package identity and guest /proc cmdline entries disagree");
        const auto proc = [&](std::string_view path) {
            const auto bytes = vm.ReadGuestFile(path);
            Require(bytes.has_value(), "Missing guest file " + std::string(path));
            return std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        };
        Require(proc("/proc/self/status").starts_with("Name:\tcom.tencent.mm\n") &&
                proc("/proc/self/stat").starts_with("1 (com.tencent.mm) R ") &&
                proc("/proc/meminfo").starts_with("MemTotal:\t") &&
                proc("/proc/self/maps") == proc("/proc/1/maps"), "Guest process metadata is inconsistent");
        const auto images = vm.GetGuestNativeImages();
        const auto maps = proc("/proc/self/maps");
        const auto loaded = [&](std::string_view name, std::string_view on_load) {
            const auto entry = std::ranges::find(images, name, &wa::interp::GuestNativeImage::name);
            Require(entry != images.end() && entry->mapped_base != 0 && entry->error.empty() &&
                    (on_load.empty() || entry->jni_on_load == on_load) && maps.contains(name),
                    "Missing guest startup mapping or unexpected JNI_OnLoad state for " + std::string(name));
        };
        loaded("libc++_shared.so", "absent");
        loaded("libstlport_shared.so", "absent");
        loaded("libcxxstl.200.so", "absent");
        loaded("libmmkv.so", "returned");
        loaded("libwechatcrash.so", "returned");
        loaded("libwechatbacktrace.so", {});
        loaded("libtrace-canary.so", "returned");
        loaded("libcso.log.so", "returned");
        loaded(kMatrixLibrary, "returned");
        loaded("libcso.so", "returned");
        loaded("libWCDB.so", "returned");
        loaded("libwechatcommon.so", "returned");
        loaded("libmarscomm.so", "returned");
        loaded("libmarsquic.so", "absent");
        loaded("libcronet.119.0.6045.214.so", "returned");
        loaded("libwechatmm.so", "returned");
        loaded("libwechatnetwork.so", "returned");
        loaded("libwechatnormsg.so", "returned");
        loaded("libzidl2.so", "returned");
        loaded("libilink2.so", "returned");
        loaded("libilink_network.so", "returned");
        loaded("libunit_rc.so", "returned");
        loaded("libmmskia.so", "absent");
        loaded("libflutter.so", "returned");
        loaded("libmm_foundation_lib.so", "returned");
        loaded("libvoipComm.so", "absent");
        loaded("libvoipCodec.so", "absent");
        loaded("libxffmpeg.so", "absent");
        loaded("libopencv_world.so", "absent");
        loaded("libWeVision.so", "absent");
        loaded("libWeEffect.so", "absent");
        loaded("libmmv8.so", "absent");
        loaded("libmmnode.so", "absent");
        loaded(kV8Library, "returned");
        loaded("libmagicbrush-nano.so", "returned");
        loaded("libwxaudio.so", "returned");
        loaded("libwechatxlog.so", "returned");
        loaded("libaff_biz.so", "returned");
        loaded("libMMProtocalJni.so", "returned");
        loaded("libwechatpaybase.so", "returned");
        loaded("libwechatpaymm.so", "returned");
        loaded("libtsmsc.so", "returned");
        Require(vm.GetLinkedDeclaredMethodStats(kPayMarsContext, "OnJniCreateContext", "(Ljava/lang/String;)V")
                    .returned > 0 && vm.GetLinkedDeclaredMethodStats(kWallet, "init", kWalletInit).returned > 0,
                "PayMars context creation and wallet native initialization both returned");
        Require(vm.GetLinkedDeclaredMethodStats(kMatrix, "jniPlaceHolder", "()V").returned > 0 &&
                vm.GetLinkedDeclaredMethodStats(kMatrix, "initJniHookByPlaceHolderMethod",
                    "(Ljava/lang/reflect/Method;)I").returned > 0 &&
                vm.CallStatic(kMatrix, "f", "(ZILjava/lang/Object;)Z", std::array{Value{}, Value{}, Value{}}).i(),
                "Matrix resolved the placeholder's ART native entry point");
        ProbePackageMetadata(vm);
    }), "Check the WeChat startup checkpoint");
    Require(status >= 0, error);
    Require(Metric(state, "onCreateCalls") == 1 && Metric(state, "onStartCalls") == 1 &&
            Metric(state, "onResumeCalls") == 1, "WeChat did not finish the LauncherUI lifecycle");
    Require(ResumedActivity() == kWelcome, "WeChat did not reach the WelcomeActivity redirect");
    Require(steady_welcome_frames == kSteadyWelcomeFrames && frame.ShowsSettledWelcome(),
            "WeChat did not publish 65 full-contrast welcome frames with non-blank bitmap pixels");
    Require(early_welcome.ShowsWelcome(), "WeChat did not render its WelcomeActivity fade");
    const auto early_alpha = early_welcome.labels.at("Log In").color >> 24;
    Require(early_alpha < 255, "WelcomeActivity did not start with partially transparent controls");
    std::cout << "{\"event\":\"welcome_fade\",\"early_alpha\":" << early_alpha
              << ",\"settled_alpha\":" << (frame.labels.at("Log In").color >> 24)
              << ",\"steady_frames\":" << steady_welcome_frames << "}\n";
    Check(wa::system::RunForegroundVmProbe([](Vm& vm) {
        Require(vm.GetLinkedDeclaredMethodStats(kHttpClient, "<init>", "()V").returned > 0 &&
                vm.GetLinkedDeclaredMethodStats(kReport, "run", "()V").returned > 0,
                "The scheduled WeChat reporter did not construct DefaultHttpClient and return");
        std::cout << "{\"event\":\"reporter_returned\"}\n";
    }), "Verify the delayed WeChat report task");
    capture("welcome.waf");
    Tap(frame, "语言", ticks);
    Advance(ticks, 18, "Open WeChat's language selector");
    const auto language = ReadFrame();
    PrintFrame(language, 0);
    Require(ResumedActivity() == kLanguage && language.bitmap_pixels && language.labels.contains("English") &&
            language.labels.contains("简体中文"), "WeChat did not render the language options");
    capture("language.waf");
    Check(wa_system_app_send_back(), "Return from the language selector");
    Advance(ticks, 18, "Draw WelcomeActivity after language selector");
    frame = ReadFrame();
    Require(ResumedActivity() == kWelcome && frame.ShowsWelcome(),
            "WeChat did not return to WelcomeActivity after the language selector");
    Tap(frame, "Log In", ticks);
    Advance(ticks, 25, "Open WeChat's login activity");
    const auto login = ReadFrame();
    PrintFrame(login, 0);
    Require(ResumedActivity() == kLogin, "WeChat did not reach MobileInputUI from Log In");
    constexpr auto phone_hint = "Enter phone number";
    Require(login.sequence > frame.sequence && login.bitmap_pixels && login.labels.contains("Log In Via Mobile Number") &&
            login.labels.contains("Mobile Number") && login.labels.contains(phone_hint) &&
            login.labels.contains("Other Login Options"), "MobileInputUI did not draw the login form");
    capture("login.waf");
    VerifyPhoneInput(login, phone_hint, ticks);
    Check(wa_system_app_send_back(), "Return from WeChat's login activity");
    Advance(ticks, 18, "Draw WelcomeActivity after login");
    Require(ResumedActivity() == kWelcome && ReadFrame().ShowsWelcome(),
            "WeChat did not return to WelcomeActivity after login");
    return 0;
} catch (const std::exception& error) {
    std::cerr << "{\"event\":\"probe_error\",\"error\":" << JsonString(error.what()) << "}\n";
    return 1;
}
