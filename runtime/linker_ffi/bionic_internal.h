#pragma once

#include "dbt_internal.h"
#include "guest_abi.h"

#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <functional>
#include <optional>
#include <set>
#include <tuple>

namespace wa::softmmu {
class CpuAccess;
struct Iotlb;
}

namespace wa::dbt {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
inline constexpr auto kNoDeadline = Deadline::max();
inline constexpr int32_t kThreadParked = 1, kThreadYielded = 2, kThreadExited = 3,
                         kThreadJumped = 4;
inline constexpr uint64_t kTlsCapacity = 256 * 1024;

// A blocked trap keeps its PC and registers in GuestCpu. After wakeup the
// dispatcher calls the same shim, which can recheck its condition.
enum class WaitKind {
    None,
    Join,
    Mutex,
    Cond,
    Rwlock,
    Once,
    Guard,
    Sem,
    Sleep,
    Barrier,
    File,
    Futex,
    Signal,
    Io,
    EventRead,
    EventWrite
};
struct GuestWait {
    WaitKind kind = WaitKind::None;
    uint64_t address = 0;
    Deadline deadline = kNoDeadline;
    bool ready = false;
    int error = 0;
};
struct CondWait {
    uint64_t condition = 0, mutex = 0;
    int result = 0;
    bool reacquiring = false;
};

struct GuestCpu;
struct GuestSignalStack {
    uint64_t address = 0, size = 0;
    uint32_t flags = 2; // Linux SS_DISABLE.
};

struct GuestThread {
    uint64_t id = 1;
    uint64_t stack_base = 0, stack_size = 1 << 20, active_sp = 0, steps_left = 0;
    bool owns_stack = true;
    uint64_t errno_address = 0, thread_pointer = 0, result = 0, joiner = 0;
    uint64_t host_entry = 0, host_errno_address = 0;
    uint64_t native_looper = 0;
    uint64_t locale = 0, signal_mask = 0, pending_signals = 0;
    GuestSignalStack signal_stack;
    int policy = 0, priority = 0, nice = 0, egl_error = 0x3000, gl_error = 0;
    bool dumpable = true;
    std::string name = "guest";
    uint32_t mbstate = 0;
    unsigned depth = 0;
    int32_t status = WA_OK;
    bool detached = false, exited = false, running = false, resumable = false;
    Deadline io_deadline{};
    GuestWait wait;
    CondWait cond_wait;
    std::map<uint32_t, uint64_t> tls_keys;
    std::map<uint64_t, uint64_t> emutls;
    std::set<uint64_t> initialized_tls;
    std::vector<ExitHandler> thread_exit_handlers;
    std::vector<std::pair<uint64_t, ExitHandler>> cleanup_handlers;
    uint64_t jump_buffer = 0;
    uint32_t jump_value = 0;
    bool jump_save_mask = false;
    std::shared_ptr<GuestCpu> cpu;
};

struct GuestMutex {
    uint64_t owner = 0;
    unsigned depth = 0;
};
struct GuestRwlock {
    uint64_t writer = 0;
    std::map<uint64_t, unsigned> readers;
};
struct GuestSemaphore {
    unsigned count = 0;
};
struct GuestBarrier {
    uint32_t count;
    uint32_t arrived = 0;
};
struct GuestEvent {
    int notification = -1;
    uint64_t id = 0, count = 0;
    bool semaphore = false, nonblocking = false;
    int timer_clock = -1;
    std::chrono::nanoseconds timer_due{}, timer_interval{};
    ~GuestEvent();
};
struct GuestEpoll {
    struct Interest {
        uint32_t events;
        uint64_t data;
        uint64_t revision;
        bool enabled = true;
    };
    std::map<int, Interest> interests;
    uint64_t revision = 0;
    int last_ready = -1;
};
struct GuestDescriptor {
    int host;
    bool owned;
    std::shared_ptr<GuestEvent> event;
    std::shared_ptr<GuestEpoll> epoll{};
};
struct GuestFile {
    FILE* host;
    bool owned;
    int descriptor;
    uint64_t owner = 0;
    unsigned lock_count = 0;
};
struct GuestDirectory {
    DIR* host;
    int descriptor;
    uint64_t entry = 0;
};
struct GuestMountedFile {
    uint64_t size = 0;
    bool directory = false;
    std::function<int64_t(void*, size_t, uint64_t)> read;
};

struct SharedFilePage;
struct GuestFileTree;
struct NativeLooper;
struct PosixRegex;
struct GuestMappingPage {
    uint8_t prot;
    bool can_write = true;
    std::shared_ptr<SharedFilePage> shared;
};

struct BionicState {
    Deadline started = Clock::now();
    GuestThread main;
    std::map<uint64_t, GuestThread> threads;
    uint64_t next_thread = 2, last_scheduled = 1, next_event = 1;
    std::map<uint32_t, uint64_t> keys;
    std::map<uint64_t, GuestMutex> mutexes;
    std::map<uint64_t, GuestRwlock> rwlocks;
    std::map<uint64_t, GuestSemaphore> semaphores;
    std::map<uint64_t, GuestBarrier> barriers;
    std::map<uint64_t, uint64_t> once_owners;
    std::map<int, GuestDescriptor> descriptors{
        {0, {0, false, {}}}, {1, {1, false, {}}}, {2, {2, false, {}}}};
    std::map<uint64_t, GuestFile> files;
    std::map<uint64_t, uint64_t> map_allocations;
    std::map<uint64_t, GuestMappingPage> map_pages;
    std::map<std::tuple<uint64_t, uint64_t, uint64_t>, std::weak_ptr<SharedFilePage>> map_file_pages;
    std::map<uint64_t, GuestDirectory> directories;
    std::map<uint64_t, std::shared_ptr<GuestFileTree>> file_trees;
    std::map<uint64_t, std::shared_ptr<NativeLooper>> loopers;
    std::map<uint64_t, std::shared_ptr<PosixRegex>> regexes;
    std::map<std::string, uint64_t, std::less<>> strings;
    std::map<std::string, uint64_t, std::less<>> environment;
    std::function<std::optional<GuestMountedFile>(std::string_view)> find_guest_file;
    std::function<std::optional<std::string_view>(std::string_view)> read_system_property;
    std::map<int, std::array<uint64_t, 3>> signals;
    std::set<uint64_t> locales;
    uint64_t stdio = 0, environ = 0, environ_array = 0, localeconv = 0, setlocale = 0;
    uint64_t timezone = 0, daylight = 0, tzname = 0;
    uint64_t strtok_save = 0, tm = 0, ctime = 0, passwd = 0, servent = 0, tmpnam = 0;
    std::string abort_message;
    std::string log_tag;
    std::string cwd;
    bool finalizing_process = false;
    int log_mask = 0xff;
    uint32_t random = 1;
    ~BionicState();
    void FlushSharedMappings() noexcept;
};

struct GuestBuffer {
    wa_dbt& dbt;
    uint64_t address;
    explicit GuestBuffer(wa_dbt& dbt, size_t size) : dbt(dbt), address(AllocateGuest(dbt, size)) {
        if (!address)
            throw GuestFault{WA_ERR_NOMEM};
    }
    ~GuestBuffer() { FreeGuest(dbt, address); }
    GuestBuffer(const GuestBuffer&) = delete;
    GuestBuffer& operator=(const GuestBuffer&) = delete;
};

struct Builtin {
    const char* name;
    wa_host_symbol_fn function;
};

#ifdef __EMSCRIPTEN__
// A declined fast call must leave guest state unchanged. It can then run as a
// normal host thunk, outside the MMU lock, with the full host call context.
struct FastHostCall {
    wa_dbt& dbt;
    softmmu::CpuAccess& memory;
    softmmu::Iotlb& tlb;
    const uint64_t* args;
    uint64_t result = 0;

    const uint8_t* ResidentRead(uint64_t address, uint64_t size) const noexcept;
    uint8_t* ResidentWrite(uint64_t address, uint64_t size) const noexcept;
    bool ReadU32(uint64_t address, uint32_t& word) const noexcept;
};

void RegisterFastHost(wa_dbt& dbt, std::string_view name, FastHostFn function);
#endif

template <void (*Function)(wa_host_call_ctx_t&)> int32_t Dispatch(wa_host_call_ctx_t* ctx, void*) {
    try {
        Function(*ctx);
        return WA_OK;
    } catch (const GuestFault& fault) {
        return fault.status;
    }
}

void Register(wa_dbt& dbt, std::string_view library, std::span<const Builtin> bindings);
void RegisterPthread(wa_dbt& dbt);
void RegisterStdio(wa_dbt& dbt);
void RegisterLocale(wa_dbt& dbt);
void RegisterPosix(wa_dbt& dbt);
void RegisterFiles(wa_dbt& dbt);
void RegisterMappings(wa_dbt& dbt);
void RegisterNetwork(wa_dbt& dbt);
void RegisterStrings(wa_dbt& dbt);
void RegisterMath(wa_dbt& dbt);
void RegisterDynamicLinker(wa_dbt& dbt);
void RegisterGraphics(wa_dbt& dbt);
void RegisterNativeLooper(wa_dbt& dbt);
void DropThreadLooper(wa_dbt& dbt);
void DropDescriptorFromLoopers(wa_dbt& dbt, int descriptor);
void RefreshTimers(wa_dbt& dbt);
void RegisterGuestJumps(wa_dbt& dbt);

int GuestErrno(int host_error);
int HostErrno(int guest_error);
uint64_t ErrnoAddress(wa_dbt& dbt);
void SetErrno(wa_dbt& dbt, int guest_error);
void PosixResult(wa_host_call_ctx_t& ctx, int64_t result);
int HostFd(const wa_dbt& dbt, uint64_t descriptor);
std::optional<GuestMountedFile> FindMountedFile(const wa_dbt& dbt, std::string_view path);
int OpenMountedFile(const GuestMountedFile& file, int flags);
int AdoptFd(wa_dbt& dbt, int host, bool owned = true, int minimum = 0);
int CloseFd(wa_dbt& dbt, int descriptor);
void StorePipe(wa_host_call_ctx_t& ctx, int read_end, int write_end, uint64_t destination);
std::shared_ptr<GuestEvent> EventFor(const wa_dbt& dbt, int descriptor);
std::shared_ptr<GuestEpoll> EpollFor(const wa_dbt& dbt, int descriptor);
void RemoveEpollInterests(wa_dbt& dbt, int descriptor);
void Fail(wa_host_call_ctx_t& ctx, int host_error);
std::string GuestPath(const wa_host_call_ctx_t& ctx, uint64_t address);
uint64_t CopyString(wa_dbt& dbt, std::string_view text);
uint64_t InternString(wa_dbt& dbt, std::string_view text);

GuestThread* FindThread(wa_dbt& dbt, uint64_t id);
int32_t StartThread(wa_dbt& dbt, GuestThread& thread, uint64_t routine, uint64_t argument);
void ReleaseThread(wa_dbt& dbt, GuestThread& thread);
bool RunReadyWorker(wa_dbt& dbt);
void Wake(wa_dbt& dbt, WaitKind kind, uint64_t address, bool all = true);
int Park(wa_dbt& dbt, WaitKind kind, uint64_t address, Deadline deadline = kNoDeadline);
Deadline GuestDeadline(const GuestMemory& mem, uint64_t address, bool monotonic);
void Yield(wa_dbt& dbt);
uint64_t ThreadPointer(wa_dbt& dbt);
uint64_t ThreadTls(wa_dbt& dbt, const LoadedImage& image);
void InitializeThreadTls(wa_dbt& dbt, GuestThread& thread, const LoadedImage& image);
const LoadedImage* ImageContaining(const wa_dbt& dbt, uint64_t address);
void WriteGuestTm(wa_dbt& dbt, uint64_t address, const struct tm& value);
struct tm ReadGuestTm(const GuestMemory& memory, uint64_t address);

// Use the guest's word size when decoding C long/time_t on ARMv7.
inline int64_t SignedWord(const GuestMemory& mem, uint64_t value) {
    return mem.width() == 4 ? static_cast<int32_t>(value) : static_cast<int64_t>(value);
}

} // namespace wa::dbt
