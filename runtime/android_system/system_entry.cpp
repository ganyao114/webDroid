#include "system_entry.h"

#include "gles_host_bridge.h"
#include "wasmandroid/hal_gfx.h"
#include "wasmandroid/libkern.h"

#include <bit>
#include <memory>
#include <new>
#include <span>

#ifdef __EMSCRIPTEN__
#include <emscripten/em_js.h>

EM_JS(int64_t, wa_browser_vfs_read, (int32_t fd, void* out, size_t size, int64_t offset), {
    return Module['vfsRead'](fd, out, size, offset);
});

// Supply the same libkern page-read import through the modularized Emscripten loader.
extern "C" int64_t host_read(int32_t fd, void* out, size_t size, int64_t offset) {
    return wa_browser_vfs_read(fd, out, size, offset);
}
#endif

struct wa_system_guest {
    std::unique_ptr<wa_softmmu_t, decltype(&wa_softmmu_destroy)> mmu{nullptr, wa_softmmu_destroy};
    std::unique_ptr<wa_dbt_t, decltype(&wa_dbt_destroy)> dbt{nullptr, wa_dbt_destroy};
    bool owns_surface = false;
};

namespace {

constexpr uint64_t kLoadBias = 0x10000000;

int64_t ReadBackingFile(void*, uint32_t file_id, void* out, size_t bytes, uint64_t offset) {
    if (file_id > INT32_MAX || offset > INT64_MAX)
        return WA_ERR_INVALID;
    return wa_vfs_pread(static_cast<int32_t>(file_id), out, bytes, static_cast<int64_t>(offset));
}

int32_t Call(wa_system_guest_t& guest, const char* symbol, std::span<const uint64_t> ints,
             std::span<const float> floats = {}) {
    uint64_t entry;
    auto status = wa_dbt_find_symbol(guest.dbt.get(), symbol, &entry);
    if (status != WA_OK)
        return status;
    uint64_t result = 0;
    status = wa_dbt_call_f32(guest.dbt.get(), entry, ints.data(), ints.size(), floats.data(),
                             floats.size(), &result);
    return status == WA_OK ? std::bit_cast<int32_t>(static_cast<uint32_t>(result)) : status;
}

void ShutdownSurface(wa_system_guest_t& guest) {
    if (guest.owns_surface) {
        Call(guest, "demo_shutdown", {});
        wa_gfx_shutdown();
        guest.owns_surface = false;
    }
}

} // namespace

int32_t wa_system_load(wa_guest_arch_t arch, uint32_t file_id, const uint8_t* elf, size_t elf_size,
                       wa_page_read_fn page_reader, void* reader_user, wa_system_guest_t** out) {
    if (!out)
        return WA_ERR_INVALID;
    *out = nullptr;
    if (!elf || !elf_size || (arch != WA_ARCH_ARM32 && arch != WA_ARCH_ARM64))
        return WA_ERR_INVALID;
    auto guest = std::unique_ptr<wa_system_guest_t>(new (std::nothrow) wa_system_guest_t);
    if (!guest)
        return WA_ERR_NOMEM;
    guest->mmu.reset(wa_softmmu_create(page_reader ? page_reader : ReadBackingFile, reader_user));
    if (!guest->mmu)
        return WA_ERR_NOMEM;
    guest->dbt.reset(wa_dbt_create(arch, guest->mmu.get()));
    if (!guest->dbt)
        return WA_ERR_NOMEM;
    auto* dbt = guest->dbt.get();
    auto status = wa_hal_register_ndk_symbols(dbt);
    uint64_t base;
    if (status == WA_OK)
        status = wa_dbt_load_elf(dbt, file_id, elf, elf_size, kLoadBias, &base);
    if (status != WA_OK)
        return status;
    *out = guest.release();
    return WA_OK;
}

void wa_system_destroy(wa_system_guest_t* guest) {
    if (guest) {
        ShutdownSurface(*guest);
        delete guest;
    }
}

int32_t wa_system_demo_init(wa_system_guest_t* guest, uint32_t width, uint32_t height) {
    if (!guest)
        return WA_ERR_INVALID;
    const wa_gfx_surface_config_t config{width, height, 24, 0};
    const auto status = wa_gfx_init("#canvas", &config);
    if (status != WA_OK)
        return status;
    guest->owns_surface = true;
    const uint64_t args[] = {width, height};
    const auto result = Call(*guest, "demo_init", args);
    if (result != 0)
        ShutdownSurface(*guest);
    return result;
}

int32_t wa_system_demo_render_frame(wa_system_guest_t* guest, float time, float x, float y) {
    if (!guest || !guest->owns_surface)
        return WA_ERR_INVALID;
    const float args[] = {time, x, y};
    return Call(*guest, "demo_render_frame", {}, args);
}

int32_t wa_system_demo_generate_audio(wa_system_guest_t* guest, uint32_t address, int32_t frames,
                                      float phase) {
    if (!guest || frames < 0)
        return WA_ERR_INVALID;
    const uint64_t args[] = {address, static_cast<uint32_t>(frames)};
    return Call(*guest, "demo_generate_audio", args, {&phase, 1});
}

int32_t wa_system_bind_audio(void* ring, size_t size, uint32_t capacity) {
    if (!ring)
        return wa_hal_set_audio_ring(nullptr);
    const auto status = wa_audio_ring_init(ring, size, capacity, WA_AUDIO_SAMPLE_RATE);
    return status == WA_OK ? wa_hal_set_audio_ring(ring) : status;
}

int32_t wa_system_map_memory(wa_system_guest_t* guest, uint32_t address, size_t size) {
    return guest ? wa_softmmu_map_anon(guest->mmu.get(), address, size, WA_PROT_READ | WA_PROT_WRITE)
                 : WA_ERR_INVALID;
}

int32_t wa_system_read_memory(wa_system_guest_t* guest, uint32_t address, void* out, size_t size) {
    return guest ? wa_softmmu_read(guest->mmu.get(), address, out, size) : WA_ERR_INVALID;
}

int32_t wa_system_read_pixels(uint32_t width, uint32_t height, uint8_t* out_rgba) {
    return wa_gfx_read_pixels_rgba(0, 0, width, height, out_rgba);
}

int32_t wa_system_compile(wa_system_guest_t* guest, const char* symbol, uint8_t** out,
                          size_t* out_size) {
    if (!guest || !out || !out_size)
        return WA_ERR_INVALID;
    *out = nullptr;
    *out_size = 0;
    uint64_t entry;
    const auto status = wa_dbt_find_symbol(guest->dbt.get(), symbol, &entry);
    return status == WA_OK ? wa_dbt_compile_wasm_unit(guest->dbt.get(), entry, out, out_size)
                           : status;
}

int32_t wa_system_get_stats(const wa_system_guest_t* guest, wa_softmmu_stats_t* mmu,
                            wa_dbt_stats_t* dbt) {
    if (!guest || !mmu || !dbt)
        return WA_ERR_INVALID;
    wa_softmmu_get_stats(guest->mmu.get(), mmu);
    wa_dbt_get_stats(guest->dbt.get(), dbt);
    return WA_OK;
}
