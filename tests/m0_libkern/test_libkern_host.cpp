#include "binder_transport.h"
#include "platform.h"
#include "wasmandroid/jit_registry.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <latch>
#include <thread>
#include <unistd.h>
#include <vector>

#define CHECK(expression)                                                                         \
    do {                                                                                          \
        if (!(expression)) {                                                                      \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                   \
            std::abort();                                                                         \
        }                                                                                         \
    } while (false)

using namespace std::chrono_literals;

namespace {

void test_futex() {
    alignas(4) uint32_t word = 0;
    CHECK(wa_futex_wait(&word, 1, -1) == WA_ERR_AGAIN);
    CHECK(wa_futex_wait(&word, 0, 0) == WA_ERR_TIMEDOUT);
    CHECK(wa_futex_wait(&word, 0, 2'000'000) == WA_ERR_TIMEDOUT);
    CHECK(wa_futex_wait(nullptr, 0, 0) == WA_ERR_INVALID);

    std::latch starting(3);
    std::atomic<uint32_t> finished{0};
    std::array<std::jthread, 3> waiters;
    for (size_t i = 0; i < waiters.size(); ++i) {
        waiters[i] = std::jthread([&, i] {
            starting.count_down();
            CHECK(wa_futex_wait(&word, 0, i == 0 ? -1 : 2'000'000'000) == WA_OK);
            ++finished;
        });
    }
    starting.wait();
    CHECK(wa_futex_wake(&word, 0) == 0);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    int32_t notified = 0;
    while (!notified && std::chrono::steady_clock::now() < deadline) {
        notified = wa_futex_wake(&word, 1);
        std::this_thread::yield();
    }
    CHECK(notified == 1);
    while (!finished.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    CHECK(finished.load() == 1);
    while (notified < 3 && std::chrono::steady_clock::now() < deadline) {
        notified += wa_futex_wake(&word, UINT32_MAX);
        std::this_thread::yield();
    }
    CHECK(notified == 3);
    for (auto& waiter : waiters) waiter.join();
    CHECK(finished == 3 && word == 0);
}

void test_epoll() {
    const auto ep = wa_epoll_create(0);
    const auto eventfd = wa_eventfd(0, 0x800);
    int32_t pipefd[2];
    CHECK(ep >= 0 && eventfd >= 0 && wa_pipe(pipefd) == WA_OK);
    CHECK(wa_eventfd(UINT64_MAX, 0) == WA_ERR_INVALID);
    const wa_epoll_event_t event_watch{WA_EPOLLIN, 0, 101};
    const wa_epoll_event_t pipe_watch{WA_EPOLLIN, 0, 202};
    CHECK(wa_epoll_ctl(ep, WA_EPOLL_CTL_ADD, eventfd, &event_watch) == WA_OK);
    CHECK(wa_epoll_ctl(ep, WA_EPOLL_CTL_ADD, eventfd, &event_watch) == WA_ERR_EXIST);
    CHECK(wa_epoll_ctl(ep, WA_EPOLL_CTL_ADD, pipefd[0], &pipe_watch) == WA_OK);
    uint64_t counter = 0;
    CHECK(wa_vfs_read(eventfd, &counter, sizeof(counter)) == WA_ERR_AGAIN);
    wa_epoll_event_t output{};
    CHECK(wa_epoll_wait(ep, &output, 1, 2) == 0);

    constexpr char bytes[] = "pipe payload";
    std::latch listening(1);
    std::jthread reader([&] {
        listening.count_down();
        uint32_t seen = 0;
        while (seen != 3) {
            CHECK(wa_epoll_wait(ep, &output, 1, 1000) == 1);
            CHECK(output.events == WA_EPOLLIN && output._pad == 0);
            if (output.data == 101) {
                CHECK(!(seen & 1));
                CHECK(wa_vfs_read(eventfd, &counter, sizeof(counter)) == sizeof(counter));
                CHECK(counter == 42);
                seen |= 1;
            } else {
                CHECK(output.data == 202 && !(seen & 2));
                char readback[sizeof(bytes)]{};
                CHECK(wa_vfs_read(pipefd[0], readback, sizeof(readback)) == sizeof(readback));
                CHECK(std::memcmp(bytes, readback, sizeof(bytes)) == 0);
                seen |= 2;
            }
        }
    });
    listening.wait();
    const uint64_t signal = 42;
    CHECK(wa_vfs_write(eventfd, &signal, sizeof(signal)) == sizeof(signal));
    CHECK(wa_vfs_write(pipefd[1], bytes, sizeof(bytes)) == sizeof(bytes));
    reader.join();
    CHECK(wa_vfs_close(pipefd[1]) == WA_OK);
    CHECK(wa_epoll_wait(ep, &output, 1, 0) == 1 && output.events == WA_EPOLLHUP);
    CHECK(output.data == 202);
    char eof;
    CHECK(wa_vfs_read(pipefd[0], &eof, 1) == 0);
    CHECK(wa_epoll_ctl(ep, WA_EPOLL_CTL_DEL, pipefd[0], nullptr) == WA_OK);
    CHECK(wa_epoll_ctl(ep, WA_EPOLL_CTL_DEL, pipefd[0], nullptr) == WA_ERR_NOENT);
    CHECK(wa_epoll_wait(ep, &output, 1, 0) == 0);
    for (int32_t fd : {ep, eventfd, pipefd[0]}) CHECK(wa_vfs_close(fd) == WA_OK);

    CHECK(wa_pipe(pipefd) == WA_OK);
    std::array<uint8_t, 9000> large{}, received{};
    for (size_t i = 0; i < large.size(); ++i) large[i] = static_cast<uint8_t>(i);
    std::jthread writer([&] {
        CHECK(wa_vfs_write(pipefd[1], large.data(), large.size()) == large.size());
        CHECK(wa_vfs_close(pipefd[1]) == WA_OK);
    });
    for (size_t consumed = 0; consumed < received.size();) {
        const auto count = wa_vfs_read(pipefd[0], received.data() + consumed,
                                       received.size() - consumed);
        CHECK(count > 0);
        consumed += count;
    }
    writer.join();
    CHECK(received == large && wa_vfs_close(pipefd[0]) == WA_OK);
}

void test_binder() {
    std::vector<std::byte> ring(512 * 1024);
    CHECK(wa_binder_init_ring(ring.data(), ring.size(), 11) == WA_OK);
    uint32_t magic;
    std::memcpy(&magic, ring.data(), sizeof(magic));
    CHECK(magic == WA_BINDER_RING_MAGIC);
    std::jthread server([&] {
        CHECK(wa_binder_init_ring(ring.data(), ring.size(), 22) == WA_OK);
        for (uint32_t i = 0; i < 50; ++i) {
            wa_binder_msg_t header{};
            uint32_t value;
            CHECK(wa_binder_recv(&header, &value, sizeof(value), 1000) == WA_OK);
            CHECK(header.src_pid == 11 && header.dst_pid == 22 && header.txn_id == i);
            CHECK(header.code == 7 && header.flags == 0 && header.payload_len == sizeof(value));
            CHECK(value == i * 17);
            CHECK(wa_binder_send(11, i, 8, 1, &value, sizeof(value)) == WA_OK);
        }
    });
    for (uint32_t i = 0; i < 50; ++i) {
        const uint32_t value = i * 17;
        CHECK(wa_binder_send(22, i, 7, 0, &value, sizeof(value)) == WA_OK);
        wa_binder_msg_t header{};
        uint32_t reply;
        CHECK(wa_binder_recv(&header, &reply, sizeof(reply), 1000) == WA_OK);
        CHECK(header.src_pid == 22 && header.dst_pid == 11 && header.txn_id == i);
        CHECK(header.code == 8 && header.flags == 1 && reply == value);
    }
    server.join();

    const auto offset = wa_binder_ring_reserve(ring.data(), 11, 22, 51, 7, 0, WA_BINDER_MAX_PAYLOAD);
    CHECK(offset >= 0);
    const auto* write = ring.data() + offset + sizeof(wa_binder_msg_t);
    ring[offset + sizeof(wa_binder_msg_t)] = std::byte{0xfa};
    CHECK(wa_binder_ring_commit(ring.data(), offset) == WA_OK);
    CHECK(wa_binder_ring_claim(ring.data(), 11, WA_BINDER_MAX_PAYLOAD, 0) == WA_ERR_AGAIN);
    CHECK(wa_binder_ring_claim(ring.data(), 22, 4, 0) == WA_ERR_OVERFLOW);
    CHECK(wa_binder_ring_claim(ring.data(), 22, WA_BINDER_MAX_PAYLOAD, 0) == offset);
    CHECK(wa_binder_ring_reply(ring.data(), offset, 8, 1) == WA_OK);
    CHECK(wa_binder_ring_claim(ring.data(), 11, WA_BINDER_MAX_PAYLOAD, 0) == offset);
    CHECK(*write == std::byte{0xfa});
    CHECK(wa_binder_ring_release(ring.data(), offset) == WA_OK);
    wa_binder_msg_t header{};
    CHECK(wa_binder_recv(&header, nullptr, 0, 2) == WA_ERR_TIMEDOUT);

    std::array<std::jthread, 3> producers;
    for (uint32_t j = 0; j < producers.size(); ++j) {
        producers[j] = std::jthread([&, j] {
            CHECK(wa_binder_init_ring(ring.data(), ring.size(), 50 + j) == WA_OK);
            for (uint32_t i = 0; i < 100; ++i) {
                int32_t status;
                do {
                    status = wa_binder_send(11, i, 0, 0, nullptr, 0);
                    if (status == WA_ERR_AGAIN) std::this_thread::yield();
                } while (status == WA_ERR_AGAIN);
                CHECK(status == WA_OK);
            }
        });
    }
    std::array<uint32_t, 3> next_txn{};
    for (uint32_t i = 0; i < 300; ++i) {
        CHECK(wa_binder_recv(&header, nullptr, 0, 1000) == WA_OK);
        CHECK(header.src_pid >= 50 && header.src_pid < 53 && header.dst_pid == 11);
        CHECK(header.txn_id == next_txn[header.src_pid - 50]++);
    }
}

uint64_t address(const void* pointer) {
    return reinterpret_cast<uintptr_t>(pointer);
}

void test_vfs_and_syscalls() {
    char path[] = "./wa-m0-vfs-XXXXXX";
    const int original = mkstemp(path);
    CHECK(original >= 0 && close(original) == 0 && unlink(path) == 0);
    CHECK(wa_vfs_open(path, WA_O_RDONLY, 0) == WA_ERR_NOENT);
    CHECK(wa_syscall_set_arch(WA_ARCH_ARM64) == WA_OK);
    const int32_t fd = wa_syscall(56, -100, address(path), WA_O_CREAT | WA_O_RDWR, 0600, 0, 0);
    CHECK(fd >= 0);
    constexpr char payload[] = "wasmandroid";
    CHECK(wa_syscall(68, fd, address(payload), sizeof(payload), 5, 0, 0) == sizeof(payload));
    CHECK(wa_vfs_flock(fd, 2 | 4) == WA_OK);
    const int other = wa_vfs_open(path, WA_O_RDWR, 0);
    CHECK(other >= 0 && wa_vfs_flock(other, 2 | 4) == WA_ERR_AGAIN);
    CHECK(wa_vfs_flock(fd, 8) == WA_OK && wa_vfs_flock(other, 2 | 4) == WA_OK);
    CHECK(wa_vfs_close(other) == WA_OK);
    CHECK(wa_vfs_ftruncate(fd, 24) == WA_OK && wa_vfs_fsync(fd) == WA_OK);
    wa_stat_t st{};
    CHECK(wa_vfs_fstat(fd, &st) == WA_OK && st.size == 24 && st.mtime_ns > 0);
    CHECK(wa_syscall(172, 0, 0, 0, 0, 0, 0) == getpid());
    CHECK(wa_syscall(178, 0, 0, 0, 0, 0, 0) > 0);
    const int ep = wa_syscall(20, 0, 0, 0, 0, 0, 0);
    CHECK(ep >= 0 && wa_syscall(57, ep, 0, 0, 0, 0, 0) == WA_OK);
    int64_t time64[2]{};
    CHECK(wa_syscall(113, 1, address(time64), 0, 0, 0, 0) == WA_OK);
    CHECK(time64[0] > 0 && time64[1] >= 0 && time64[1] < 1'000'000'000);

    CHECK(wa_syscall_set_arch(WA_ARCH_ARM32) == WA_OK);
    CHECK(wa_syscall(20, 0, 0, 0, 0, 0, 0) == getpid());
    CHECK(wa_syscall(224, 0, 0, 0, 0, 0, 0) > 0);
    char readback[sizeof(payload)]{};
    CHECK(wa_syscall(180, fd, address(readback), sizeof(readback), 0, 5, 0) == sizeof(readback));
    CHECK(std::memcmp(payload, readback, sizeof(payload)) == 0);
    CHECK(wa_syscall(181, fd, address(payload), 1, 0, 23, 0) == 1);
    CHECK(wa_syscall(6, fd, 0, 0, 0, 0, 0) == WA_OK);
    const int readfd = wa_syscall(322, -100, address(path), WA_O_RDONLY, 0, 0, 0);
    CHECK(readfd >= 0);
    char file[24]{};
    CHECK(wa_syscall(3, readfd, address(file), sizeof(file), 0, 0, 0) == sizeof(file));
    CHECK(std::memcmp(file + 5, payload, sizeof(payload)) == 0 && file[23] == payload[0]);
    CHECK(wa_syscall(6, readfd, 0, 0, 0, 0, 0) == WA_OK && unlink(path) == 0);
    int32_t time32[2]{};
    CHECK(wa_syscall(263, 0, address(time32), 0, 0, 0, 0) == WA_OK);
    CHECK(time32[0] > 0 && time32[1] >= 0 && time32[1] < 1'000'000'000);
    uint32_t futex = 1;
    CHECK(wa_syscall(240, address(&futex), 128, 0, 0, 0, 0) == WA_ERR_AGAIN);
    int32_t pipe[2];
    CHECK(wa_syscall(359, address(pipe), 0x800, 0, 0, 0, 0) == WA_OK);
    CHECK(wa_syscall(3, pipe[0], address(file), 1, 0, 0, 0) == WA_ERR_AGAIN);
    CHECK(wa_syscall(4, pipe[1], address(payload), sizeof(payload), 0, 0, 0) == sizeof(payload));
    CHECK(wa_vfs_close(pipe[0]) == WA_OK && wa_vfs_close(pipe[1]) == WA_OK);
    CHECK(wa_syscall(0xffff, 0, 0, 0, 0, 0, 0) == WA_ERR_NOSYS);
}

void test_jit_registry() {
    constexpr size_t capacity = 8;
    alignas(8) std::array<std::byte, sizeof(wa_jit_registry_header_t) +
                                        capacity * sizeof(wa_jit_slot_entry_t)> region{};
    wa_jit_slot_entry_t found{};
    std::latch starting(4);
    std::array<std::jthread, 4> workers;
    for (uint32_t i = 0; i < workers.size(); ++i) {
        workers[i] = std::jthread([&, i] {
            starting.arrive_and_wait();
            CHECK(wa_jit_registry_init(region.data(), region.size()) == WA_OK);
            CHECK(wa_jit_registry_reserve(region.data(), 0, 42) == 0);
            const auto slot = wa_jit_registry_reserve(region.data(), i + 10, 42);
            CHECK(slot > 0);
            CHECK(wa_jit_registry_publish(region.data(), slot, 7, i) == WA_OK);
        });
    }
    for (auto& worker : workers) worker.join();
    const auto& header = *reinterpret_cast<wa_jit_registry_header_t*>(region.data());
    CHECK(header.magic == WA_JIT_REGISTRY_MAGIC && header.version == WA_ABI_VERSION);
    CHECK(header.capacity == capacity && header.next_slot == 5 && header.published_chunks == 1);
    CHECK(wa_jit_registry_lookup(region.data(), 0, &found) == 0);
    CHECK(found.state == WA_JIT_SLOT_QUEUED && found.code_hash == 42 && found.table_slot == 0);
    CHECK(wa_jit_registry_publish(region.data(), 0, 8, 9) == WA_OK);
    CHECK(wa_jit_registry_publish(region.data(), 0, 8, 9) == WA_OK && header.published_chunks == 2);
    CHECK(wa_jit_registry_publish(region.data(), 0, 9, 9) == WA_ERR_EXIST);
    CHECK(wa_jit_registry_lookup(region.data(), 0, &found) == 0);
    CHECK(found.state == WA_JIT_SLOT_READY && found.chunk_id == 8 && found.export_idx == 9);
    CHECK(wa_jit_registry_lookup(region.data(), 42, &found) == WA_ERR_NOENT);
    CHECK(wa_jit_registry_reserve(region.data(), 0, 41) == WA_ERR_EXIST);
    CHECK(wa_jit_registry_reserve(region.data(), 21, 42) == 5);
    CHECK(wa_jit_registry_reserve(region.data(), 22, 42) == 6);
    CHECK(wa_jit_registry_reserve(region.data(), 23, 42) == 7);
    CHECK(wa_jit_registry_reserve(region.data(), 24, 42) == WA_ERR_NOMEM);
}

}  // namespace

int main() {
    test_futex();
    test_epoll();
    test_binder();
    test_vfs_and_syscalls();
    test_jit_registry();
    std::puts("PASS futex, epoll/pipe/eventfd, Binder, VFS/syscalls, JIT registry");
}
