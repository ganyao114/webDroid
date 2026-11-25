#include "vlc_probe.h"

#include "../../runtime/android_system/foreground_test_access.h"
#include "../../runtime/android_system/guest_audio.h"
#include "jni_bridge.h"
#include <wasmandroid/hal_gfx.h>

#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace {

using wa::interp::JniBridge;
using wa::interp::Object;
using wa::interp::ScopedHandleScope;
using wa::interp::Value;
using wa::interp::Vm;
using wa::audio::Backend;

// The hosting VM owns these global references between playback probes.
thread_local std::array<jobject, 3> playback{};
thread_local std::string_view picked_path;

void Require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string CheckAbi(Vm& vm, Object* activity) {
    ScopedHandleScope roots(vm);
    constexpr auto build = "Landroid/os/Build;";
    constexpr auto str = "Ljava/lang/String;";
    constexpr auto string_array = "[Ljava/lang/String;";
    Require(vm.StringUtf8(vm.ReadStaticField(build, "CPU_ABI", str).ref()) == "arm64-v8a", "Build.CPU_ABI");
    const auto list = [&](const char* field) {
        const auto array = roots.Hold(vm.ReadStaticField(build, field, string_array));
        return vm.StringUtf8(vm.CallStatic("Ljava/util/Arrays;", "toString",
            "([Ljava/lang/Object;)Ljava/lang/String;", std::array{array}).ref());
    };
    Require(list("SUPPORTED_ABIS") == "[arm64-v8a, armeabi-v7a]", "Build.SUPPORTED_ABIS");
    Require(list("SUPPORTED_64_BIT_ABIS") == "[arm64-v8a]", "Build.SUPPORTED_64_BIT_ABIS");
    Require(list("SUPPORTED_32_BIT_ABIS") == "[armeabi-v7a]", "Build.SUPPORTED_32_BIT_ABIS");

    auto* info = roots.Hold(vm.CallVirtual(activity, "getApplicationInfo",
        "()Landroid/content/pm/ApplicationInfo;").ref());
    const auto dir = vm.StringUtf8(vm.ReadField(info, "nativeLibraryDir", str).ref());
    Require(dir == "/data/app/org.videolan.vlc/lib/arm64", "ApplicationInfo.nativeLibraryDir = " + dir);
    auto* path = roots.Hold(vm.NewString(dir + "/libvlcjni.so"));
    auto* file = roots.Hold(vm.NewObject("Ljava/io/RandomAccessFile;",
        "(Ljava/lang/String;Ljava/lang/String;)V", std::array{Value::Ref(path), Value::Ref(roots.Hold(vm.NewString("r")))}));
    for (const int byte : std::array<int, 6>{0x7f, 'E', 'L', 'F', 2, 1})
        Require(vm.CallVirtual(file, "read", "()I").i() == byte, "ELF64 magic and encoding");
    vm.CallVirtual(file, "seek", "(J)V", std::array{Value::Long(18)});
    Require(vm.CallVirtual(file, "read", "()I").i() == 183 &&
            vm.CallVirtual(file, "read", "()I").i() == 0, "ELF e_machine is AArch64");
    vm.CallVirtual(file, "close", "()V");

    constexpr auto util = "Lorg/videolan/libvlc/util/VLCUtil;";
    Require(vm.CallStatic(util, "hasCompatibleCPU", "(Landroid/content/Context;)Z",
        std::array{Value::Ref(activity)}).i(), "VLCUtil.hasCompatibleCPU");
    auto* specs = roots.Hold(vm.CallStatic(util, "getMachineSpecs",
        "()Lorg/videolan/libvlc/util/VLCUtil$MachineSpecs;").ref());
    Require(specs && vm.ReadField(specs, "is64bits", "Z").i() &&
        vm.ReadField(specs, "processors", "I").i() == 4 &&
        std::abs(vm.ReadField(specs, "frequency", "F").f() - 2400.0f) < 0.01f,
        "VLCUtil CPU topology and frequency");
    return dir + " ELF64, is64bits=true, 4 CPUs, frequency=" +
        std::to_string(vm.ReadField(specs, "frequency", "F").f());
}

std::string VisibleLabels(Vm& vm, Object* activity) {
    auto* env = vm.JniEnv();
    auto& jni = JniBridge::From(env);
    if (env->PushLocalFrame(8) != JNI_OK) throw std::bad_alloc();
    struct PopFrame {
        JNIEnv* env;
        ~PopFrame() { env->PopLocalFrame(nullptr); }
    } locals{env};
    const auto text_view = env->FindClass("android/widget/TextView");
    const auto view_group = env->FindClass("android/view/ViewGroup");
    if (!text_view || !view_group) throw wa::interp::ManagedException{};
    ScopedHandleScope roots(vm);
    auto* window = roots.Hold(vm.CallVirtual(activity, "getWindow", "()Landroid/view/Window;").ref());
    auto* decor = roots.Hold(vm.CallVirtual(window, "getDecorView", "()Landroid/view/View;").ref());
    auto* rect = roots.Hold(vm.NewObject("Landroid/graphics/Rect;"));
    std::string labels;
    const auto walk = [&](auto&& self, Object* view) -> void {
        if (!view || vm.CallVirtual(view, "getVisibility", "()I").i() != 0) return;
        ScopedHandleScope level(vm);
        level.Hold(view);
        const auto reference = jni.Local(view);
        const bool text = env->IsInstanceOf(reference, text_view);
        const bool group = env->IsInstanceOf(reference, view_group);
        env->DeleteLocalRef(reference);
        if (text && vm.CallVirtual(view, "getGlobalVisibleRect", "(Landroid/graphics/Rect;)Z",
            std::array{Value::Ref(rect)}).i()) {
            auto* sequence = level.Hold(vm.CallVirtual(view, "getText", "()Ljava/lang/CharSequence;").ref());
            if (sequence) {
                auto label = vm.StringUtf8(vm.CallVirtual(sequence, "toString", "()Ljava/lang/String;").ref());
                for (auto& ch : label) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
                if (!label.empty()) {
                    const auto mid = [&](const char* from, const char* to) {
                        return (vm.ReadField(rect, from, "I").i() + vm.ReadField(rect, to, "I").i()) / 2;
                    };
                    labels += std::to_string(mid("left", "right")) + '\t' +
                        std::to_string(mid("top", "bottom")) + '\t' + label + '\n';
                }
            }
        }
        if (group) {
            const auto count = vm.CallVirtual(view, "getChildCount", "()I").i();
            for (int i = 0; i < count; ++i) {
                auto* child = vm.CallVirtual(view, "getChildAt", "(I)Landroid/view/View;",
                    std::array{Value::Int(i)}).ref();
                self(self, child);
            }
        }
    };
    walk(walk, decor);
    return labels;
}

std::vector<uint8_t> Wave() {
    constexpr uint32_t rate = 44'100;
    constexpr uint32_t samples = rate / 4;
    constexpr uint32_t size = samples * sizeof(int16_t);
    std::vector<uint8_t> wave;
    wave.reserve(44 + size);
    const auto tag = [&](std::string_view text) { wave.insert(wave.end(), text.begin(), text.end()); };
    const auto little_endian = [&](uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i, value >>= 8) wave.push_back(static_cast<uint8_t>(value));
    };
    tag("RIFF");
    little_endian(36 + size, 4);
    tag("WAVEfmt ");
    little_endian(16, 4);
    little_endian(1, 2);
    little_endian(1, 2);
    little_endian(rate, 4);
    little_endian(rate * 2, 4);
    little_endian(2, 2);
    little_endian(16, 2);
    tag("data");
    little_endian(size, 4);
    for (uint32_t i = 0; i < samples; ++i) {
        const auto value = static_cast<int16_t>(8192 * std::sin(2 * std::numbers::pi * 440 * i / rate));
        little_endian(static_cast<uint16_t>(value), 2);
    }
    return wave;
}

std::string ProbeMedia(Vm& vm, Object* activity) {
    Require(!playback[0], "VLC playback is already active");
    ScopedHandleScope roots(vm);
    constexpr auto klass = "Lorg/videolan/libvlc/LibVLC;";
    vm.CallStatic(klass, "loadLibraries", "()V");
    Require(vm.GetGuestNativeStats().libraries >= 3, "LibVLC.loadLibraries");
    const auto version = vm.StringUtf8(vm.CallStatic(klass, "version", "()Ljava/lang/String;").ref());
    Require(version == "3.0.21 Vetinari" && vm.CallStatic(klass, "majorVersion", "()I").i() == 3,
        "Unexpected LibVLC version " + version);
    const auto changeset = vm.StringUtf8(vm.CallStatic(klass, "changeset", "()Ljava/lang/String;").ref());
    Require(!changeset.empty(), "LibVLC.changeset");
    std::printf("LibVLC version %s, changeset %s\n", version.c_str(), changeset.c_str());
    auto* options = roots.Hold(vm.NewObject("Ljava/util/ArrayList;"));
    vm.CallVirtual(options, "add", "(Ljava/lang/Object;)Z",
        std::array{Value::Ref(roots.Hold(vm.NewString("--aout=opensles")))});
    auto* instance = roots.Hold(vm.NewObject(klass, "(Landroid/content/Context;Ljava/util/List;)V",
        std::array{Value::Ref(activity), Value::Ref(options)}));
    Require(vm.CallVirtual(instance, "getInstance", "()J").j() != 0, "LibVLC.nativeNew");
    std::puts("LibVLC.nativeNew returned a native instance");

    std::string path(picked_path);
    if (path.empty()) {
        auto* files = roots.Hold(vm.CallVirtual(activity, "getFilesDir", "()Ljava/io/File;").ref());
        path = vm.StringUtf8(vm.CallVirtual(files, "getAbsolutePath", "()Ljava/lang/String;").ref()) + "/test_tone.wav";
        vm.MountReadOnlyFile(path, Wave());
    } else {
        Require(vm.GetLinkedDeclaredMethodStats("Landroid/app/Activity;", "embeddedDocumentResult",
            "(I[Ljava/lang/String;Ljava/lang/String;)V").returned > 0, "Android document result was not delivered");
    }
    auto* file = roots.Hold(vm.NewString(path));
    auto* media = roots.Hold(vm.NewObject("Lorg/videolan/libvlc/Media;",
        "(Lorg/videolan/libvlc/interfaces/ILibVLC;Ljava/lang/String;)V",
        std::array{Value::Ref(instance), Value::Ref(file)}));
    Require(vm.CallVirtual(media, "getInstance", "()J").j() != 0, "Media.nativeNewFromPath");
    const auto parsed = vm.CallVirtual(media, "parse", "()Z").i();
    const auto duration = vm.CallVirtual(media, "getDuration", "()J").j();
    const auto tracks = vm.CallVirtual(media, "getTrackCount", "()I").i();
    const auto state = vm.CallVirtual(media, "getState", "()I").i();
    auto result = version + " " + changeset + ", parsed=" + std::to_string(parsed) +
        ", durationMs=" + std::to_string(duration) + ", tracks=" + std::to_string(tracks) +
        ", state=" + std::to_string(state);
    Require(parsed && duration == 250 && tracks == 1, result);
    auto* track = roots.Hold(vm.CallVirtual(media, "getTrack", "(I)Lorg/videolan/libvlc/interfaces/IMedia$Track;",
        std::array{Value::Int(0)}).ref());
    const auto codec = vm.StringUtf8(vm.ReadField(track, "codec", "Ljava/lang/String;").ref());
    const auto rate = vm.ReadField(track, "rate", "I").i();
    const auto channels = vm.ReadField(track, "channels", "I").i();
    Require(codec == "PCM S16 LE" && rate == 44100 && channels == 1, "Unexpected PCM audio track");
    result += ", codec=" + codec + ", rate=" + std::to_string(rate) +
        ", channels=" + std::to_string(channels);

    auto* player = roots.Hold(vm.NewObject("Lorg/videolan/libvlc/MediaPlayer;",
        "(Lorg/videolan/libvlc/interfaces/ILibVLC;)V", std::array{Value::Ref(instance)}));
    Require(vm.CallVirtual(player, "getInstance", "()J").j() != 0, "MediaPlayer.nativeNewFromLibVlc");
    vm.CallVirtual(player, "setMedia", "(Lorg/videolan/libvlc/interfaces/IMedia;)V",
        std::array{Value::Ref(media)});
    Require(vm.CallVirtual(player, "setAudioOutput", "(Ljava/lang/String;)Z",
        std::array{Value::Ref(roots.Hold(vm.NewString("opensles")))}).i(), "Select VLC OpenSL ES output");
    result += ", playerState=" + std::to_string(vm.CallVirtual(player, "getPlayerState", "()I").i()) +
        ", audioOutput=" + vm.StringUtf8(vm.ReadField(player, "mAudioOutput", "Ljava/lang/String;").ref());
    wa::audio::ResetMetrics();
    vm.CallVirtual(player, "play", "()V");
    auto& jni = JniBridge::From(vm.JniEnv());
    playback = {jni.Global(player), jni.Global(media), jni.Global(instance)};
    return result;
}

std::string ProbeAudio(Vm&, Object*) {
    constexpr std::array backends{Backend::OpenSLES, Backend::AudioTrack};
    for (const auto backend : backends) {
        const auto metrics = wa::audio::ReadMetrics(backend);
        if (!metrics.frames) continue;
        const auto rms = std::sqrt(metrics.square_sum / (2 * metrics.frames));
        const auto hz = metrics.rising_edges > 1 ?
            double(metrics.rising_edges - 1) * WA_AUDIO_SAMPLE_RATE / (metrics.last_edge - metrics.first_edge) : 0;
        const auto sink = backend == Backend::OpenSLES ? "OpenSLES" : "AudioTrack";
        return std::string("{\"sink\":\"") + sink + "\",\"frames\":" + std::to_string(metrics.frames) +
            ",\"nonzeroFrames\":" + std::to_string(metrics.nonzero_frames) + ",\"rms\":" +
            std::to_string(rms) + ",\"peak\":" + std::to_string(metrics.peak) + ",\"frequency\":" +
            std::to_string(hz) + '}';
    }
    return "{\"frames\":0}";
}

std::string StopMedia(Vm& vm, Object*) {
    if (!playback[0]) return "idle";
    const auto references = std::exchange(playback, {});
    auto* env = vm.JniEnv();
    struct ReleaseReferences {
        JNIEnv* env;
        const std::array<jobject, 3>& refs;
        ~ReleaseReferences() { for (auto reference : refs) env->DeleteGlobalRef(reference); }
    } release{env, references};
    vm.CallVirtual(JniBridge::Read(references[0]), "stop", "()V");
    for (auto reference : references) vm.CallVirtual(JniBridge::Read(reference), "release", "()V");
    return "stopped";
}

std::string OpenToneDocument(Vm& vm, Object* activity) {
    ScopedHandleScope roots(vm);
    auto* action = roots.Hold(vm.NewString("android.intent.action.OPEN_DOCUMENT"));
    auto* intent = roots.Hold(vm.NewObject("Landroid/content/Intent;", "(Ljava/lang/String;)V",
        std::array{Value::Ref(action)}));
    auto* mime = roots.Hold(vm.NewString("audio/wav"));
    vm.CallVirtual(intent, "setType", "(Ljava/lang/String;)Landroid/content/Intent;", std::array{Value::Ref(mime)});
    vm.CallVirtual(activity, "startActivityForResult", "(Landroid/content/Intent;I)V",
        std::array{Value::Ref(intent), Value::Int(0x7212)});
    return "choosing WAV";
}

} // namespace

int32_t wa_test_vlc_abi(char* out, size_t cap) {
    return wa::system::RunForegroundProbe(CheckAbi, out, cap);
}

int32_t wa_test_vlc_labels(char* out, size_t cap) {
    return wa::system::RunForegroundProbe(VisibleLabels, out, cap);
}

int32_t wa_test_vlc_media(char* out, size_t cap) {
    return wa::system::RunForegroundProbe(ProbeMedia, out, cap);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int32_t wa_test_vlc_audio(char* out, size_t cap) {
    return wa::system::RunForegroundProbe(ProbeAudio, out, cap);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int32_t wa_test_vlc_stop(char* out, size_t cap) {
    return wa::system::RunForegroundProbe(StopMedia, out, cap);
}

#ifdef __EMSCRIPTEN__
extern "C" {
EMSCRIPTEN_KEEPALIVE int32_t wa_test_vlc_open_document(char* out, size_t cap) {
    return wa::system::RunForegroundProbe(OpenToneDocument, out, cap);
}

EMSCRIPTEN_KEEPALIVE int32_t wa_test_vlc_picked_media(const char* path, char* out, size_t cap) {
    if (!path || !std::string_view(path).starts_with("/__wa_documents/")) return WA_ERR_INVALID;
    picked_path = path;
    const auto status = wa::system::RunForegroundProbe(ProbeMedia, out, cap);
    picked_path = {};
    return status;
}

EMSCRIPTEN_KEEPALIVE int32_t wa_test_vlc_wave(uint8_t* out, size_t cap) {
    const auto wave = Wave();
    if (cap < wave.size()) return WA_ERR_NOMEM;
    std::memcpy(out, wave.data(), wave.size());
    return static_cast<int32_t>(wave.size());
}
}
#endif
