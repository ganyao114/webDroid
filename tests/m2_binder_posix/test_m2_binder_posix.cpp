#include "platform.h"
#include "wasmandroid/libkern.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

namespace {

uint64_t address(const void* pointer) {
    return reinterpret_cast<uintptr_t>(pointer);
}

int64_t call(uint32_t id, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0,
             uint64_t e = 0, uint64_t f = 0) {
    return wa_syscall(id, a, b, c, d, e, f);
}

void testTimeRandomAndName() {
    char names[6][65]{};
    CHECK(call(160, address(names)) == WA_OK);
    CHECK(std::strcmp(names[0], "Linux") == 0 && std::strcmp(names[1], "wasm-android") == 0);
    CHECK(std::strcmp(names[2], "6.6.0-wasm") == 0 && std::strcmp(names[4], "aarch64") == 0);
    CHECK(call(160) == WA_ERR_FAULT);

    std::array<uint8_t, 64> one{}, two{};
    CHECK(call(278, address(one.data()), one.size(), 0) == one.size());
    CHECK(call(278, address(two.data()), two.size(), 1) == two.size());
    CHECK(one != two && std::ranges::any_of(one, [](uint8_t b) { return b != 0; }));
    CHECK(call(278, 0, 1, 0) == WA_ERR_FAULT && call(278, 0, 0, 8) == WA_ERR_INVALID);
    CHECK(call(278, 0, 0, 0) == 0);

    int64_t time[2], before[2], after[2];
    for (int clock : {0, 1, 7}) {
        CHECK(call(113, clock, address(time)) == WA_OK);
        CHECK(time[0] > 0 && time[1] >= 0 && time[1] < 1'000'000'000);
    }
    CHECK(call(113, 99, address(time)) == WA_ERR_INVALID && call(113, 1) == WA_ERR_FAULT);
    const int64_t sleep[] = {0, 2'000'000}, bad[] = {0, 1'000'000'000};
    CHECK(call(113, 1, address(before)) == WA_OK);
    CHECK(call(101, address(sleep)) == WA_OK);
    CHECK(call(113, 1, address(after)) == WA_OK);
    CHECK((after[0] - before[0]) * 1'000'000'000 + after[1] - before[1] >= sleep[1]);
    CHECK(call(101, address(bad)) == WA_ERR_INVALID && call(101) == WA_ERR_FAULT);

    const char name[] = "abcdefghijklmnopqrstuvwxyz";
    char label[16]{};
    CHECK(call(167, 15, address(name)) == WA_OK && call(167, 16, address(label)) == WA_OK);
    CHECK(std::strcmp(label, "abcdefghijklmno") == 0 && call(167, 16) == WA_ERR_FAULT);
    std::jthread other([&] {
        char label[16]{};
        CHECK(call(167, 16, address(label)) == WA_OK && std::strcmp(label, "wasm-android") == 0);
        CHECK(call(167, 15, address("worker")) == WA_OK);
        CHECK(call(167, 16, address(label)) == WA_OK && std::strcmp(label, "worker") == 0);
    });
    other.join();
    CHECK(call(167, 16, address(label)) == WA_OK && std::strcmp(label, "abcdefghijklmno") == 0);

    CHECK(wa_syscall_set_arch(WA_ARCH_ARM32) == WA_OK);
    CHECK(call(122, address(names)) == WA_OK && std::strcmp(names[4], "armv7l") == 0);
    CHECK(call(384, address(two.data()), two.size(), 0) == two.size());
    int32_t time32[2];
    CHECK(call(263, 0, address(time32)) == WA_OK && time32[0] > 0);
    CHECK(call(403, 7, address(time)) == WA_OK && time[0] > 0);
    const int32_t sleep32[] = {0, 100'000};
    CHECK(call(162, address(sleep32)) == WA_OK);
    CHECK(call(172, 16, address(label)) == WA_OK && std::strcmp(label, "abcdefghijklmno") == 0);
    CHECK(wa_syscall_set_arch(WA_ARCH_ARM64) == WA_OK);
}

struct Iov {
    uint64_t base, size;
};

void testVectorAndPipe() {
    int32_t pipe[2];
    constexpr int32_t cloexec = 0x80000, nonblock = 0x800;
    CHECK(call(59, address(pipe), cloexec) == WA_OK);
    CHECK(call(25, pipe[0], 1) == 1 && call(25, pipe[1], 3) == WA_O_WRONLY);
    CHECK(call(25, pipe[0], 2, 0) == WA_OK && call(25, pipe[0], 1) == 0);
    const int32_t duplicate = call(25, pipe[0], 0, 12);
    CHECK(duplicate >= 12 && call(25, duplicate, 1) == 0);
    CHECK(call(25, duplicate, 4, nonblock) == WA_OK && call(25, pipe[0], 3) == nonblock);
    char first[2]{}, second[8]{};
    Iov read[] = {{address(first), sizeof(first)}, {address(second), sizeof(second)}};
    CHECK(call(65, duplicate, address(read), 2) == WA_ERR_AGAIN);
    CHECK(call(65, duplicate, address(read), 1025) == WA_ERR_INVALID);
    CHECK(call(65, duplicate, 0, 1) == WA_ERR_FAULT);
    CHECK(call(62, pipe[0], 0, 0) == -29);
    CHECK(call(57, pipe[0]) == WA_OK);
    CHECK(call(65, pipe[0], address(read), 2) == WA_ERR_BADF &&
          call(25, pipe[0], 1) == WA_ERR_BADF);
    const Iov write[] = {{address("ab"), 2}, {0, 0}, {address("cde"), 3}};
    CHECK(call(66, pipe[1], address(write), 3) == 5);
    CHECK(call(65, duplicate, address(read), 2) == 5);
    CHECK(std::memcmp(first, "ab", 2) == 0 && std::strcmp(second, "cde") == 0);
    CHECK(call(57, pipe[1]) == WA_OK && call(65, duplicate, address(read), 2) == 0);
    CHECK(call(57, duplicate) == WA_OK);

    // Verify that stdout uses the same scatter/gather path and restores the fd.
    const int stdout_fd = dup(STDOUT_FILENO);
    CHECK(stdout_fd >= 0 && ::pipe(pipe) == 0);
    std::fflush(stdout);
    CHECK(dup2(pipe[1], STDOUT_FILENO) == STDOUT_FILENO && ::close(pipe[1]) == 0);
    const int64_t written = call(66, STDOUT_FILENO, address(write), 3);
    CHECK(dup2(stdout_fd, STDOUT_FILENO) == STDOUT_FILENO && ::close(stdout_fd) == 0);
    CHECK(written == 5 && call(65, pipe[0], address(read), 2) == 5 && ::close(pipe[0]) == 0);
    CHECK(std::memcmp(first, "ab", 2) == 0 && std::strcmp(second, "cde") == 0);
}

template <typename T, size_t N> T field(const std::array<uint8_t, N>& bytes, size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

void testFile() {
    char path[] = "./m2-posix.XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    const Iov write[] = {{address("123"), 3}, {address("4567"), 4}};
    CHECK(call(66, fd, address(write), 2) == 7);
    CHECK(call(62, fd, -3, 2) == 4);
    CHECK(call(25, fd, 4, WA_O_APPEND) == WA_OK && (call(25, fd, 3) & WA_O_APPEND));
    const int second = call(25, fd, 0, 20);
    CHECK(second >= 20 && call(25, second, 1) == 0 && call(62, second, 0, 1) == 4);
    CHECK(call(64, second, address("89"), 2) == 2 && call(62, fd, 0, 1) == 9);
    CHECK(call(25, second, 4, 0) == WA_OK && !(call(25, fd, 3) & WA_O_APPEND));
    CHECK(call(57, second) == WA_OK);

    std::array<uint8_t, 136> st;
    st.fill(0xa5);
    CHECK(call(80, fd, address(st.data())) == WA_OK && field<int64_t>(st, 48) == 9);
    CHECK((field<uint32_t>(st, 16) & 0170000) == 0100000 && st[128] == 0xa5);
    const int directory = ::open(".", O_RDONLY | O_DIRECTORY);
    CHECK(directory >= 0);
    CHECK(call(79, directory, address(path), address(st.data()), 0) == WA_OK);
    CHECK(field<int64_t>(st, 48) == 9 && ::close(directory) == 0);
    CHECK(call(79, fd, address(""), address(st.data()), 0x1000) == WA_OK);
    CHECK(call(79, fd, address(""), address(st.data()), 0) == WA_ERR_NOENT);
    CHECK(call(80, -1, address(st.data())) == WA_ERR_BADF);

    CHECK(wa_syscall_set_arch(WA_ARCH_ARM32) == WA_OK);
    st.fill(0xa5);
    CHECK(call(195, address(path), address(st.data())) == WA_OK);
    CHECK(field<int64_t>(st, 48) == 9 && (field<uint32_t>(st, 16) & 0170000) == 0100000);
    CHECK(st[104] == 0xa5 && call(197, fd, address(st.data())) == WA_OK);
    CHECK(call(327, -100, address(path), address(st.data())) == WA_OK &&
          field<int64_t>(st, 48) == 9);
    const int64_t legacy = call(106, address(path), address(st.data()));
    CHECK(legacy == WA_ERR_OVERFLOW || (legacy == WA_OK && field<uint32_t>(st, 20) == 9));
    CHECK(call(19, fd, -2, 2) == 7);
    int64_t seek;
    CHECK(call(140, fd, 1, 12, address(&seek), 0) == WA_OK && seek == (int64_t{1} << 32) + 12);
    CHECK(call(55, fd, 1) == call(221, fd, 1));
    CHECK(call(6, fd) == WA_OK && unlink(path) == 0);
    CHECK(wa_syscall_set_arch(WA_ARCH_ARM64) == WA_OK);
}

void testMemory() {
    constexpr uint64_t page = WA_PAGE_SIZE;
    constexpr uint32_t anonymous = 0x22, fixed = 0x10, no_replace = 0x100000;
    CHECK(call(222, 0, 0, 3, anonymous, -1, 0) == WA_ERR_INVALID);
    CHECK(call(222, 0, page, 3, 2, -1, 0) == WA_ERR_NOTSUP);
    const int64_t mapping = call(222, 0, 3 * page, 3, anonymous, -1, 0);
    CHECK(mapping > 0 && mapping % page == 0);
    auto* bytes = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(mapping));
    CHECK(bytes[0] == 0 && bytes[3 * page - 1] == 0);
    bytes[page] = 31;
    CHECK(call(226, mapping + 1, page, 1) == WA_ERR_INVALID);
    CHECK(call(226, mapping + page, page, 1) == WA_OK);
    CHECK(call(222, mapping + page, page, 3, anonymous | no_replace, -1, 0) == WA_ERR_EXIST);
    CHECK(call(215, mapping + page, page) == WA_OK);
    CHECK(call(226, mapping, 3 * page, 1) == WA_ERR_NOMEM);
    CHECK(call(222, mapping + page, page, 3, anonymous | fixed, -1, 0) == mapping + page);
    CHECK(bytes[page] == 0 && call(226, mapping, 3 * page, 3) == WA_OK);
    CHECK(call(215, mapping, 3 * page) == WA_OK && call(226, mapping, page, 3) == WA_ERR_NOMEM);
    CHECK(call(215, mapping, page) == WA_OK);

    CHECK(wa_syscall_set_arch(WA_ARCH_ARM32) == WA_OK);
    const int64_t mapped32 = call(192, 0, page, 3, anonymous, -1, 0);
    CHECK(mapped32 > 0 && call(125, mapped32, page, 1) == WA_OK &&
          call(91, mapped32, page) == WA_OK);
    const int64_t heap = call(45);
    CHECK(heap > 0 && call(45, heap + page + 71) == heap + page + 71);
    auto* heap_data = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(heap));
    CHECK(heap_data[page] == 0);
    heap_data[page] = 92;
    CHECK(call(45, heap - page) == heap + page + 71);
    CHECK(call(45, heap + 1) == heap + 1 && call(125, heap + page, page, 3) == WA_ERR_NOMEM);
    CHECK(call(45, heap + page + 1) == heap + page + 1 && heap_data[page] == 0);
    CHECK(call(45, heap) == heap);
    CHECK(wa_syscall_set_arch(WA_ARCH_ARM64) == WA_OK);
}

} // namespace

int main() {
    testTimeRandomAndName();
    testVectorAndPipe();
    testFile();
    testMemory();
    std::puts("PASS ARM32 and ARM64 POSIX syscall coverage");
}
