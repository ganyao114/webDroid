#pragma once

#include "wasmandroid/softmmu_linker.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>

namespace wa {

class GuestCall {
  public:
    explicit GuestCall(wa_host_call_ctx_t& context) : context_(context), stack_(context.sp) {}

    const wa_host_call_ctx_t& context() const { return context_; }

    bool arm32() const { return context_.arch == WA_ARCH_ARM32; }
    bool ok() const { return status_ == WA_OK; }
    int32_t status() const { return status_; }

    bool fail(int32_t status) {
        if (ok()) {
            status_ = status;
        }
        return false;
    }

    void result(uint32_t value) { context_.ret0 = value; }
    double fp(size_t index) const { return context_.fargs[index]; }

    uint64_t word(size_t index) {
        const size_t registers = arm32() ? 4 : 8;
        if (index < registers) {
            return arm32() ? static_cast<uint32_t>(context_.args[index]) : context_.args[index];
        }
        uint64_t address = stack_;
        if (!stack_ || !advance(address, (index - registers) * pointer_size())) {
            fail(WA_ERR_FAULT);
            return 0;
        }
        return pointer(address);
    }

    int32_t i32(size_t index) { return std::bit_cast<int32_t>(static_cast<uint32_t>(word(index))); }

    int64_t signed_word(size_t index) {
        return arm32() ? i32(index) : std::bit_cast<int64_t>(word(index));
    }

    size_t pointer_size() const { return arm32() ? 4 : 8; }

    uint64_t pointer(uint64_t address) {
        if (arm32()) {
            uint32_t value = 0;
            read(address, value);
            return value;
        }
        uint64_t value = 0;
        read(address, value);
        return value;
    }

    bool put_pointer(uint64_t address, uint32_t value) {
        return arm32() ? write(address, value) : write(address, static_cast<uint64_t>(value));
    }

    bool advance(uint64_t& address, size_t bytes) {
        if (address > address_limit() || bytes > address_limit() - address) {
            return fail(WA_ERR_OVERFLOW);
        }
        address += bytes;
        return true;
    }

    bool copy_in(uint64_t address, void* target, size_t bytes) {
        if (!ok() || !bytes) {
            return ok();
        }
        return check_range(address, bytes) &&
               check(wa_softmmu_read(context_.mmu, address, target, bytes));
    }

    bool copy_out(uint64_t address, const void* source, size_t bytes) {
        if (!ok() || !bytes) {
            return ok();
        }
        return check_range(address, bytes) &&
               check(wa_softmmu_write(context_.mmu, address, source, bytes));
    }

    template <class T> bool read(uint64_t address, T& value) {
        return copy_in(address, &value, sizeof(value));
    }

    template <class T> bool write(uint64_t address, const T& value) {
        return copy_out(address, &value, sizeof(value));
    }

    template <class T> std::unique_ptr<T[]> buffer(size_t count) {
        if (!ok() || !count) {
            return nullptr;
        }
        if (count > std::numeric_limits<size_t>::max() / sizeof(T)) {
            fail(WA_ERR_OVERFLOW);
            return nullptr;
        }
        std::unique_ptr<T[]> result(new (std::nothrow) T[count]);
        if (!result) {
            fail(WA_ERR_NOMEM);
        }
        return result;
    }

    bool string(uint64_t address, std::string& value, size_t limit, bool truncate = false) {
        value.clear();
        while (value.size() < limit) {
            char chunk[256];
            const size_t page_remaining = WA_PAGE_SIZE - (address & (WA_PAGE_SIZE - 1));
            const size_t bytes = std::min({sizeof(chunk), page_remaining, limit - value.size()});
            if (!copy_in(address, chunk, bytes)) {
                return false;
            }
            const auto* terminator = static_cast<const char*>(std::memchr(chunk, '\0', bytes));
            value.append(chunk, terminator ? static_cast<size_t>(terminator - chunk) : bytes);
            if (terminator) {
                return true;
            }
            if (value.size() < limit && !advance(address, bytes)) {
                return false;
            }
        }
        return truncate || fail(WA_ERR_OVERFLOW);
    }

  private:
    uint64_t address_limit() const { return arm32() ? UINT32_MAX : UINT64_MAX; }

    bool check(int32_t status) { return status == WA_OK || fail(status); }

    bool check_range(uint64_t address, size_t bytes) {
        return (context_.mmu && address && address <= address_limit() &&
                bytes - 1 <= address_limit() - address) ||
               fail(WA_ERR_FAULT);
    }

    wa_host_call_ctx_t& context_;
    uint64_t stack_;
    int32_t status_ = WA_OK;
};

void android_log_write(GuestCall& call);
void android_log_print(GuestCall& call);

} // namespace wa
