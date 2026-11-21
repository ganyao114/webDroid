#include "gles_host_bridge.h"
#include "wasmandroid/hal_gfx.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

struct wa_softmmu {
    uint64_t base;
    std::array<uint8_t, 2 * WA_PAGE_SIZE> backing{};

    uint8_t* region(uint64_t address, size_t len) {
        const uint64_t offset = address - base;
        return len <= backing.size() && offset <= backing.size() - len ? backing.data() + offset
                                                                       : nullptr;
    }
};

struct wa_dbt {
    struct Binding {
        std::string soname;
        wa_host_symbol_fn function;
        void* user;
    };
    std::map<std::string, Binding> bindings;
};

int32_t wa_dbt_register_symbol(wa_dbt_t* dbt, const char* soname, const char* symbol,
                               wa_host_symbol_fn function, void* user) {
    return dbt->bindings.try_emplace(symbol, wa_dbt::Binding{soname ? soname : "", function, user})
                   .second
               ? WA_OK
               : WA_ERR_EXIST;
}

int32_t wa_softmmu_read(wa_softmmu_t* mmu, uint64_t address, void* out, size_t len) {
    if (auto* buffer = mmu->region(address, len)) {
        std::memcpy(out, buffer, len);
        return WA_OK;
    }
    return WA_ERR_FAULT;
}

int32_t wa_softmmu_write(wa_softmmu_t* mmu, uint64_t address, const void* in, size_t len) {
    if (auto* buffer = mmu->region(address, len)) {
        std::memcpy(buffer, in, len);
        return WA_OK;
    }
    return WA_ERR_FAULT;
}

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

struct Guest {
    wa_dbt_t& dbt;
    wa_softmmu_t mmu;
    wa_guest_arch_t arch;

    template <class T> void store(uint64_t offset, const T& data) {
        check(wa_softmmu_write(&mmu, mmu.base + offset, &data, sizeof(data)) == WA_OK,
              "guest store");
    }

    template <class T> T load(uint64_t offset) {
        T data;
        check(wa_softmmu_read(&mmu, mmu.base + offset, &data, sizeof(data)) == WA_OK, "guest load");
        return data;
    }

    uint64_t invoke(const char* name, std::initializer_list<uint64_t> args, int32_t status = WA_OK,
                    bool with_stack = true, double real = 0) {
        constexpr uint64_t stack = 0x100;
        wa_host_call_ctx_t context{.dbt = &dbt, .mmu = &mmu, .arch = arch};
        context.ret1 = with_stack ? mmu.base + stack : 0;
        context.fargs[0] = real;
        size_t i = 0;
        const size_t registers = arch == WA_ARCH_ARM32 ? 4 : 8;
        for (const uint64_t arg : args) {
            if (i < registers) {
                context.args[i] = arg;
            } else if (arch == WA_ARCH_ARM32) {
                store(stack + (i - registers) * 4, static_cast<uint32_t>(arg));
            } else {
                store(stack + (i - registers) * 8, arg);
            }
            ++i;
        }
        const auto& binding = dbt.bindings.at(name);
        check(binding.function(&context, binding.user) == status, name);
        check(context.ret1 == 0 && !context.has_float_ret, "integer result ABI");
        return context.ret0;
    }
};

void exercise(Guest& guest) {
    const auto base = guest.mmu.base;
    const wa_gfx_surface_config_t config{256, 192, 24, 0};
    check(wa_gfx_init(nullptr, &config) == WA_OK, "native surface creation");
    const auto display = guest.invoke("eglGetDisplay", {0});
    check(display != 0 && guest.invoke("eglInitialize", {display, base + 4, base + 8}) == 1,
          "EGL display initialization");
    check(guest.load<int32_t>(4) == 1 && guest.load<int32_t>(8) == 5, "EGL version outputs");

    constexpr int32_t egl_none = 0x3038;
    constexpr int32_t config_attrs[] = {0x3025, 16, 0x3040, 0x0040, egl_none};
    guest.store(0x20, config_attrs);
    guest.store(0x40, UINT64_C(0xdeadbeefdeadbeef));
    const auto choose = {display, base + 0x20, base + 0x40, uint64_t{1}, base + 0x50};
    if (guest.arch == WA_ARCH_ARM32) {
        guest.invoke("eglChooseConfig", choose, WA_ERR_FAULT, false);
    }
    check(guest.invoke("eglChooseConfig", choose) == 1 && guest.load<int32_t>(0x50) == 1,
          "EGL config choice and stack arguments");
    const auto config_id = guest.load<uint32_t>(0x40);
    check(config_id != 0 &&
              guest.load<uint32_t>(0x44) == (guest.arch == WA_ARCH_ARM32 ? 0xdeadbeef : 0),
          "guest EGL handle width");

    constexpr int32_t context_attrs[] = {0x3098, 3, egl_none};
    guest.store(0x60, context_attrs);
    const auto surface = guest.invoke("eglCreateWindowSurface", {display, config_id, 1, 0});
    const auto context = guest.invoke("eglCreateContext", {display, config_id, 0, base + 0x60});
    check(surface && context &&
              guest.invoke("eglMakeCurrent", {display, surface, surface, context}) == 1,
          "EGL context setup through the bridge");
    check(wa_gfx_resize(64, 32) == WA_OK, "native surface resize");
    check(guest.invoke("eglQuerySurface", {display, surface, 0x3057, base + 0x70}) == 1 &&
              guest.load<int32_t>(0x70) == 64,
          "EGL resize query");
    check(guest.invoke("eglSwapBuffers", {display, surface}) == 1, "EGL swap");
    uint8_t pixel[4] = {255, 255, 255, 0};
    check(wa_gfx_read_pixels_rgba(63, 31, 1, 1, pixel) == WA_OK && pixel[0] == 0 && pixel[1] == 0 &&
              pixel[2] == 0 && pixel[3] == 255,
          "native opaque black surface");
    guest.invoke("glClear", {0}, WA_ERR_NOTSUP);

    alignas(wa_audio_ring_header_t) std::array<uint8_t, 64> ring{};
    constexpr float pcm[] = {0.25f, -0.5f, -0.75f, 0.125f};
    guest.store(0x200, pcm);
    check(wa_audio_ring_init(ring.data(), ring.size(), 4, WA_AUDIO_SAMPLE_RATE) == WA_OK &&
              wa_hal_set_audio_ring(ring.data()) == WA_OK,
          "attach audio ring");
    constexpr auto push = "wa_ndk_audio_push_stereo_f32";
    check(guest.invoke(push, {base + 0x200, 2}) == 2, "guest audio push");
    guest.invoke(push, {base + 2 * WA_PAGE_SIZE - 4, 2}, WA_ERR_FAULT);
    float output[4];
    check(wa_audio_ring_read(ring.data(), output, 2) == 2 && std::equal(pcm, pcm + 4, output),
          "PCM through the guest memory bridge");
    wa_hal_set_audio_ring(nullptr);

    guest.store(0x300, "BridgeTest");
    guest.store(2 * WA_PAGE_SIZE - 5, "text");
    guest.store(0x320, "Log %s %04x %.2f");
    const uint64_t text = base + 2 * WA_PAGE_SIZE - 5;
    const uint64_t real = std::bit_cast<uint64_t>(1.25);
    const auto print = "__android_log_print";
    if (guest.arch == WA_ARCH_ARM32) {
        check(guest.invoke(print, {4, base + 0x300, base + 0x320, text, 42, 0,
                                   static_cast<uint32_t>(real), real >> 32}) == 1,
              "ARM32 log varargs");
    } else {
        check(guest.invoke(print, {4, base + 0x300, base + 0x320, text, 42}, WA_OK, true, 1.25) ==
                  1,
              "ARM64 log varargs");
    }
    check(guest.invoke("__android_log_write", {4, base + 0x300, text}) == 1, "guest log write");
    guest.invoke("__android_log_write", {4, 0, base + 2 * WA_PAGE_SIZE}, WA_ERR_FAULT);
    check(guest.invoke("eglTerminate", {display}) == 1, "EGL teardown");
    wa_gfx_shutdown();
}

} // namespace

int main() {
    wa_dbt_t dbt;
    check(wa_hal_register_ndk_symbols(&dbt) == WA_OK, "register NDK symbols");
    check(dbt.bindings.at("eglGetDisplay").soname == "libEGL.so" &&
              dbt.bindings.at("glTexImage2D").soname == "libGLESv3.so" &&
              dbt.bindings.at("__android_log_print").soname == "liblog.so" &&
              dbt.bindings.at("wa_ndk_audio_push_stereo_f32").soname.empty(),
          "NDK library routing");
    Guest arm32{dbt, {0x10000}, WA_ARCH_ARM32};
    Guest arm64{dbt, {0x100000000}, WA_ARCH_ARM64};
    exercise(arm32);
    exercise(arm64);
    std::puts("NDK host bridge passed ARM32 and ARM64 SoftMMU checks");
}
