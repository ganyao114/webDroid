#include "binder_fixture.h"
#include "platform.h"
#include "wasmandroid/libkern.h"

#include <cstring>

namespace {

using namespace m2;

std::unique_ptr<BinderProcess> process;
std::shared_ptr<Calc> calc;
std::shared_ptr<Callback> callback;
std::shared_ptr<BpBinder> remote;
std::array<BinderTransaction, kCalls> pending;
uint32_t marker;

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

extern "C" int32_t m2_init(wa_pid_t pid) {
    if (process || pid < 1 || pid > 3) {
        return WA_ERR_INVALID;
    }
    process = std::make_unique<BinderProcess>(pid, nullptr, kRingBytes);
    const int32_t status = process->attachThread();
    if (status != WA_OK || pid != WA_SERVICEMANAGER_PID) {
        return status;
    }
    return ServiceManager::publish(*process);
}

extern "C" void m2_set_marker(uint32_t value) {
    marker = value;
}
extern "C" uint32_t m2_get_marker() {
    return marker;
}

extern "C" int32_t m2_manager_run() {
    if (process->pid() != WA_SERVICEMANAGER_PID) {
        return WA_ERR_INVALID;
    }
    for (int32_t i = 0; i < kManagerCalls; ++i) {
        if (const int32_t status = process->pollOnce(5000); status != WA_OK) {
            return status;
        }
    }
    return kManagerCalls;
}

extern "C" int32_t m2_service_register() {
    if (process->pid() != 2) {
        return WA_ERR_INVALID;
    }
    calc = std::make_shared<Calc>();
    const int32_t status = process->registerBinder(calc);
    return status == WA_OK ? defaultServiceManager()->addService(kServiceName, calc->handle())
                           : status;
}

extern "C" int32_t m2_service_run() {
    if (!calc) {
        return WA_ERR_INVALID;
    }
    while (calc->calls() < kCalls + 1) {
        if (const int32_t status = process->pollOnce(5000); status != WA_OK) {
            return status;
        }
    }
    return calc->calls() - 1;
}

extern "C" int32_t m2_client_queue() {
    if (process->pid() != 3) {
        return WA_ERR_INVALID;
    }
    remote = lookupService();
    if (!remote) {
        return WA_ERR_NOENT;
    }
    callback = std::make_shared<Callback>();
    if (const int32_t status = process->registerBinder(callback); status != WA_OK) {
        return status;
    }
    for (int32_t index = 0; index < kCalls; ++index) {
        Parcel request;
        if (!makeRequest(request, index, callback)) {
            return WA_ERR_NOMEM;
        }
        const int32_t status = remote->beginTransact(kCalculate, request, pending[index]);
        if (status != WA_OK) {
            return status;
        }
    }
    return kCalls;
}

extern "C" int32_t m2_client_finish() {
    if (!remote || !callback) {
        return WA_ERR_INVALID;
    }
    // Wait out of submission order, so every earlier reply must be routed
    // while waiting for the last call.
    for (int32_t index = kCalls - 1; index >= 0; --index) {
        Parcel reply;
        if (const int32_t status = pending[index].wait(&reply); status != WA_OK) {
            return status;
        }
        if (!exactReply(reply, index, callback)) {
            return WA_ERR_INTERNAL;
        }
    }
    return doSyncCall(*remote, callback) && callback->complete() ? kCalls : WA_ERR_INTERNAL;
}

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
