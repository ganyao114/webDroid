#include "foreground_test.h"
#include "foreground_test_access.h"
#include "hybrid_native_runtime.h"
#include "jni_bridge.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>

namespace {

constexpr auto kTinker = "Lcom/tencent/tinker/loader/app/TinkerApplication;";
constexpr auto kAppLike = "Lcom/tencent/mm/app/MMApplicationLike;";
constexpr auto kFence = "(Landroid/app/Application;ILjava/lang/String;ZJJLandroid/content/Intent;)Landroid/os/Handler;";
constexpr auto kAppLikeConstructor = "(Landroid/app/Application;IZJJLandroid/content/Intent;)V";
constexpr int kWidth = 480, kHeight = 800;

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
    method("Lcom/tencent/mm/ui/LauncherUI;", "onCreate", "(Landroid/os/Bundle;)V");
}

bool PrintFrame() {
    auto* scene = wa_system_app_sf_ctx();
    const auto release = [scene](const wa_sf_frame_packet_t* frame) { wa_sf_frame_release(scene, frame); };
    const std::unique_ptr<const wa_sf_frame_packet_t, decltype(release)> frame(
        wa_sf_frame_acquire(scene), release);
    Require(frame != nullptr, "Missing SurfaceFlinger frame");
    size_t draws = 0;
    std::set<std::string> labels;
    const auto* layers = wa_sf_frame_layers(frame.get());
    for (uint32_t i = 0; i < frame->layer_count; ++i) {
        const auto& layer = layers[i];
        if (!(layer.desc.flags & WA_SF_VISIBLE) || layer.desc.alpha <= 0 ||
            layer.desc.surface_kind != WA_SF_SURFACE_DISPLAY_LIST) continue;
        const auto* list = wa_sf_frame_list(frame.get(), &layer);
        Require(list || !layer.list_size, "Missing display list");
        for (size_t offset = 0; offset < layer.list_size;) {
            wa_canvas_record_t record{};
            const auto remaining = layer.list_size - offset;
            Require(remaining >= sizeof(record), "Truncated display list record");
            std::memcpy(&record, list + offset, sizeof(record));
            Require(record.byte_size >= sizeof(record) && record.byte_size <= remaining,
                    "Invalid display list record size");
            if (record.opcode >= WA_CANVAS_DRAW_COLOR && record.opcode <= WA_CANVAS_DRAW_TEXT) ++draws;
            if (record.opcode == WA_CANVAS_DRAW_TEXT) {
                wa_canvas_text_t text{};
                Require(record.byte_size >= sizeof(record) + sizeof(text), "Truncated draw text");
                std::memcpy(&text, list + offset + sizeof(record), sizeof(text));
                Require(text.utf8_offset >= sizeof(record) + sizeof(text) &&
                        text.utf8_offset <= record.byte_size &&
                        text.utf8_len <= record.byte_size - text.utf8_offset,
                        "Invalid display list text bounds");
                if (text.utf8_len && (text.argb >> 24))
                    labels.emplace(reinterpret_cast<const char*>(list + offset + text.utf8_offset),
                                   text.utf8_len);
            }
            offset += record.byte_size;
        }
    }
    std::cout << "{\"event\":\"surface_frame\",\"sequence\":" << frame->sequence
              << ",\"draw_commands\":" << draws << ",\"drawn_text\":[";
    for (auto label = labels.begin(); label != labels.end(); ++label) {
        if (label != labels.begin()) std::cout << ',';
        std::cout << JsonString(*label);
    }
    std::cout << "]}\n";
    return draws && !labels.empty();
}

std::string RasterizeDecor(Vm& vm, Object* activity) {
    ScopedHandleScope roots(vm);
    auto* window = roots.Hold(vm.CallVirtual(activity, "getWindow", "()Landroid/view/Window;").ref());
    auto* decor = roots.Hold(vm.CallVirtual(window, "getDecorView", "()Landroid/view/View;").ref());
    const auto width = vm.CallVirtual(decor, "getWidth", "()I").i();
    const auto height = vm.CallVirtual(decor, "getHeight", "()I").i();
    Require(width > 0 && width <= kWidth && height > 0 && height <= kHeight, "Invalid decor bitmap size");
    constexpr auto config = "Landroid/graphics/Bitmap$Config;";
    const std::array create{Value::Int(width), Value::Int(height),
                            roots.Hold(vm.ReadStaticField(config, "ARGB_8888", config))};
    auto* bitmap = roots.Hold(vm.CallStatic("Landroid/graphics/Bitmap;", "createBitmap",
        "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;", create).ref());
    const std::array bitmap_arg{Value::Ref(bitmap)};
    auto* canvas = roots.Hold(vm.NewObject("Landroid/graphics/Canvas;", "(Landroid/graphics/Bitmap;)V", bitmap_arg));
    const std::array canvas_arg{Value::Ref(canvas)};
    vm.CallVirtual(decor, "draw", "(Landroid/graphics/Canvas;)V", canvas_arg);

    auto* env = vm.JniEnv();
    auto pixels = env->NewIntArray(width * height);
    const std::array read{Value::Ref(wa::interp::JniBridge::Read(pixels)), Value::Int(0),
                          Value::Int(width), Value::Int(0), Value::Int(0), Value::Int(width), Value::Int(height)};
    vm.CallVirtual(bitmap, "getPixels", "([IIIIIII)V", read);
    std::vector<jint> colors(width * height);
    env->GetIntArrayRegion(pixels, 0, static_cast<jsize>(colors.size()), colors.data());
    env->DeleteLocalRef(pixels);
    const bool non_blank = std::ranges::any_of(colors, [&](jint color) {
        return (static_cast<uint32_t>(color) >> 24) && color != colors.front();
    });
    return non_blank ? "true" : "false";
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
    if (status == 0) {
        for (int i = 1; i <= 12; ++i) {
            status = wa_system_app_tick(i * 16'666'667LL);
            if (status >= 0) continue;
            error = hybrid_test::LastError();
            break;
        }
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
    if (!published)
        std::cout << "{\"event\":\"surface_frame\",\"draw_commands\":0,\"drawn_text\":[],\"non_blank_pixels\":null}\n";
    const bool has_drawn_text = published && PrintFrame();

    Require(state.contains("\"package\":\"com.tencent.mm\"") &&
            state.contains("\"launchActivity\":\"com.tencent.mm.ui.LauncherUI\"") &&
            Metric(state, "dexFiles") == 18 && Metric(state, "dexClasses") == 255001,
            "WeChat manifest or DEX set did not match 8.0.80");
    Require(Metric(state, "attachBaseContextCalls") > 0, "WeChat did not enter Application.attachBaseContext");
    Check(wa::system::RunForegroundVmProbe([](Vm& vm) {
        Require(vm.GetLinkedDeclaredMethodStats(kAppLike, "<init>", kAppLikeConstructor).returned > 0 &&
                vm.GetLinkedDeclaredMethodStats(kTinker, "createInlineFence", kFence).returned > 0,
                "WeChat did not finish the ApplicationLike constructor and createInlineFence");
        const auto pid = vm.CallStatic("Landroid/os/Process;", "myPid", "()I").i();
        const auto self = vm.ReadGuestFile("/proc/self/cmdline");
        const auto by_pid = vm.ReadGuestFile("/proc/" + std::to_string(pid) + "/cmdline");
        constexpr char process[] = "com.tencent.mm";
        const auto expected = std::span(reinterpret_cast<const uint8_t*>(process), sizeof(process));
        Require(self && by_pid && std::ranges::equal(*self, expected) && std::ranges::equal(*by_pid, expected),
                "The package identity and guest /proc cmdline entries disagree");
    }), "Check the Tinker startup checkpoint");
    if (status < 0) {
        Require(!error.empty(), "WeChat stopped without a diagnostic");
        Require(!require_ui, error);
        return 0;
    }
    Require(Metric(state, "applicationCreateReturns") > 0 && Metric(state, "onResumeCalls") > 0,
            "WeChat did not complete Application.onCreate and resume an activity");
    std::array<char, 16> pixels{};
    Check(wa::system::RunForegroundProbe(RasterizeDecor, pixels.data(), pixels.size()),
          "Rasterize foreground decor to an Android bitmap");
    std::cout << "{\"event\":\"decor_bitmap\",\"non_blank_pixels\":" << pixels.data() << "}\n";
    Require(has_drawn_text && std::string_view(pixels.data()) == "true",
            "WeChat did not draw visible text and non-blank pixels");
    return 0;
} catch (const std::exception& error) {
    std::cerr << "{\"event\":\"probe_error\",\"error\":" << JsonString(error.what()) << "}\n";
    return 1;
}
