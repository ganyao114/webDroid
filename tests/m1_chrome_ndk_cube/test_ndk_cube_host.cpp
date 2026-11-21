#include "system_entry.h"
#include "wasmandroid/hal_gfx.h"
#include "wasmandroid/libkern.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace {

constexpr int kSide = 256;
constexpr int kFrames = 140;
constexpr uint32_t kCapacity = 256;
constexpr uint32_t kGuestPcm = 0x20000000;
using Pcm = std::array<float, kFrames * WA_AUDIO_CHANNELS>;
const char* abi;

void Require(bool ok, const char* what) {
    if (ok)
        return;
    std::fprintf(stderr, "%s: %s failed\n", abi, what);
    std::exit(1);
}

void Expect(int32_t actual, int32_t expected, const char* what) {
    if (actual == expected)
        return;
    std::fprintf(stderr, "%s: %s returned %d, expected %d\n", abi, what, actual, expected);
    std::exit(1);
}

class Pbuffer {
  public:
    Pbuffer() {
        display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
        Require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr) &&
                    eglBindAPI(EGL_OPENGL_ES_API),
                "native surfaceless EGL");
        const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
        EGLConfig config;
        EGLint count;
        Require(eglChooseConfig(display, attrs, &config, 1, &count) && count == 1,
                "native GLES3 config");
        const EGLint surface_attrs[] = {EGL_WIDTH, kSide, EGL_HEIGHT, kSide, EGL_NONE};
        const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, surface_attrs);
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
        Require(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT &&
                    eglMakeCurrent(display, surface, surface, context),
                "native GLES3 pbuffer");
    }

    ~Pbuffer() {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
    }

  private:
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
};

struct BackingFile {
    uint32_t fd;
    size_t length;
    size_t reads = 0;
    size_t bytes = 0;

    static int64_t Read(void* user, uint32_t file_id, void* dst, size_t len, uint64_t offset) {
        auto& file = *static_cast<BackingFile*>(user);
        Require(file_id == file.fd && len && len <= WA_PAGE_SIZE && offset <= file.length &&
                    len <= file.length - offset,
                "page-sized read from NDK shared library");
        const auto result = wa_vfs_pread(file.fd, dst, len, offset);
        Require(result == static_cast<int64_t>(len), "NDK backing file read");
        ++file.reads;
        file.bytes += len;
        return result;
    }
};

void Run(wa_guest_arch_t arch, const char* path, const char* wasm_path) {
    abi = arch == WA_ARCH_ARM32 ? "armeabi-v7a" : "arm64-v8a";
    std::ifstream input(path, std::ios::binary);
    Require(input.is_open(), "open NDK shared library for ELF parsing");
    const std::vector<uint8_t> elf{std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>()};
    auto backing_file = std::unique_ptr<std::FILE, decltype(&std::fclose)>(
        std::fopen(path, "rb"), std::fclose);
    Require(backing_file && !elf.empty(), "open NDK shared library for demand paging");
    BackingFile backing{static_cast<uint32_t>(fileno(backing_file.get())), elf.size()};

    Pbuffer pbuffer;
    wa_system_guest_t* loaded = nullptr;
    Expect(wa_system_load(arch, backing.fd, elf.data(), elf.size(), BackingFile::Read, &backing,
                          &loaded),
           WA_OK, "load and resolve NDK shared library");
    const std::unique_ptr<wa_system_guest_t, decltype(&wa_system_destroy)> guest(loaded,
                                                                               wa_system_destroy);
    wa_softmmu_stats_t mmu{};
    wa_dbt_stats_t dbt{};
    Expect(wa_system_get_stats(guest.get(), &mmu, &dbt), WA_OK, "stats after ELF load");
    const auto faults_at_load = mmu.demand_file_faults;
    Require(faults_at_load && faults_at_load == backing.reads && backing.bytes < elf.size(),
            "lazy reads during relocation and ELF initializers");

    Expect(wa_system_demo_init(guest.get(), kSide, kSide), 0, "demo_init");
    const auto first = wa_system_demo_render_frame(guest.get(), 0.0f, 0.5f, 0.5f);
    const auto second = wa_system_demo_render_frame(guest.get(), 0.5f, 0.5f, 0.5f);
    if (first < 0 || second < 0) {
        std::fprintf(stderr, "%s: demo_render_frame returned %d, %d\n", abi, first, second);
        std::exit(1);
    }
    Require(first != second, "animated GLES3 frame checksums");

    alignas(wa_audio_ring_header_t) std::array<uint8_t, sizeof(wa_audio_ring_header_t) +
                                                          kCapacity * WA_AUDIO_CHANNELS * sizeof(float)>
        ring{};
    Expect(wa_system_bind_audio(ring.data(), ring.size(), kCapacity), WA_OK, "bind PCM ring");
    Expect(wa_system_map_memory(guest.get(), kGuestPcm, WA_PAGE_SIZE), WA_OK, "map guest PCM");
    Expect(wa_system_demo_generate_audio(guest.get(), kGuestPcm, kFrames, 0.25f), kFrames,
           "demo_generate_audio");
    Pcm output;
    Pcm expected;
    Expect(wa_system_read_memory(guest.get(), kGuestPcm, expected.data(), sizeof(expected)), WA_OK,
           "read guest PCM");
    Expect(wa_audio_ring_read(ring.data(), output.data(), kFrames), kFrames, "drain PCM ring");
    Expect(wa_system_bind_audio(nullptr, 0, 0), WA_OK, "detach PCM ring");
    Require(std::memcmp(output.data(), expected.data(), sizeof(expected)) == 0,
            "bit-exact stereo PCM through the weak audio import");
    const float first_left = 0.20f * std::sin(0.25f) + 0.09f * std::sin(0.25f * 1.25992105f) +
                             0.06f * std::sin(0.25f * 1.49830708f);
    Require(std::abs(output[0] - first_left) < 1e-6f && output[0] != output[1],
            "audio phase argument and stereo samples");

    uint8_t* unit_bytes;
    size_t unit_size;
    Expect(wa_system_compile(guest.get(), "demo_render_frame", &unit_bytes, &unit_size), WA_OK,
           "compile demo_render_frame using SwiftVM");
    const std::unique_ptr<uint8_t, decltype(&wa_dbt_free_buffer)> unit(unit_bytes, wa_dbt_free_buffer);
    std::ofstream output_file(wasm_path, std::ios::binary);
    output_file.write(reinterpret_cast<const char*>(unit.get()), unit_size);
    output_file.close();
    Require(output_file.good(), "write standalone Wasm unit");
    Expect(wa_system_get_stats(guest.get(), &mmu, &dbt), WA_OK, "stats after guest execution");
    Require(mmu.demand_file_faults == backing.reads && mmu.demand_file_faults > faults_at_load &&
                mmu.cow_faults > 0 && mmu.guard_faults == 0,
            "execution demand pages and private relocations");
    Require(dbt.insns_executed && dbt.host_calls && dbt.wasm_units_compiled == 1 &&
                dbt.wasm_bytes_emitted == unit_size,
            "DBT execution and emission counters");
    std::printf("%s: 0 missing symbols, init 0, frame checksums %d/%d, %d bit-exact PCM frames, "
                "%llu -> %llu demand faults, %zu-byte Wasm unit\n",
                abi, first, second, kFrames, static_cast<unsigned long long>(faults_at_load),
                static_cast<unsigned long long>(mmu.demand_file_faults), unit_size);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "Usage: %s ARM32.so ARM64.so ARM32.wasm ARM64.wasm\n", argv[0]);
        return 2;
    }
    Run(WA_ARCH_ARM32, argv[1], argv[3]);
    Run(WA_ARCH_ARM64, argv[2], argv[4]);
}
