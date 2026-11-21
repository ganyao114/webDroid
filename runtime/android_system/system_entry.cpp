#include "system_entry.h"

#include "../linker_ffi/dbt_internal.h"
#include "../softmmu_opfs/softmmu_internal.h"
#include "gles_host_bridge.h"
#include "runtime/arch/arm32/numeric.h"
#include "runtime/arch/arm32/numeric_registry.h"
#include "runtime/arch/arm32/state.h"
#include "runtime/arch/arm64/numeric.h"
#include "runtime/arch/arm64/state.h"
#include "runtime/backend/wasm/abi.h"
#include "runtime/exit.h"
#include "runtime/ir/opcode.h"
#include "wasmandroid/hal_gfx.h"
#include "wasmandroid/libkern.h"

#include <algorithm>
#include <bit>
#include <cstring>
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

namespace {

namespace rt = swift::runtime;
using Arm32State = rt::arm32::GuestState;
using Arm64State = rt::arm64::GuestState;
using rt::backend::wasm::WasmRunContext;

} // namespace

struct wa_system_jit_context {
    WasmRunContext run{};
    union {
        Arm32State arm32;
        Arm64State arm64;
    };
    wa_system_guest_t* owner;
    int32_t access_status = WA_OK;
    bool active = false;

    explicit wa_system_jit_context(wa_system_guest_t& guest) : arm64{}, owner(&guest) {}
};

static_assert(offsetof(wa_system_jit_context_t, run) == 0);
static_assert(offsetof(wa_system_jit_context_t, arm32) == sizeof(WasmRunContext));
static_assert(offsetof(wa_system_jit_context_t, arm64) == sizeof(WasmRunContext));
#ifdef __EMSCRIPTEN__
static_assert(sizeof(wa_system_jit_step_t) == 16 && offsetof(wa_system_jit_step_t, context) == 8 &&
              offsetof(wa_system_jit_step_t, checksum) == 12);
#endif

struct wa_system_guest {
    std::unique_ptr<wa_softmmu_t, decltype(&wa_softmmu_destroy)> mmu{nullptr, wa_softmmu_destroy};
    std::unique_ptr<wa_dbt_t, decltype(&wa_dbt_destroy)> dbt{nullptr, wa_dbt_destroy};
    std::unique_ptr<wa_system_jit_context_t> jit;
    std::unique_ptr<uint32_t[]> jit_misses;
    uint64_t render_entry = 0;
    bool owns_surface = false;
};

namespace {

constexpr uint64_t kLoadBias = 0x10000000;
constexpr size_t kIotlbEntries = wa::softmmu::kAddressLimit / WA_PAGE_SIZE;

bool JitActive(const wa_system_guest_t& guest) {
    return guest.jit && guest.jit->active;
}

void RecordFault(wa_system_jit_context_t& context, const wa::softmmu::Access& access) {
    context.access_status = access.status;
    context.run.fault_address = access.fault_address;
    context.run.exit_subcode = static_cast<uint32_t>(rt::NativeExit::MemoryFault);
}

template <typename T> T ReadMmu(wa_system_jit_context_t* context, uint32_t address) {
    static_assert(std::endian::native == std::endian::little);
    T value{};
    const auto access = wa::softmmu::Read(context->owner->mmu.get(), address,
                                          {reinterpret_cast<uint8_t*>(&value), sizeof(value)});
    if (access.status != WA_OK)
        RecordFault(*context, access);
    return value;
}

template <typename T> void WriteMmu(wa_system_jit_context_t* context, uint32_t address, T value) {
    const auto access =
        wa::softmmu::Write(context->owner->mmu.get(), address,
                           {reinterpret_cast<const uint8_t*>(&value), sizeof(value)});
    if (access.status != WA_OK)
        RecordFault(*context, access);
    else if (access.code_changed)
        RecordFault(*context, {WA_ERR_NOTSUP, address});
}

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
        wa_system_jit_cancel_render(guest, guest->jit.get());
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
    if (JitActive(*guest))
        return WA_ERR_BUSY;
    const float args[] = {time, x, y};
    return Call(*guest, "demo_render_frame", {}, args);
}

int32_t wa_system_demo_generate_audio(wa_system_guest_t* guest, uint32_t address, int32_t frames,
                                      float phase) {
    if (!guest || frames < 0)
        return WA_ERR_INVALID;
    if (JitActive(*guest))
        return WA_ERR_BUSY;
    const uint64_t args[] = {address, static_cast<uint32_t>(frames)};
    return Call(*guest, "demo_generate_audio", args, {&phase, 1});
}

int32_t wa_system_jit_begin_render(wa_system_guest_t* guest, float time, float x, float y,
                                   wa_system_jit_step_t* out) {
    if (!guest || !guest->owns_surface || !out)
        return WA_ERR_INVALID;
    if (JitActive(*guest))
        return WA_ERR_BUSY;
    if (!guest->jit) {
        const auto status =
            wa_dbt_find_symbol(guest->dbt.get(), "demo_render_frame", &guest->render_entry);
        if (status != WA_OK)
            return status;
        std::unique_ptr<wa_system_jit_context_t> frame(new (std::nothrow)
                                                           wa_system_jit_context_t(*guest));
        std::unique_ptr<uint32_t[]> misses(new (std::nothrow) uint32_t[kIotlbEntries]);
        if (!frame || !misses)
            return WA_ERR_NOMEM;
        // Both IOTLBs share immutable miss entries so all accesses honor SoftMMU permissions.
        std::fill_n(misses.get(), kIotlbEntries, UINT32_MAX);
        guest->jit = std::move(frame);
        guest->jit_misses = std::move(misses);
    }

    uint64_t sp;
    const auto status = wa::dbt::PrepareJitStack(*guest->dbt, sp);
    if (status != WA_OK)
        return status;

    auto& frame = *guest->jit;
    const auto misses = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(guest->jit_misses.get()));
    frame.run = {.iotlb_read_offset = misses, .iotlb_write_offset = misses};
    frame.access_status = WA_OK;

    const auto entry = guest->render_entry;
    uint64_t pc = entry;
    if (guest->dbt->arch == WA_ARCH_ARM32) {
        auto& state = *std::construct_at(&frame.arm32);
        state.r[0] = std::bit_cast<uint32_t>(time);
        state.r[1] = std::bit_cast<uint32_t>(x);
        state.r[2] = std::bit_cast<uint32_t>(y);
        state.r[13] = sp;
        state.r[14] = WA_EXIT_SENTINEL_PC;
        pc = state.pc =
            rt::arm32::Location::Make(static_cast<uint32_t>(entry) & ~1u, entry & 1).Key();
    } else {
        auto& state = *std::construct_at(&frame.arm64);
        std::memcpy(state.q[0], &time, sizeof(time));
        std::memcpy(state.q[1], &x, sizeof(x));
        std::memcpy(state.q[2], &y, sizeof(y));
        state.gprs[31] = sp;
        state.gprs[30] = WA_EXIT_SENTINEL_PC;
        state.pc = pc;
    }
    frame.active = true;
    *out = {pc, &frame, 0};
    return WA_SYSTEM_JIT_CONTINUE;
}

int32_t wa_system_jit_handle_exit(wa_system_guest_t* guest, wa_system_jit_context_t* context,
                                  uint64_t next_pc, wa_system_jit_step_t* out) {
    if (!guest || !context || context != guest->jit.get() || !context->active || !out)
        return WA_ERR_INVALID;
    const auto fail = [context](int32_t status) {
        context->active = false;
        return status;
    };
    if (context->run.exit_subcode)
        return fail(context->access_status == WA_OK ? WA_ERR_FAULT : context->access_status);
    // SwiftVM returns small NativeExit codes for services, with zero for dispatch.
    if (next_pc && next_pc < kLoadBias)
        return fail(WA_ERR_NOTSUP);

    const bool arm32 = guest->dbt->arch == WA_ARCH_ARM32;
    auto& pc = arm32 ? context->arm32.pc : context->arm64.pc;
    if (next_pc)
        pc = next_pc;
    auto address = arm32 ? rt::arm32::Location{pc}.Address() : pc;
    if (address >= WA_HOST_STUB_BASE && address < WA_HOST_STUB_LIMIT) {
        void* state = arm32 ? static_cast<void*>(&context->arm32) : &context->arm64;
        const auto status = wa::dbt::DispatchHostStub(*guest->dbt, address, state);
        if (status != WA_OK)
            return fail(status);
        address = arm32 ? rt::arm32::Location{pc}.Address() : pc;
    }
    if (address == WA_EXIT_SENTINEL_PC) {
        const auto result =
            static_cast<uint32_t>(arm32 ? context->arm32.r[0] : context->arm64.gprs[0]);
        *out = {pc, context, std::bit_cast<int32_t>(result)};
        context->active = false;
        return WA_SYSTEM_JIT_DONE;
    }
    if (address < kLoadBias || address >= WA_HOST_STUB_BASE)
        return fail(WA_ERR_FAULT);
    *out = {pc, context, 0};
    return WA_SYSTEM_JIT_CONTINUE;
}

void wa_system_jit_cancel_render(wa_system_guest_t* guest, wa_system_jit_context_t* context) {
    if (guest && context && guest->jit.get() == context)
        context->active = false;
}

int32_t wa_system_jit_compile_pc(wa_system_guest_t* guest, uint64_t pc, uint8_t** out,
                                 size_t* out_size) {
    return guest ? wa::dbt::CompileWasmLocation(*guest->dbt, pc, out, out_size) : WA_ERR_INVALID;
}

uint32_t wa_system_mmu_read8(wa_system_jit_context_t* context, uint32_t address, uint64_t) {
    return ReadMmu<uint8_t>(context, address);
}
uint32_t wa_system_mmu_read16(wa_system_jit_context_t* context, uint32_t address, uint64_t) {
    return ReadMmu<uint16_t>(context, address);
}
uint32_t wa_system_mmu_read32(wa_system_jit_context_t* context, uint32_t address, uint64_t) {
    return ReadMmu<uint32_t>(context, address);
}
uint64_t wa_system_mmu_read64(wa_system_jit_context_t* context, uint32_t address, uint64_t) {
    return ReadMmu<uint64_t>(context, address);
}
void wa_system_mmu_write8(wa_system_jit_context_t* context, uint32_t address, uint32_t value,
                          uint64_t) {
    WriteMmu(context, address, static_cast<uint8_t>(value));
}
void wa_system_mmu_write16(wa_system_jit_context_t* context, uint32_t address, uint32_t value,
                           uint64_t) {
    WriteMmu(context, address, static_cast<uint16_t>(value));
}
void wa_system_mmu_write32(wa_system_jit_context_t* context, uint32_t address, uint32_t value,
                           uint64_t) {
    WriteMmu(context, address, value);
}
void wa_system_mmu_write64(wa_system_jit_context_t* context, uint32_t address, uint64_t value,
                           uint64_t) {
    WriteMmu(context, address, value);
}

int32_t wa_system_jit_numeric_lambda(wa_system_jit_context_t* context, uint32_t opcode,
                                     uint64_t callback, uint64_t state_address, uint64_t a,
                                     uint64_t b, uint64_t* out) {
    if (!context || !context->active || !out || opcode != uint32_t(rt::ir::OpCode::CallLambda))
        return WA_ERR_INVALID;
    if (context->owner->dbt->arch == WA_ARCH_ARM32) {
        if (state_address != reinterpret_cast<uintptr_t>(&context->arm32.fpscr) ||
            callback > UINTPTR_MAX || !rt::arm32::NumericCallbackIdentity(callback))
            return WA_ERR_NOTSUP;
        const auto fn =
            reinterpret_cast<rt::arm32::NumericCallback>(static_cast<uintptr_t>(callback));
        *out = fn(state_address, a, b);
    } else {
        if (state_address != reinterpret_cast<uintptr_t>(&context->arm64) ||
            callback != reinterpret_cast<uintptr_t>(&rt::arm64::ExecuteNumeric))
            return WA_ERR_NOTSUP;
        *out = rt::arm64::ExecuteNumeric(state_address, a, b);
    }
    return WA_OK;
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
