#include "platform.h"
#include "wasmandroid/libkern.h"

#include <cstring>

namespace {

uint64_t address(const void* pointer) {
    return reinterpret_cast<uintptr_t>(pointer);
}

int64_t call(uint32_t id, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0,
             uint64_t e = 0, uint64_t f = 0) {
    return wa_syscall(id, a, b, c, d, e, f);
}

template <typename Word>
bool vectorPipe(uint32_t pipe2, uint32_t readv, uint32_t writev, uint32_t close) {
    int32_t pipe[2];
    if (call(pipe2, address(pipe), 0x800) != WA_OK) {
        return false;
    }
    struct Iov {
        Word base, length;
    };
    char output[4]{};
    const Iov write[] = {{static_cast<Word>(address("a")), 1},
                         {static_cast<Word>(address("bc")), 2}};
    const Iov read[] = {{static_cast<Word>(address(output)), 2},
                        {static_cast<Word>(address(output + 2)), 1}};
    const bool exact = call(writev, pipe[1], address(write), 2) == 3 &&
                       call(readv, pipe[0], address(read), 2) == 3 &&
                       std::strcmp(output, "abc") == 0;
    const auto reader = call(close, pipe[0]), writer = call(close, pipe[1]);
    return exact && reader == WA_OK && writer == WA_OK;
}

} // namespace

extern "C" int32_t m2_posix_smoke() {
    char fields[6][65], thread_name[16];
    uint8_t random[17]{};
    int64_t clock[2];
    if (wa_syscall_set_arch(WA_ARCH_ARM64) != WA_OK || call(160, address(fields)) != WA_OK ||
        std::strcmp(fields[4], "aarch64") != 0 || call(113, 7, address(clock)) != WA_OK ||
        call(278, address(random), sizeof(random), 0) != sizeof(random) ||
        !vectorPipe<uint64_t>(59, 65, 66, 57)) {
        return -1;
    }
    const uint64_t output[] = {address("wasm"), 4, address("-posix"), 6};
    if (call(66, 1, address(output), 2) != 10 || call(167, 15, address("m2-client")) != WA_OK ||
        call(167, 16, address(thread_name)) != WA_OK ||
        std::strcmp(thread_name, "m2-client") != 0) {
        return -2;
    }
    if (wa_syscall_set_arch(WA_ARCH_ARM32) != WA_OK || call(122, address(fields)) != WA_OK ||
        std::strcmp(fields[4], "armv7l") != 0 || !vectorPipe<uint32_t>(359, 145, 146, 6)) {
        return -3;
    }
    const int64_t mapped = call(192, 0, WA_PAGE_SIZE, 3, 0x22, -1);
    if (mapped <= 0 || *reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(mapped)) != 0 ||
        call(125, mapped, WA_PAGE_SIZE, 1) != WA_OK || call(91, mapped, WA_PAGE_SIZE) != WA_OK ||
        call(125, mapped, WA_PAGE_SIZE, 1) != WA_ERR_NOMEM) {
        return -4;
    }
    return wa_syscall_set_arch(WA_ARCH_ARM64);
}
