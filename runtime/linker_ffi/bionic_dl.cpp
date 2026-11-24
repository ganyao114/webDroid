#include "bionic_internal.h"
#include "elf_image.h"

#include <cerrno>
#include <random>

namespace wa::dbt {
namespace {

constexpr size_t kStackGuardSlot = 5;
constexpr std::string_view kTlsdescName = "__wa_tlsdesc";

void Iterate(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    const auto width = mem.width();
    const auto counters = 4 * width;
    const auto size = counters + 16 + 2 * width;
    const GuestBuffer info(dbt, size);
    std::vector<uint64_t> images;
    images.reserve(dbt.images.size());
    for (const auto& image : dbt.images)
        images.push_back(image.base);
    for (const auto base : images) {
        const auto image = std::ranges::find(dbt.images, base, &LoadedImage::base);
        if (image == dbt.images.end())
            continue;
        mem.WriteWord(info.address, image->bias);
        mem.WriteWord(info.address + width, image->name_address);
        mem.WriteWord(info.address + width * 2, image->phdr);
        mem.Write<uint16_t>(info.address + width * 3, image->phnum);
        mem.Write<uint64_t>(info.address + counters, dbt.image_loads);
        mem.Write<uint64_t>(info.address + counters + 8, dbt.image_unloads);
        mem.WriteWord(info.address + counters + 16, image->tls_id);
        mem.WriteWord(info.address + counters + 16 + width, ThreadTls(dbt, *image));
        const uint64_t args[] = {info.address, size, ctx.args[1]};
        CheckGuest(wa_dbt_call(&dbt, ctx.args[0], args, 3, &ctx.ret0));
        if (static_cast<int32_t>(ctx.ret0))
            break;
    }
}

void FindExidx(wa_host_call_ctx_t& ctx) {
    const auto* image = ImageContaining(*ctx.dbt, ctx.args[0] & ~uint64_t{1});
    GuestMemory(ctx).Write<int32_t>(ctx.args[1], image ? image->exidx_count : 0);
    ctx.ret0 = image ? image->exidx : 0;
}

void DlAddr(wa_host_call_ctx_t& ctx) {
    const auto* image = ImageContaining(*ctx.dbt, ctx.args[0]);
    if (!image)
        return;
    const GuestMemory mem(ctx);
    const auto info = ctx.args[1];
    const auto width = mem.width();
    std::string_view name;
    uint64_t previous = 0;
    for (const auto& [symbol, address] : image->exports) {
        if (address > previous && address <= ctx.args[0]) {
            previous = address;
            name = symbol;
        }
    }
    mem.WriteWord(info, image->name_address);
    mem.WriteWord(info + width, image->base);
    mem.WriteWord(info + 2 * width, name.empty() ? 0 : InternString(*ctx.dbt, name));
    mem.WriteWord(info + 3 * width, previous);
    ctx.ret0 = 1;
}

void TlsGetAddr(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto id = mem.ReadWord(ctx.args[0]);
    const auto offset = mem.ReadWord(ctx.args[0] + mem.width());
    const auto image = std::ranges::find(ctx.dbt->images, id, &LoadedImage::tls_id);
    if (image == ctx.dbt->images.end() || !id || offset >= image->tls_size)
        throw GuestFault{WA_ERR_FAULT};
    ctx.ret0 = ThreadTls(*ctx.dbt, *image) + offset;
}

void TlsDesc(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = GuestMemory(ctx).ReadWord(ctx.args[0] + 8);
    // TLSDESC preserves all registers except the return register x0.
    ctx.ret1 = ctx.args[1];
}

void EmuTls(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    auto& slots = dbt.running->emutls;
    const auto control = ctx.args[0];
    if (const auto it = slots.find(control); it != slots.end()) {
        ctx.ret0 = it->second;
        return;
    }
    const GuestMemory mem(ctx);
    const auto width = mem.width();
    const auto size = mem.ReadWord(control);
    const auto align = mem.ReadWord(control + width);
    const auto initial = mem.ReadWord(control + width * 3);
    if (!std::has_single_bit(align) || size > kTlsCapacity)
        throw GuestFault{WA_ERR_INVALID};
    const auto address = AllocateGuest(dbt, size, align);
    if (!address)
        throw GuestFault{WA_ERR_NOMEM};
    try {
        if (initial && size) {
            std::vector<uint8_t> data(size);
            mem.Read(initial, data.data(), data.size());
            mem.Write(address, data.data(), data.size());
        }
        slots.emplace(control, address);
    } catch (...) {
        FreeGuest(dbt, address);
        throw;
    }
    ctx.ret0 = address;
}

void AuxVal(wa_host_call_ctx_t& ctx) {
    switch (ctx.args[0]) {
    case 6:
        ctx.ret0 = WA_PAGE_SIZE;
        break; // AT_PAGESZ
    case 11:
    case 12:
    case 13:
    case 14: // UIDs and GIDs
    case 16:
    case 23:
    case 26:
        break; // HWCAP, SECURE, HWCAP2
    default:
        SetErrno(*ctx.dbt, GuestErrno(ENOENT));
        break;
    }
}

} // namespace

const LoadedImage* ImageContaining(const wa_dbt& dbt, uint64_t address) {
    for (const auto& image : dbt.images)
        if (std::ranges::any_of(image.segments, [address](const auto& segment) {
                return address >= segment.first && address < segment.second;
            }))
            return &image;
    return nullptr;
}

void InitializeThreadTls(wa_dbt& dbt, GuestThread& thread, const LoadedImage& image) {
    if (!thread.thread_pointer || !image.tls_id || thread.initialized_tls.contains(image.tls_id))
        return;
    const auto address = thread.thread_pointer + image.tls_offset;
    GuestMemory(dbt.mmu, dbt.arch)
        .Write(address, image.tls_initial.data(), image.tls_initial.size());
    thread.initialized_tls.insert(image.tls_id);
}

uint64_t ThreadPointer(wa_dbt& dbt) {
    auto& thread = *dbt.running;
    if (!thread.thread_pointer) {
        thread.thread_pointer = AllocateGuest(dbt, kTlsCapacity, kTlsCapacity);
        if (!thread.thread_pointer)
            throw GuestFault{WA_ERR_NOMEM};
        const GuestMemory mem(dbt.mmu, dbt.arch);
        const auto guard = FindVirtualSymbol(dbt, "libc.so", "__stack_chk_guard");
        mem.WriteWord(thread.thread_pointer, thread.thread_pointer);
        mem.WriteWord(thread.thread_pointer + kStackGuardSlot * mem.width(), mem.ReadWord(guard));
        for (const auto& image : dbt.images)
            InitializeThreadTls(dbt, thread, image);
    }
    return thread.thread_pointer;
}

uint64_t ThreadTls(wa_dbt& dbt, const LoadedImage& image) {
    return image.tls_id ? ThreadPointer(dbt) + image.tls_offset : 0;
}

void RegisterDynamicLinker(wa_dbt& dbt) {
    const Builtin dl[] = {
        {"dl_iterate_phdr", Dispatch<Iterate>},
        {"dl_unwind_find_exidx", Dispatch<FindExidx>},
        {"dladdr", Dispatch<DlAddr>},
    };
    Register(dbt, "libdl.so", dl);
    const Builtin c[] = {
        {"dl_iterate_phdr", Dispatch<Iterate>},
        {"dl_unwind_find_exidx", Dispatch<FindExidx>},
        {"__gnu_Unwind_Find_exidx", Dispatch<FindExidx>},
        {"__tls_get_addr", Dispatch<TlsGetAddr>},
        {"__emutls_get_address", Dispatch<EmuTls>},
        {"getauxval", Dispatch<AuxVal>},
    };
    Register(dbt, "libc.so", c);
    CheckGuest(RegisterHost(dbt, "libc.so", kTlsdescName, Dispatch<TlsDesc>, nullptr, true));
    const auto guard = AllocateGuest(dbt, 8);
    if (!guard)
        throw GuestFault{WA_ERR_NOMEM};
    std::random_device random;
    const auto bits = ((uint64_t{random()} << 32) | uint64_t{random()}) & ~uint64_t{255};
    GuestMemory(dbt.mmu, dbt.arch).WriteWord(guard, bits);
    CheckGuest(RegisterData(dbt, "libc.so", "__stack_chk_guard", guard));
}

} // namespace wa::dbt
