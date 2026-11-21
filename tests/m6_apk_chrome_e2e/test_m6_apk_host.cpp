#include "apk_loader.h"
#include "wasmandroid/hal_gfx.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <androidfw/ZipFileRO.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

namespace {

constexpr uint32_t kPcm = 0x20000000;
constexpr uint32_t kCapacity = 256;
using Guest = std::unique_ptr<wa_system_guest_t, decltype(&wa_system_destroy)>;
using Apk = std::unique_ptr<wa_system_apk_t, decltype(&wa_system_close_apk)>;

void Require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class Pbuffer {
  public:
    Pbuffer(int width, int height) {
        display_ = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
        Require(display_ != EGL_NO_DISPLAY && eglInitialize(display_, nullptr, nullptr) &&
                    eglBindAPI(EGL_OPENGL_ES_API), "Initialize native Mesa EGL");
        const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
            EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
        EGLConfig config;
        EGLint count;
        Require(eglChooseConfig(display_, attributes, &config, 1, &count) && count == 1,
                "Choose native GLES3 config");
        const EGLint surface_attributes[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
        const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        surface_ = eglCreatePbufferSurface(display_, config, surface_attributes);
        context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, context_attributes);
        Require(surface_ != EGL_NO_SURFACE && context_ != EGL_NO_CONTEXT &&
                    eglMakeCurrent(display_, surface_, surface_, context_), "Create native pbuffer");
    }

    ~Pbuffer() {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display_, context_);
        eglDestroySurface(display_, surface_);
        eglTerminate(display_);
    }

  private:
    EGLDisplay display_;
    EGLSurface surface_;
    EGLContext context_;
};

struct JavaConfig {
    int width;
    int height;
    int audio_frames;
    std::array<float, 10> times;
};

Apk OpenApk(const char* path) {
    std::ifstream input(path, std::ios::binary);
    Require(input.is_open(), "Read wa_cube_app.apk");
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),
                                    std::istreambuf_iterator<char>()};
    return Apk(wa_system_open_apk(bytes.data(), bytes.size()), wa_system_close_apk);
}

void VerifyNativeEntry(const android::ZipFileRO& zip, const char* path, size_t expected_size) {
    const auto entry = zip.findEntryByName(path);
    Require(entry != nullptr, "Read native APK entry with ZipFileRO");
    uint16_t method;
    uint32_t size;
    off64_t offset;
    const bool found = zip.getEntryInfo(entry, &method, &size, nullptr, &offset,
                                        nullptr, nullptr, nullptr);
    zip.releaseEntry(entry);
    Require(found && method == android::ZipFileRO::kCompressStored && size == expected_size &&
                offset > 0 && offset % (16 * 1024) == 0, "Check the stored ELF length and 16 KiB alignment");
}

JavaConfig RunJava(wa_system_apk_t* apk, char* node, char* script,
                   const std::filesystem::path& output) {
    const uint8_t* wasm = nullptr;
    size_t size = 0;
    Require(wa_system_apk_compile_dex(apk, &wasm, &size) == WA_OK && wasm && size,
            "Verify APK DEX and emit Wasm with ART HGraph");
    const auto* stats = wa_system_apk_dex_stats(apk);
    Require(stats && stats->methods == 4 && stats->instructions_before > stats->instructions_after_folding &&
                stats->instructions_after_folding > stats->instructions_after_dce &&
                stats->blocks_before > stats->blocks_after_dce, "Observe ART constant folding and DCE");
    std::cout << "ART instructions " << stats->instructions_before << " -> "
              << stats->instructions_after_folding << " -> " << stats->instructions_after_dce
              << ", blocks " << stats->blocks_before << " -> " << stats->blocks_after_dce << '\n';
    auto wasm_file = (output / "cube-java.wasm").string();
    auto config_file = (output / "java-config.txt").string();
    std::ofstream module(wasm_file, std::ios::binary | std::ios::trunc);
    module.write(reinterpret_cast<const char*>(wasm), size);
    module.close();
    Require(module.good(), "Write compiled Java Wasm");

    char mode[] = "--host-java";
    char* const args[]{node, script, mode, wasm_file.data(), config_file.data(), nullptr};
    pid_t child;
    Require(posix_spawn(&child, node, nullptr, nullptr, args, environ) == 0, "Start Node");
    int status;
    Require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "Validate and execute the compiled Java Wasm in Node");
    JavaConfig config{};
    std::ifstream values(config_file);
    values >> config.width >> config.height >> config.audio_frames;
    for (auto& time : config.times)
        values >> time;
    Require(static_cast<bool>(values), "Read the Java return values from Node");
    return config;
}

void Render(const JavaConfig& config, Guest incoming, const char* abi) {
    const Pbuffer pbuffer(config.width, config.height);
    const auto guest = std::move(incoming);
    wa_softmmu_stats_t mmu{};
    wa_dbt_stats_t dbt{};
    Require(wa_system_get_stats(guest.get(), &mmu, &dbt) == WA_OK, "Read ELF load stats");
    const auto initial_faults = mmu.demand_file_faults;
    Require(initial_faults > 0, "Demand-page the ELF while applying relocations");
    Require(wa_system_demo_init(guest.get(), config.width, config.height) == WA_OK, "Initialize the cube");
    std::set<int32_t> checksums;
    for (const auto time : config.times) {
        const auto checksum = wa_system_demo_render_frame(guest.get(), time, 0.5f, 0.5f);
        Require(checksum >= 0, "Render a GLES3 frame with SwiftVM");
        checksums.insert(checksum);
    }
    Require(checksums.size() > 1 && glGetError() == GL_NO_ERROR, "Render ten animated GLES3 frames");

    alignas(wa_audio_ring_header_t) std::array<uint8_t, sizeof(wa_audio_ring_header_t) +
        kCapacity * WA_AUDIO_CHANNELS * sizeof(float)> ring{};
    Require(config.audio_frames > 0 && config.audio_frames <= kCapacity, "Fit Java PCM count in ring");
    Require(wa_system_map_memory(guest.get(), kPcm, WA_PAGE_SIZE) == WA_OK &&
                wa_system_bind_audio(ring.data(), ring.size(), kCapacity) == WA_OK, "Bind native PCM ring");
    const auto frames = wa_system_demo_generate_audio(guest.get(), kPcm, config.audio_frames, 0.25f);
    std::vector<float> expected(config.audio_frames * WA_AUDIO_CHANNELS), output(expected.size());
    const auto bytes = expected.size() * sizeof(float);
    const auto read = wa_system_read_memory(guest.get(), kPcm, expected.data(), bytes);
    const auto drained = wa_audio_ring_read(ring.data(), output.data(), config.audio_frames);
    Require(wa_system_bind_audio(nullptr, 0, 0) == WA_OK, "Detach native PCM ring");
    Require(frames == config.audio_frames && drained == frames && read == WA_OK &&
                std::memcmp(output.data(), expected.data(), bytes) == 0, "Read bit-exact stereo PCM");
    const float first_left = 0.20f * std::sin(0.25f) + 0.09f * std::sin(0.25f * 1.25992105f) +
                             0.06f * std::sin(0.25f * 1.49830708f);
    Require(std::abs(output[0] - first_left) < 1e-6f && output[0] != output[1], "Verify the stereo phase");
    Require(wa_system_get_stats(guest.get(), &mmu, &dbt) == WA_OK &&
                dbt.insns_executed > 0 && dbt.host_calls > 0 && mmu.demand_file_faults > initial_faults &&
                mmu.cow_faults > 0 && mmu.guard_faults == 0, "Check SwiftVM and lazy SoftMMU stats");
    std::cout << abi << " rendered " << config.times.size() << " frames and " << frames
              << " bit-exact stereo PCM frames, with " << initial_faults << " -> "
              << mmu.demand_file_faults << " demand faults\n";
}

void Run(char* argv[]) {
    auto apk = OpenApk(argv[1]);
    const auto* info = wa_system_apk_info(apk.get());
    Require(info != nullptr, "Open APK with AOSP 17 ApkAssets");
    std::cout << info->package_name << ' ' << info->version_name << " (" << info->version_code
              << ") " << info->launcher_activity << " · " << info->label_en_us << " / "
              << info->label_zh_cn << " · " << info->native_library << " ("
              << info->arm32_library_size << '/' << info->arm64_library_size << " bytes)\n";
    Require(std::string_view(info->package_name) == "org.wasmandroid.cube" &&
                info->version_code == 17 && std::string_view(info->version_name) == "1.7-wasm" &&
                std::string_view(info->launcher_activity) == ".CubeActivity" &&
                std::string_view(info->native_library) == "ndk_gles3_cube" &&
                info->arm32_library_size && info->arm64_library_size, "Read the binary APK manifest");
    Require(std::string_view(info->label_en_us) == "WasmAndroid 3D Cube" &&
                std::string_view(info->label_zh_cn) == "WasmAndroid 3D 立方体", "Resolve APK locales");
    const auto config = RunJava(apk.get(), argv[2], argv[3], argv[4]);

    const std::unique_ptr<std::FILE, decltype(&std::fclose)> file(std::fopen(argv[1], "rb"), std::fclose);
    Require(static_cast<bool>(file), "Open the APK VFS backing file");
    const auto fd = fileno(file.get());
    const std::unique_ptr<android::ZipFileRO> zip(android::ZipFileRO::openFd(fd, argv[1], false));
    Require(static_cast<bool>(zip), "Open the aligned APK with ZipFileRO");
    VerifyNativeEntry(*zip, "lib/armeabi-v7a/libndk_gles3_cube.so", info->arm32_library_size);
    VerifyNativeEntry(*zip, "lib/arm64-v8a/libndk_gles3_cube.so", info->arm64_library_size);
    Require(wa_system_apk_set_backing_file(apk.get(), fd) == WA_OK, "Set the APK VFS backing file");
    Guest arm32(wa_system_apk_load_native(apk.get(), "armeabi-v7a"), wa_system_destroy);
    Require(wa_system_apk_set_backing_file(apk.get(), -1) == WA_OK, "Select in-memory backing");
    Guest arm64(wa_system_apk_load_native(apk.get(), "arm64-v8a"), wa_system_destroy);
    Require(arm32 && arm64, "Load both embedded NDK libraries");
    apk.reset(); // The guests retain their demand-paged backing after the APK handle closes.
    Render(config, std::move(arm32), "armeabi-v7a");
    Render(config, std::move(arm64), "arm64-v8a");
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cerr << "Expected APK, Node, verifier script and output directory\n";
        return 2;
    }
    try {
        Run(argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
