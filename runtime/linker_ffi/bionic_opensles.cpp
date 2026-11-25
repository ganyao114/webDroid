#include "bionic_internal.h"
#include "softmmu_internal.h"

#include "../android_system/guest_audio.h"

#include <array>
#include <climits>
#include <cmath>
#include <deque>

namespace wa::dbt {
namespace {

constexpr auto kLibrary = "libOpenSLES.so";
enum Result : uint32_t {
    Success, Preconditions, Invalid, NoMemory, ResourceError, ResourceLost, IoError,
    BufferInsufficient, ContentCorrupted, ContentUnsupported, ContentNotFound, PermissionDenied,
    Unsupported,
};
enum class Interface : size_t {
    Object, Engine, OutputMix, Play, Volume, BufferQueue, SimpleBufferQueue, Configuration, Count,
};
enum class Kind : uint32_t { Engine = 0x1001, Player = 0x1004, Mix = 0x1009 };
enum PlayState : uint32_t { Stopped = 1, Paused, Playing };
constexpr uint32_t kUnknownTime = UINT32_MAX;
constexpr uint32_t kBufferQueueLocator = 6, kSimpleBufferQueueLocator = 0x800007bd, kMixLocator = 4;
constexpr uint32_t kPcm = 2, kPcmEx = 4, kLittleEndian = 2;
constexpr size_t kInterfaces = static_cast<size_t>(Interface::Count);

struct Uuid {
    uint32_t low;
    uint16_t mid, high, sequence;
    uint8_t node[6]{0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b};
    bool operator==(const Uuid&) const = default;
};
static_assert(sizeof(Uuid) == 16);

// Wilhelm's OpenSLES_IID.cpp. Each dynamic symbol holds a guest pointer to its UUID.
constexpr std::array<std::pair<const char*, Uuid>, kInterfaces> kIids{{
    {"SL_IID_OBJECT", {0x79216360, 0xddd7, 0x11db, 0xac16}},
    {"SL_IID_ENGINE", {0x8d97c260, 0xddd4, 0x11db, 0x958f}},
    {"SL_IID_OUTPUTMIX", {0x97750f60, 0xddd7, 0x11db, 0x92b1}},
    {"SL_IID_PLAY", {0xef0bd9c0, 0xddd7, 0x11db, 0xbf49}},
    {"SL_IID_VOLUME", {0x09e8ede0, 0xddde, 0x11db, 0xb4f6}},
    {"SL_IID_BUFFERQUEUE", {0x2bc99cc0, 0xddd4, 0x11db, 0x8d99}},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", {0x198e4940, 0xc5d7, 0x11df, 0xa2a6}},
    {"SL_IID_ANDROIDCONFIGURATION", {0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c}},
}};

constexpr auto Index(Interface type) { return static_cast<size_t>(type); }
constexpr auto Mask(Interface type) { return 1u << Index(type); }
constexpr uint32_t kQueues = Mask(Interface::BufferQueue) | Mask(Interface::SimpleBufferQueue);

constexpr uint32_t Interfaces(Kind kind) {
    switch (kind) {
    case Kind::Engine: return Mask(Interface::Object) | Mask(Interface::Engine);
    case Kind::Mix: return Mask(Interface::Object) | Mask(Interface::OutputMix);
    case Kind::Player: return Mask(Interface::Object) | Mask(Interface::Play) |
        Mask(Interface::Volume) | kQueues | Mask(Interface::Configuration);
    }
    return 0;
}

struct Callback {
    uint64_t function = 0, caller = 0, context = 0;
};

struct Buffer {
    uint64_t address;
    uint32_t size;
    uint32_t copied = 0;
    uint64_t end_frame = 0;
};

struct Player {
    audio::PcmStream stream;
    std::deque<Buffer> queue;
    Callback callback;
    uint32_t capacity, play_state = Stopped, completion_index = 0;
    uint32_t stream_type = 3, performance_mode = 3;
    uint32_t marker = kUnknownTime, position_period = 1000;
    int16_t level = 0, pan = 0;
    bool mute = false, stereo_position = false, worker = false;

    void ApplyVolume() {
        const auto gain = mute ? 0.f : std::pow(10.f, level / 2000.f);
        const auto balance = stereo_position ? pan / 1000.f : 0.f;
        stream.SetVolume(gain * (1 - std::max(0.f, balance)), gain * (1 + std::min(0.f, balance)));
    }
};

struct Object {
    Kind kind;
    uint32_t interfaces;
    uint64_t engine = 0;
    bool realized = false, destroyed = false;
    int32_t priority = 0;
    bool preemptable = false;
    std::optional<Player> player;
};

struct OpenSl {
    explicit OpenSl(wa_dbt& dbt) : dbt(dbt), memory(dbt.mmu, dbt.arch) {}

    wa_dbt& dbt;
    GuestMemory memory;
    std::array<uint64_t, kInterfaces> tables{};
    std::map<uint64_t, Object> objects;
    uint64_t next_buffer_stub = 0, runner = 0;
    std::shared_ptr<audio::WorkerPump> pump;

    uint64_t Handle(uint64_t object, Interface type) const { return object + Index(type) * memory.width(); }
    uint64_t WorkerArg(uint64_t object) const { return object + kInterfaces * memory.width(); }

    uint32_t InterfaceMask(const GuestMemory& mem, uint64_t pointer) const {
        if (!pointer) return 0;
        const auto uuid = mem.Read<Uuid>(pointer);
        for (size_t i = 0; i < kIids.size(); ++i)
            if (kIids[i].second == uuid) return 1u << i;
        return 0;
    }

    bool RequiredSupported(const GuestMemory& mem, uint32_t count, uint64_t ids, uint64_t required,
                           uint32_t capabilities) const {
        if (count > 64 || (count && (!ids || !required))) return false;
        for (uint32_t i = 0; i < count; ++i) {
            if (mem.Read<uint32_t>(required + i * sizeof(uint32_t)) &&
                !(InterfaceMask(mem, mem.ReadWord(ids + i * mem.width())) & capabilities)) return false;
        }
        return true;
    }

    Result Create(const GuestMemory& mem, uint64_t output, Object object) {
        if (!output) return Invalid;
        const auto address = AllocateGuest(dbt, (kInterfaces + 1) * mem.width());
        if (!address) return NoMemory;
        try {
            for (size_t i = 0; i < kInterfaces; ++i) {
                if (object.interfaces & (1u << i)) memory.WriteWord(address + i * mem.width(), tables[i]);
            }
            memory.WriteWord(WorkerArg(address), next_buffer_stub);
            objects.emplace(address, std::move(object));
            mem.WriteWord(output, address);
        } catch (...) {
            Destroy(address);
            throw;
        }
        return Success;
    }

    void Destroy(uint64_t address) {
        objects.erase(address);
        FreeGuest(dbt, address);
    }
};

std::mutex owners_mutex;
std::map<BionicState*, std::unique_ptr<OpenSl>> owners;

Result CreateEngine(wa_host_call_ctx_t& ctx, OpenSl& sl) {
    const GuestMemory mem(ctx);
    const auto output = ctx.args[0], option_count = ctx.args[1], options = ctx.args[2];
    if (!output || (option_count && !options)) return Invalid;
    mem.WriteWord(output, 0);
    const auto interfaces = Interfaces(Kind::Engine);
    if (!sl.RequiredSupported(mem, ctx.args[3], ctx.args[4], ctx.args[5], interfaces)) return Unsupported;
    return sl.Create(mem, output, {Kind::Engine, interfaces});
}

Result CreateMix(wa_host_call_ctx_t& ctx, OpenSl& sl, uint64_t engine) {
    const GuestMemory mem(ctx);
    const auto output = ctx.args[1];
    if (!output) return Invalid;
    mem.WriteWord(output, 0);
    const auto interfaces = Interfaces(Kind::Mix);
    if (!sl.RequiredSupported(mem, ctx.args[2], ctx.args[3], ctx.args[4], interfaces)) return Unsupported;
    return sl.Create(mem, output, {Kind::Mix, interfaces, engine});
}

Result CreatePlayer(wa_host_call_ctx_t& ctx, OpenSl& sl, uint64_t engine) {
    const GuestMemory mem(ctx);
    const auto output = ctx.args[1];
    if (!output) return Invalid;
    mem.WriteWord(output, 0);
    if (!ctx.args[2] || !ctx.args[3]) return Invalid;
    const auto source = mem.ReadWord(ctx.args[2]), format = mem.ReadWord(ctx.args[2] + mem.width());
    const auto sink = mem.ReadWord(ctx.args[3]);
    if (!source || !sink || !format) return Invalid;
    const auto locator = mem.Read<uint32_t>(source);
    if ((locator != kBufferQueueLocator && locator != kSimpleBufferQueueLocator) ||
        mem.Read<uint32_t>(sink) != kMixLocator) return ContentUnsupported;
    const auto mix = sl.objects.find(mem.ReadWord(sink + mem.width()));
    if (mix == sl.objects.end() || mix->second.kind != Kind::Mix || mix->second.engine != engine ||
        !mix->second.realized) return Preconditions;

    const auto buffers = mem.Read<uint32_t>(source + 4);
    const auto pcm = mem.Read<std::array<uint32_t, 7>>(format);
    const auto [type, channels, millihertz, bits, container, channel_mask, endian] = pcm;
    if (!buffers || buffers > 255) return Invalid;
    if ((type != kPcm && type != kPcmEx) || millihertz % 1000 || bits != container ||
        endian != kLittleEndian) return ContentUnsupported;
    const auto representation = type == kPcmEx ? mem.Read<uint32_t>(format + sizeof(pcm)) : 0;
    audio::Encoding encoding{};
    if (bits == 16 && (type == kPcm || representation == 1)) encoding = audio::Encoding::S16;
    else if (bits == 8 && (type == kPcm || representation == 2)) encoding = audio::Encoding::U8;
    else if (bits == 32 && representation == 3) encoding = audio::Encoding::Float;
    const auto rate = millihertz / 1000;
    if (!audio::SupportsPcm(rate, channels, encoding)) return ContentUnsupported;
    const auto interfaces = Interfaces(Kind::Player);
    CallReader args(ctx);
    for (int i = 0; i < 4; ++i) args.Next();
    const auto count = args.Next(), ids = args.Next(), required = args.Next();
    if (!sl.RequiredSupported(mem, count, ids, required, interfaces)) return Unsupported;
    Object object{Kind::Player, interfaces, engine};
    object.player.emplace(Player{audio::PcmStream(audio::Backend::OpenSLES, rate, channels, encoding),
                                 {}, {}, buffers});
    return sl.Create(mem, output, std::move(object));
}

void StartQueueWorker(OpenSl& sl, uint64_t object) {
    // AAPCS64 and ARMv7 implementations of:
    // while (next(arg, &call)) if (call.function) call.function(call.caller, call.context);
    // The guest calls the callback itself, so the scheduler can suspend it on a guest mutex.
    static constexpr uint32_t arm64[] = {
        0xa9bd7bf3, 0xaa0003f3,                         // save x19/lr, keep arg
        0xf9400270, 0xaa1303e0, 0x910043e1, 0xd63f0200, // call next(arg, sp + 16)
        0x340000c0, 0xa94103f0, 0xf94013e1, 0xb4ffff30, // exit or load callback arguments
        0xd63f0200, 0x17fffff7,                         // invoke callback and loop
        0xa8c37bf3, 0xd65f03c0,
    };
    static constexpr uint32_t arm[] = {
        0xe92d4010, 0xe24dd010, 0xe1a04000,             // save r4/lr and allocate callback record
        0xe594c000, 0xe1a00004, 0xe1a0100d, 0xe12fff3c, // call next(arg, sp)
        0xe3500000, 0x0a000005, 0xe59dc000, 0xe59d0004, 0xe59d1008,
        0xe35c0000, 0x112fff3c, 0xeafffff3,             // invoke a non-null callback and loop
        0xe28dd010, 0xe8bd8010,
    };
    auto& dbt = sl.dbt;
    if (!sl.runner) {
        const std::span<const uint32_t> code = dbt.arch == WA_ARCH_ARM64 ?
            std::span<const uint32_t>(arm64) : std::span<const uint32_t>(arm);
        const auto address = AllocateGuest(dbt, code.size_bytes(), WA_PAGE_SIZE,
            WA_PROT_READ | WA_PROT_WRITE | WA_PROT_EXEC);
        if (!address) throw GuestFault{WA_ERR_NOMEM};
        try {
            sl.memory.Write(address, code.data(), code.size_bytes());
            CheckGuest(softmmu::Protect(dbt.mmu, address, WA_PAGE_SIZE, WA_PROT_READ | WA_PROT_EXEC));
            sl.runner = address;
        } catch (...) {
            FreeGuest(dbt, address);
            throw;
        }
    }
    const auto id = dbt.bionic->next_thread++;
    auto [it, inserted] = dbt.bionic->threads.try_emplace(id);
    if (!inserted) throw GuestFault{WA_ERR_BUSY};
    auto& thread = it->second;
    thread.id = id;
    thread.name = "OpenSL ES";
    thread.detached = true;
    thread.stack_size = 256 * 1024;
    try {
        CheckGuest(StartThread(dbt, thread, sl.runner, sl.WorkerArg(object)));
    } catch (...) {
        ReleaseThread(dbt, thread);
        dbt.bionic->threads.erase(it);
        throw;
    }
}

int32_t NextBuffer(wa_host_call_ctx_t* ctx, void* data) {
    auto& sl = *static_cast<OpenSl*>(data);
    auto& dbt = sl.dbt;
    const GuestMemory mem(*ctx);
    const auto arg = ctx->args[0], result = ctx->args[1];
    const auto address = arg - kInterfaces * mem.width();
    try {
        // A parked host call restarts at the same PC. Acknowledge the previous wake first.
        if (dbt.running->wait.kind == WaitKind::Io) Park(dbt, WaitKind::Io, arg);
        while (true) {
            const auto found = sl.objects.find(address);
            if (found == sl.objects.end()) break;
            auto& object = found->second;
            if (object.destroyed) {
                sl.Destroy(address);
                break;
            }
            auto& player = *object.player;
            if (player.play_state == Playing && !player.queue.empty()) {
                for (auto& buffer : player.queue) {
                    std::array<uint8_t, 4096> pcm;
                    const auto frame_bytes = player.stream.FrameBytes();
                    while (buffer.copied < buffer.size) {
                        const auto bytes = std::min<uint32_t>(buffer.size - buffer.copied,
                                                              pcm.size() / frame_bytes * frame_bytes);
                        mem.Read(buffer.address + buffer.copied, pcm.data(), bytes);
                        const auto copied = player.stream.Write({pcm.data(), bytes}) * frame_bytes;
                        buffer.copied += copied;
                        if (copied < bytes) break;
                    }
                    if (buffer.copied < buffer.size) break;
                    if (!buffer.end_frame) buffer.end_frame = player.stream.Written();
                }
                const auto end = player.queue.front().end_frame;
                if (end && player.stream.Position() >= end) {
                    player.queue.pop_front();
                    ++player.completion_index;
                    const auto& callback = player.callback;
                    mem.WriteWord(result, callback.function);
                    mem.WriteWord(result + mem.width(), callback.caller);
                    mem.WriteWord(result + 2 * mem.width(), callback.context);
                    ctx->ret0 = 1;
                    return WA_OK;
                }
                Park(dbt, WaitKind::Io, arg, Clock::now() + std::chrono::milliseconds(4));
            } else {
                Park(dbt, WaitKind::Io, arg);
            }
        }
        ctx->ret0 = 0;
        return WA_OK;
    } catch (const GuestFault& fault) {
        return fault.status;
    }
}

using Handler = Result (*)(wa_host_call_ctx_t&, OpenSl&, uint64_t, Object&);
using Entry = wa_host_symbol_fn;

template <Interface Type, Handler Function>
int32_t Method(wa_host_call_ctx_t* ctx, void* data) {
    auto& sl = *static_cast<OpenSl*>(data);
    const auto address = ctx->args[0] - Index(Type) * sl.memory.width();
    const auto found = sl.objects.find(address);
    ctx->ret0 = Invalid;
    if (found == sl.objects.end() || found->second.destroyed ||
        !(found->second.interfaces & Mask(Type))) return WA_OK;
    try {
        ctx->ret0 = Function(*ctx, sl, address, found->second);
    } catch (const GuestFault& fault) {
        ctx->ret0 = fault.status == WA_ERR_NOMEM ? NoMemory : Invalid;
    } catch (const std::bad_alloc&) {
        ctx->ret0 = NoMemory;
    }
    return WA_OK;
}

int32_t CreateEngineEntry(wa_host_call_ctx_t* ctx, void* data) {
    try {
        ctx->ret0 = CreateEngine(*ctx, *static_cast<OpenSl*>(data));
    } catch (const GuestFault& fault) {
        ctx->ret0 = fault.status == WA_ERR_NOMEM ? NoMemory : Invalid;
    } catch (const std::bad_alloc&) {
        ctx->ret0 = NoMemory;
    }
    return WA_OK;
}

template <class T>
Result Out(const wa_host_call_ctx_t& ctx, T value, size_t arg = 1) {
    if (!ctx.args[arg]) return Invalid;
    GuestMemory(ctx).Write<T>(ctx.args[arg], value);
    return Success;
}

int32_t Unavailable(wa_host_call_ctx_t* ctx, void*) {
    ctx->ret0 = Unsupported;
    return WA_OK;
}

Result Realize(wa_host_call_ctx_t& ctx, OpenSl&, uint64_t, Object& object) {
    if (object.realized) return Preconditions;
    if (ctx.args[1]) return Unsupported;
    object.realized = true;
    return Success;
}

Result ObjectState(wa_host_call_ctx_t& ctx, OpenSl&, uint64_t, Object& object) {
    return Out<uint32_t>(ctx, object.realized ? 2 : 1);
}

Result GetInterface(wa_host_call_ctx_t& ctx, OpenSl& sl, uint64_t address, Object& object) {
    const GuestMemory mem(ctx);
    if (!ctx.args[2]) return Invalid;
    mem.WriteWord(ctx.args[2], 0);
    const auto bit = sl.InterfaceMask(mem, ctx.args[1]);
    if (!(object.interfaces & bit)) return Unsupported;
    if (!object.realized && bit != Mask(Interface::Configuration)) return Preconditions;
    mem.WriteWord(ctx.args[2], sl.Handle(address, static_cast<Interface>(std::countr_zero(bit))));
    return Success;
}

Result Destroy(wa_host_call_ctx_t&, OpenSl& sl, uint64_t address, Object& object) {
    if (object.player && object.player->worker) {
        object.destroyed = true;
        Wake(sl.dbt, WaitKind::Io, sl.WorkerArg(address));
    } else {
        sl.Destroy(address);
    }
    return Success;
}

Result SetPriority(wa_host_call_ctx_t& ctx, OpenSl&, uint64_t, Object& object) {
    object.priority = static_cast<int32_t>(ctx.args[1]);
    object.preemptable = ctx.args[2] != 0;
    return Success;
}

Result GetPriority(wa_host_call_ctx_t& ctx, OpenSl&, uint64_t, Object& object) {
    if (!ctx.args[1] || !ctx.args[2]) return Invalid;
    Out(ctx, object.priority);
    return Out<uint32_t>(ctx, object.preemptable, 2);
}

Result SetPlay(wa_host_call_ctx_t& ctx, OpenSl& sl, uint64_t address, Object& object) {
    auto& player = *object.player;
    const auto state = static_cast<uint32_t>(ctx.args[1]);
    if (state < Stopped || state > Playing) return Invalid;
    if (state == Playing && !player.worker) {
        StartQueueWorker(sl, address);
        player.worker = true;
    }
    player.stream.SetPlaying(state == Playing);
    player.play_state = state;
    Wake(sl.dbt, WaitKind::Io, sl.WorkerArg(address));
    return Success;
}

Result Enqueue(wa_host_call_ctx_t& ctx, OpenSl& sl, uint64_t address, Object& object) {
    auto& player = *object.player;
    const auto bytes = static_cast<uint32_t>(ctx.args[2]);
    if (!ctx.args[1] || !bytes || bytes % player.stream.FrameBytes()) return Invalid;
    if (CheckMappedAccess(ctx.dbt, ctx.args[1], bytes, WA_PROT_READ) != WA_OK) return Invalid;
    if (player.queue.size() >= player.capacity) return BufferInsufficient;
    player.queue.push_back({ctx.args[1], bytes});
    Wake(sl.dbt, WaitKind::Io, sl.WorkerArg(address));
    return Success;
}

Result Clear(wa_host_call_ctx_t&, OpenSl& sl, uint64_t address, Object& object) {
    auto& player = *object.player;
    player.queue.clear();
    player.stream.Reset();
    player.completion_index = 0;
    Wake(sl.dbt, WaitKind::Io, sl.WorkerArg(address));
    return Success;
}

Result QueueState(wa_host_call_ctx_t& ctx, OpenSl&, uint64_t, Object& object) {
    const auto& player = *object.player;
    return Out(ctx, std::array{static_cast<uint32_t>(player.queue.size()), player.completion_index});
}

Result QueueCallback(wa_host_call_ctx_t& ctx, OpenSl&, uint64_t, Object& object) {
    if (ctx.args[1] && !IsExecutable(*ctx.dbt, ctx.args[1])) return Invalid;
    object.player->callback = {ctx.args[1], ctx.args[0], ctx.args[2]};
    return Success;
}

Result Position(wa_host_call_ctx_t& ctx, OpenSl&, uint64_t, Object& object) {
    auto& stream = object.player->stream;
    return Out<uint32_t>(ctx, stream.Position() * 1000 / stream.Rate());
}

template <auto Field, class T> Result GetPlayer(wa_host_call_ctx_t& ctx, OpenSl&, uint64_t, Object& object) {
    return Out<T>(ctx, (*object.player).*Field);
}

template <auto Field, class T, int Min, int Max, bool Volume = false>
Result SetPlayer(wa_host_call_ctx_t& ctx, OpenSl&, uint64_t, Object& object) {
    const auto value = static_cast<T>(ctx.args[1]);
    if (value < Min || value > Max) return Invalid;
    (*object.player).*Field = value;
    if constexpr (Volume) object.player->ApplyVolume();
    return Success;
}

Result ListOutput(wa_host_call_ctx_t& ctx, OpenSl&, uint64_t, Object&) {
    const GuestMemory mem(ctx);
    if (!ctx.args[1]) return Invalid;
    if (ctx.args[2] && mem.Read<int32_t>(ctx.args[1]) < 1) return BufferInsufficient;
    if (ctx.args[2]) Out<uint32_t>(ctx, 0xfffffffe, 2);
    return Out<int32_t>(ctx, 1);
}

template <bool Set> Result Configure(wa_host_call_ctx_t& ctx, OpenSl&, uint64_t, Object& object) {
    const GuestMemory mem(ctx);
    if (!ctx.args[1] || !ctx.args[2]) return Invalid;
    auto& player = *object.player;
    const auto key = mem.String(ctx.args[1], 128, true);
    auto* value = key == "androidPlaybackStreamType" ? &player.stream_type :
                  key == "androidPerformanceMode" ? &player.performance_mode : nullptr;
    if (!value) return Unsupported;
    if constexpr (Set) {
        if (object.realized) return Preconditions;
        if (ctx.args[3] != sizeof(*value)) return Invalid;
        *value = mem.Read<uint32_t>(ctx.args[2]);
    } else {
        const auto bytes = mem.Read<uint32_t>(ctx.args[2]);
        mem.Write<uint32_t>(ctx.args[2], sizeof(*value));
        if (ctx.args[3] && bytes < sizeof(*value)) return BufferInsufficient;
        if (ctx.args[3]) mem.Write<uint32_t>(ctx.args[3], *value);
    }
    return Success;
}

uint64_t RegisterEntry(OpenSl& sl, const char* name, Entry entry, const char* soname = "") {
    uint64_t address;
    CheckGuest(RegisterHost(sl.dbt, soname, name, entry, &sl, true, &address));
    return address;
}

template <Interface Type, size_t N>
void Table(OpenSl& sl, const std::array<Entry, N>& entries, uint64_t& next) {
    sl.tables[Index(Type)] = next;
    for (size_t i = 0; i < N; ++i) {
        const auto name = std::string(kIids[Index(Type)].first) + "." + std::to_string(i);
        sl.memory.WriteWord(next, RegisterEntry(sl, name.c_str(), entries[i]));
        next += sl.memory.width();
    }
}

template <Interface Type>
void QueueTable(OpenSl& sl, uint64_t& next) {
    Table<Type>(sl, std::array{Method<Type, Enqueue>, Method<Type, Clear>, Method<Type, QueueState>,
        Method<Type, QueueCallback>}, next);
}

void RegisterTables(OpenSl& sl, uint64_t next) {
    using enum Interface;
    // The slot order is part of the ABI in the NDK OpenSLES.h and OpenSLES_Android.h headers.
    Table<Object>(sl, std::array{Method<Object, Realize>, Unavailable, Method<Object, ObjectState>,
        Method<Object, GetInterface>, Unavailable, Unavailable, Method<Object, Destroy>,
        Method<Object, SetPriority>, Method<Object, GetPriority>, Unavailable}, next);
    Table<Engine>(sl, std::array{Unavailable, Unavailable,
        Method<Engine, [](auto& ctx, OpenSl& state, uint64_t address, auto&) {
            return CreatePlayer(ctx, state, address);
        }>, Unavailable, Unavailable, Unavailable, Unavailable,
        Method<Engine, [](auto& ctx, OpenSl& state, uint64_t address, auto&) {
            return CreateMix(ctx, state, address);
        }>, Unavailable, Unavailable, Unavailable, Unavailable, Unavailable, Unavailable,
        Unavailable}, next);
    Table<OutputMix>(sl, std::array{Method<OutputMix, ListOutput>, Unavailable, Unavailable}, next);
    Table<Play>(sl, std::array{Method<Play, SetPlay>, Method<Play, GetPlayer<&Player::play_state, uint32_t>>,
        Method<Play, [](auto& ctx, OpenSl&, uint64_t, auto&) { return Out(ctx, kUnknownTime); }>,
        Method<Play, Position>, Unavailable, Unavailable, Unavailable,
        Method<Play, SetPlayer<&Player::marker, uint32_t, 0, INT_MAX>>,
        Method<Play, [](auto&, OpenSl&, uint64_t, auto& object) {
            object.player->marker = kUnknownTime; return Success;
        }>, Method<Play, GetPlayer<&Player::marker, uint32_t>>,
        Method<Play, SetPlayer<&Player::position_period, uint32_t, 1, INT_MAX>>,
        Method<Play, GetPlayer<&Player::position_period, uint32_t>>}, next);
    Table<Volume>(sl, std::array{Method<Volume, SetPlayer<&Player::level, int16_t, INT16_MIN, 0, true>>,
        Method<Volume, GetPlayer<&Player::level, int16_t>>,
        Method<Volume, [](auto& ctx, OpenSl&, uint64_t, auto&) { return Out<int16_t>(ctx, 0); }>,
        Method<Volume, SetPlayer<&Player::mute, uint32_t, 0, 1, true>>,
        Method<Volume, GetPlayer<&Player::mute, uint32_t>>,
        Method<Volume, SetPlayer<&Player::stereo_position, uint32_t, 0, 1, true>>,
        Method<Volume, GetPlayer<&Player::stereo_position, uint32_t>>,
        Method<Volume, SetPlayer<&Player::pan, int16_t, -1000, 1000, true>>,
        Method<Volume, GetPlayer<&Player::pan, int16_t>>}, next);
    QueueTable<BufferQueue>(sl, next);
    QueueTable<SimpleBufferQueue>(sl, next);
    Table<Interface::Configuration>(sl,
        std::array{Method<Interface::Configuration, Configure<true>>,
                   Method<Interface::Configuration, Configure<false>>, Unavailable, Unavailable}, next);
}

} // namespace

void RegisterOpenSl(wa_dbt& dbt) {
    auto sl = std::make_unique<OpenSl>(dbt);
    const auto width = sl->memory.width();
    const auto page = AllocateGuest(dbt, WA_PAGE_SIZE);
    if (!page) throw GuestFault{WA_ERR_NOMEM};
    auto slot = page;
    auto uuid = page + kInterfaces * width;
    for (const auto& [name, id] : kIids) {
        sl->memory.WriteWord(slot, uuid);
        sl->memory.Write(uuid, id);
        CheckGuest(RegisterData(dbt, kLibrary, name, slot, true));
        slot += width;
        uuid += sizeof(Uuid);
    }
    RegisterTables(*sl, uuid);
    CheckGuest(softmmu::Protect(dbt.mmu, page, WA_PAGE_SIZE, WA_PROT_READ));
    RegisterEntry(*sl, "slCreateEngine", CreateEngineEntry, kLibrary);
    sl->next_buffer_stub = RegisterEntry(*sl, "OpenSL.nextBuffer", NextBuffer);
    sl->pump = std::make_shared<audio::WorkerPump>([&dbt, owner = sl.get()](bool track_playing) {
        std::lock_guard lock(dbt.mutex);
        return (track_playing || !owner->objects.empty()) && RunReadyWorker(dbt);
    });
    audio::RegisterWorkerPump(sl->pump);
    std::lock_guard lock(owners_mutex);
    owners.emplace(dbt.bionic.get(), std::move(sl));
}

void DestroyOpenSl(BionicState& bionic) {
    std::lock_guard lock(owners_mutex);
    owners.erase(&bionic);
}

} // namespace wa::dbt
