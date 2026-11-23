#pragma once

#include "wasmandroid/softmmu_linker.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace wa::dbt {

static_assert(std::endian::native == std::endian::little);

struct GuestFault {
    int32_t status;
};

inline void CheckGuest(int32_t status) {
    if (status != WA_OK) throw GuestFault{status};
}

inline uint64_t AlignUp(uint64_t value, size_t alignment) {
    return (value + alignment - 1) & ~(uint64_t{alignment} - 1);
}

class GuestMemory {
  public:
    GuestMemory(wa_softmmu_t* mmu, wa_guest_arch_t arch) : mmu_(mmu), width_(arch == WA_ARCH_ARM64 ? 8 : 4) {}
    explicit GuestMemory(const wa_host_call_ctx_t& ctx) : GuestMemory(ctx.mmu, ctx.arch) {}
    size_t width() const { return width_; }

    void Read(uint64_t address, void* out, size_t count) const {
        if (count) CheckGuest(wa_softmmu_read(mmu_, address, out, count));
    }

    void Write(uint64_t address, const void* bytes, size_t count) const {
        if (count) CheckGuest(wa_softmmu_write(mmu_, address, bytes, count));
    }

    template<class T> T Read(uint64_t address) const {
        T value{};
        Read(address, &value, sizeof(value));
        return value;
    }

    template<class T> void Write(uint64_t address, T value) const {
        Write(address, &value, sizeof(value));
    }

    uint64_t ReadWord(uint64_t address) const {
        return width_ == 8 ? Read<uint64_t>(address) : Read<uint32_t>(address);
    }

    void WriteWord(uint64_t address, uint64_t value) const { Write(address, &value, width_); }

    std::string String(uint64_t address, size_t limit = 1 << 20, bool truncate = false) const {
        std::string result;
        std::array<char, 256> bytes;
        while (result.size() < limit) {
            const size_t count = std::min({bytes.size(), limit - result.size(),
                                          size_t(WA_PAGE_SIZE - (address & (WA_PAGE_SIZE - 1)))});
            Read(address, bytes.data(), count);
            const auto* end = static_cast<const char*>(std::memchr(bytes.data(), 0, count));
            result.append(bytes.data(), end ? static_cast<size_t>(end - bytes.data()) : count);
            if (end) return result;
            address += count;
        }
        if (truncate) return result;
        throw GuestFault{WA_ERR_OVERFLOW};
    }

  private:
    wa_softmmu_t* mmu_;
    size_t width_;
};

enum class ArgKind { Word, Long, Float, Double };

struct ArgLocation {
    enum Bank { Gpr, Fp, Stack } bank;
    size_t offset;
    size_t size;
};

// Classify scalar arguments, both when building a call and when decoding a
// callback. Android ARMv7 uses the base PCS, including for variadic calls.
class ArgumentLayout {
  public:
    explicit ArgumentLayout(wa_guest_arch_t arch) : a64_(arch == WA_ARCH_ARM64) {}

    ArgLocation Next(ArgKind kind) {
        const bool wide = kind == ArgKind::Long || kind == ArgKind::Double;
        const bool fp = kind == ArgKind::Float || kind == ArgKind::Double;
        if (a64_) {
            if (fp && fpr_ < 8) return {ArgLocation::Fp, fpr_++, 8};
            if (!fp && gpr_ < 8) return {ArgLocation::Gpr, gpr_++, 8};
            return StackSlot(8);
        }
        const size_t words = wide ? 2 : 1;
        if (wide) gpr_ = (gpr_ + 1) & ~size_t{1};
        if (gpr_ + words <= 4) {
            const auto first = gpr_;
            gpr_ += words;
            return {ArgLocation::Gpr, first, words * 4};
        }
        gpr_ = 4;
        return StackSlot(words * 4);
    }

  private:
    ArgLocation StackSlot(size_t size) {
        const size_t offset = static_cast<size_t>(AlignUp(stack_, size));
        stack_ = offset + size;
        return {ArgLocation::Stack, offset, size};
    }

    bool a64_;
    size_t gpr_ = 0, fpr_ = 0, stack_ = 0;
};

class CallArguments {
  public:
    explicit CallArguments(wa_guest_arch_t arch) : layout_(arch), arm32_(arch == WA_ARCH_ARM32) {}

    void Add(ArgKind kind, uint64_t bits) {
        const auto slot = layout_.Next(kind);
        switch (slot.bank) {
        case ArgLocation::Gpr:
            frame_.gpr[slot.offset] = slot.size == 4 ? static_cast<uint32_t>(bits) : bits;
            // A 64-bit scalar occupies two consecutive ARM32 core registers.
            if (slot.size == 8 && arm32_) frame_.gpr[slot.offset + 1] = bits >> 32;
            break;
        case ArgLocation::Fp: frame_.fp_bits[slot.offset] = bits; break;
        case ArgLocation::Stack:
            stack_.resize(slot.offset + slot.size);
            std::memcpy(stack_.data() + slot.offset, &bits, slot.size);
            break;
        }
    }

    wa_guest_call_frame_t Frame() const {
        auto result = frame_;
        result.stack = stack_.data();
        result.stack_size = stack_.size();
        return result;
    }

  private:
    ArgumentLayout layout_;
    const bool arm32_;
    wa_guest_call_frame_t frame_{};
    std::vector<uint8_t> stack_;
};

class CallReader {
  public:
    explicit CallReader(const wa_host_call_ctx_t& ctx)
        : ctx_(ctx), memory_(ctx), layout_(ctx.arch) {}

    uint64_t Next(ArgKind kind = ArgKind::Word) {
        const auto slot = layout_.Next(kind);
        switch (slot.bank) {
        case ArgLocation::Gpr:
            if (memory_.width() == 4 && slot.size == 8)
                return ctx_.args[slot.offset] | (ctx_.args[slot.offset + 1] << 32);
            return ctx_.args[slot.offset];
        case ArgLocation::Fp: return ctx_.fp_bits[slot.offset];
        case ArgLocation::Stack:
            return slot.size == 8 ? memory_.Read<uint64_t>(ctx_.sp + slot.offset)
                                  : memory_.Read<uint32_t>(ctx_.sp + slot.offset);
        }
        return 0;
    }

  private:
    const wa_host_call_ctx_t& ctx_;
    GuestMemory memory_;
    ArgumentLayout layout_;
};

class VaListReader {
  public:
    VaListReader(const wa_host_call_ctx_t& ctx, uint64_t address) : memory_(ctx) {
        if (memory_.width() == 4) {
            // Clang passes the one-word AAPCS32 va_list aggregate by value.
            stack_ = address;
        } else {
            // The 32-byte AAPCS64 aggregate is passed indirectly.
            stack_ = memory_.Read<uint64_t>(address);
            gr_top_ = memory_.Read<uint64_t>(address + 8);
            vr_top_ = memory_.Read<uint64_t>(address + 16);
            gr_offs_ = memory_.Read<int32_t>(address + 24);
            vr_offs_ = memory_.Read<int32_t>(address + 28);
        }
    }

    uint64_t Next(ArgKind kind = ArgKind::Word) {
        if (memory_.width() == 8) {
            const bool fp = kind == ArgKind::Float || kind == ArgKind::Double;
            auto& offset = fp ? vr_offs_ : gr_offs_;
            const int32_t stride = fp ? 16 : 8;
            if (offset < 0 && offset % stride == 0) {
                const auto address = (fp ? vr_top_ : gr_top_) + offset;
                offset += stride;
                return memory_.Read<uint64_t>(address);
            }
            stack_ = AlignUp(stack_, 8);
            const auto address = stack_;
            stack_ += 8;
            return memory_.Read<uint64_t>(address);
        }
        const bool wide = kind == ArgKind::Long || kind == ArgKind::Double;
        const size_t size = wide ? 8 : 4;
        const auto address = AlignUp(stack_, size);
        stack_ = address + size;
        return wide ? memory_.Read<uint64_t>(address) : memory_.Read<uint32_t>(address);
    }

  private:
    GuestMemory memory_;
    uint64_t stack_ = 0, gr_top_ = 0, vr_top_ = 0;
    int32_t gr_offs_ = 0, vr_offs_ = 0;
};

inline void SetScalarResult(wa_host_call_ctx_t& ctx, ArgKind kind, uint64_t bits) {
    if (ctx.arch == WA_ARCH_ARM64 && (kind == ArgKind::Float || kind == ArgKind::Double)) {
        ctx.has_fp_ret_bits = true;
        ctx.fp_ret_bits = bits;
    } else {
        ctx.ret0 = bits;
        if (ctx.arch == WA_ARCH_ARM32) ctx.ret1 = bits >> 32;
    }
}

} // namespace wa::dbt
