#include "generated/jni_vtable_thunks.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>

#if __has_include(<sys/epoll.h>)
#include <sys/epoll.h>
#define WA_TEST_NATIVE_EPOLL 1
#endif

using namespace wa::ffi;

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                   \
            std::exit(EXIT_FAILURE);                                                               \
        }                                                                                          \
    } while (false)

namespace {

static_assert(sizeof(HostIovec) == sizeof(iovec) && alignof(HostIovec) == alignof(iovec));
static_assert(offsetof(HostIovec, iov_base) == offsetof(iovec, iov_base));
static_assert(offsetof(HostIovec, iov_len) == offsetof(iovec, iov_len));

constexpr uint64_t kBase = 0x10000;
constexpr uint64_t kGuard = kBase + 4 * WA_PAGE_SIZE;
constexpr uint64_t kWidePointer = UINT64_C(0xfedcba9876543210);
using Mmu = std::unique_ptr<wa_softmmu_t, decltype(&wa_softmmu_destroy)>;

Mmu make_memory(wa_page_read_fn reader = nullptr, void* user = nullptr) {
    Mmu mmu(wa_softmmu_create(reader, user), wa_softmmu_destroy);
    CHECK(mmu);
    CHECK(wa_softmmu_map_anon(mmu.get(), kBase, 4 * WA_PAGE_SIZE, WA_PROT_READ | WA_PROT_WRITE) ==
          WA_OK);
    CHECK(wa_softmmu_map_guard(mmu.get(), kGuard, WA_PAGE_SIZE) == WA_OK);
    return mmu;
}

template <size_t Size> auto guest_bytes(wa_softmmu_t* mmu, uint64_t address) {
    std::array<uint8_t, Size> bytes;
    CHECK(wa_softmmu_read(mmu, address, bytes.data(), bytes.size()) == WA_OK);
    return bytes;
}

template <size_t Size>
uint64_t little_word(const std::array<uint8_t, Size>& bytes, size_t offset, size_t width) {
    CHECK(width <= 8 && offset + width <= Size);
    uint64_t result = 0;
    while (width != 0) {
        --width;
        result = (result << 8) | bytes[offset + width];
    }
    return result;
}

template <size_t N> void put_text(wa_softmmu_t* mmu, uint64_t address, const char (&text)[N]) {
    CHECK(wa_softmmu_write(mmu, address, text, N) == WA_OK);
}

void test_timespec() {
    auto memory = make_memory();
    auto* mmu = memory.get();
    const uint64_t address = kBase + WA_PAGE_SIZE - 7;
    const timespec input{.tv_sec = -INT64_C(0x100000001), .tv_nsec = 999999999};
    CHECK(write_timespec(mmu, address, input) == WA_OK);
    const auto bytes = guest_bytes<16>(mmu, address);
    CHECK(little_word(bytes, 0, 8) == UINT64_C(0xfffffffeffffffff));
    CHECK(little_word(bytes, 8, 8) == 999999999);

    timespec native{};
    CHECK(read_timespec(mmu, address, native) == WA_OK);
    CHECK(native.tv_sec == input.tv_sec && native.tv_nsec == input.tv_nsec);
    Wasm32Timespec wasm{};
    CHECK(read_timespec(mmu, address, wasm) == WA_OK);
    CHECK(wasm.tv_sec == input.tv_sec && wasm.tv_nsec == input.tv_nsec);
    Armv7Timespec arm{23, 24};
    CHECK(read_timespec(mmu, address, arm) == WA_ERR_OVERFLOW);
    CHECK(arm.tv_sec == 23 && arm.tv_nsec == 24);

    Aarch64Timespec wide{INT32_MIN, INT32_MAX};
    CHECK(write_timespec(mmu, address, wide) == WA_OK);
    CHECK(read_timespec(mmu, address, arm) == WA_OK);
    CHECK(arm.tv_sec == INT32_MIN && arm.tv_nsec == INT32_MAX);
    wide.tv_nsec = int64_t{INT32_MIN} - 1;
    CHECK(write_timespec(mmu, address, wide) == WA_OK);
    CHECK(read_timespec(mmu, address, wasm) == WA_ERR_OVERFLOW);
    CHECK(wasm.tv_sec == input.tv_sec && wasm.tv_nsec == input.tv_nsec);
    CHECK(read_timespec(mmu, address, arm) == WA_ERR_OVERFLOW);
    CHECK(arm.tv_nsec == INT32_MAX);

    constexpr uint64_t tail = kGuard - 8;
    auto before = guest_bytes<8>(mmu, tail);
    CHECK(write_timespec(mmu, tail, input) == WA_ERR_FAULT);
    CHECK(guest_bytes<8>(mmu, tail) == before);
    CHECK(read_timespec(mmu, tail, native) == WA_ERR_FAULT);
    CHECK(native.tv_sec == input.tv_sec && native.tv_nsec == input.tv_nsec);
    CHECK(read_timespec(mmu, UINT64_MAX - 7, native) == WA_ERR_FAULT);
}

void test_stat() {
    auto memory = make_memory();
    auto* mmu = memory.get();
    const uint64_t address = kBase + WA_PAGE_SIZE - 21;
    Aarch64Stat input{
        .st_dev = 0x1234,
        .st_ino = kWidePointer,
        .st_mode = 0100755,
        .st_nlink = 11,
        .st_uid = 12,
        .st_gid = 13,
        .st_rdev = 0xabcd,
        .reserved1 = UINT64_MAX,
        .st_size = -INT64_C(0x102030405),
        .st_blksize = 4096,
        .reserved2 = -1,
        .st_blocks = -19,
        .st_atim = {INT32_MIN, -1},
        .st_mtim = {22, 999999999},
        .st_ctim = {-33, 123456789},
        .unused4 = UINT32_MAX,
        .unused5 = UINT32_MAX,
    };
    CHECK(write_stat(mmu, address, input) == WA_OK);
    const auto bytes = guest_bytes<128>(mmu, address);
    CHECK(little_word(bytes, 0, 8) == input.st_dev);
    CHECK(little_word(bytes, 8, 8) == input.st_ino);
    CHECK(little_word(bytes, 16, 4) == input.st_mode);
    CHECK(little_word(bytes, 20, 4) == input.st_nlink);
    CHECK(little_word(bytes, 24, 4) == input.st_uid);
    CHECK(little_word(bytes, 28, 4) == input.st_gid);
    CHECK(little_word(bytes, 32, 8) == input.st_rdev);
    CHECK(little_word(bytes, 40, 8) == 0);
    CHECK(little_word(bytes, 48, 8) == static_cast<uint64_t>(input.st_size));
    CHECK(little_word(bytes, 56, 4) == static_cast<uint32_t>(input.st_blksize));
    CHECK(little_word(bytes, 60, 4) == 0);
    CHECK(little_word(bytes, 64, 8) == static_cast<uint64_t>(input.st_blocks));
    CHECK(little_word(bytes, 72, 8) == static_cast<uint64_t>(INT32_MIN));
    CHECK(little_word(bytes, 80, 8) == UINT64_MAX);
    CHECK(little_word(bytes, 88, 8) == 22 && little_word(bytes, 96, 8) == 999999999);
    CHECK(little_word(bytes, 104, 8) == static_cast<uint64_t>(-33));
    CHECK(little_word(bytes, 112, 8) == 123456789);
    CHECK(little_word(bytes, 120, 8) == 0);

    struct stat native {};
    CHECK(read_stat(mmu, address, native) == WA_OK);
    CHECK(native.st_ino == input.st_ino && native.st_size == input.st_size);
    CHECK(native.st_mode == input.st_mode && native.st_blocks == input.st_blocks);
    CHECK(write_stat(mmu, address + 150, native) == WA_OK);
    CHECK(guest_bytes<128>(mmu, address + 150) == bytes);

    Wasm32Stat wasm{};
    CHECK(read_stat(mmu, address, wasm) == WA_OK);
    CHECK(wasm.st_atim.tv_sec == INT32_MIN && wasm.st_atim.tv_nsec == -1);
    CHECK(wasm.st_mtim.tv_nsec == 999999999 && wasm.st_ctim.tv_sec == -33);
    CHECK(write_stat(mmu, address + 150, wasm) == WA_OK);
    CHECK(guest_bytes<128>(mmu, address + 150) == bytes);

    Armv7Stat arm{};
    arm.st_ino = 777;
    CHECK(read_stat(mmu, address, arm) == WA_ERR_OVERFLOW);
    CHECK(arm.st_ino == 777);
    arm.st_blocks = UINT64_MAX;
    CHECK(write_stat(mmu, address, arm) == WA_ERR_OVERFLOW);
    CHECK(guest_bytes<128>(mmu, address) == bytes);

    input.st_blocks = int64_t{INT32_MAX} + 1;
    CHECK(write_stat(mmu, address, input) == WA_OK);
    CHECK(read_stat(mmu, address, wasm) == WA_ERR_OVERFLOW);
    CHECK(wasm.st_blocks == -19 && wasm.st_ino == kWidePointer);

    input.st_blocks = 19;
    input.st_ctim.tv_nsec = INT64_C(0x100000000);
    CHECK(write_stat(mmu, address, input) == WA_OK);
    CHECK(read_stat(mmu, address, wasm) == WA_ERR_OVERFLOW);
    CHECK(wasm.st_ctim.tv_nsec == 123456789);
    input.st_ctim.tv_nsec = 0;
    input.st_dev = uint64_t{UINT32_MAX} + 1;
    CHECK(write_stat(mmu, address, input) == WA_OK);
    CHECK(read_stat(mmu, address, wasm) == WA_ERR_OVERFLOW);
    CHECK(wasm.st_dev == 0x1234);
#ifdef __EMSCRIPTEN__
    CHECK(read_stat(mmu, address, native) == WA_ERR_OVERFLOW);
    CHECK(native.st_dev == 0x1234);
#endif
}

void test_iovec_and_epoll() {
    auto memory = make_memory();
    auto* mmu = memory.get();
    const uint64_t address = kBase + 5;
    const Ilp32Iovec input{UINT32_MAX, UINT32_MAX - 1};
    CHECK(write_iovec(mmu, address, input) == WA_OK);
    auto bytes = guest_bytes<16>(mmu, address);
    CHECK(little_word(bytes, 0, 8) == UINT32_MAX);
    CHECK(little_word(bytes, 8, 8) == UINT32_MAX - 1);
    Ilp32Iovec ilp32{};
    HostIovec host{};
    CHECK(read_iovec(mmu, address, ilp32) == WA_OK);
    CHECK(ilp32.iov_base == input.iov_base && ilp32.iov_len == input.iov_len);
    CHECK(read_iovec(mmu, address, host) == WA_OK);
    CHECK(host.iov_base == input.iov_base && host.iov_len == input.iov_len);

    Aarch64Iovec guest{kWidePointer, 15};
    CHECK(write_iovec(mmu, address, guest) == WA_OK);
    CHECK(read_iovec(mmu, address, ilp32) == WA_ERR_OVERFLOW);
    CHECK(ilp32.iov_base == input.iov_base && ilp32.iov_len == input.iov_len);
    if constexpr (sizeof(uintptr_t) == 4) {
        CHECK(read_iovec(mmu, address, host) == WA_ERR_OVERFLOW);
        CHECK(host.iov_base == input.iov_base && host.iov_len == input.iov_len);
    } else {
        CHECK(read_iovec(mmu, address, host) == WA_OK);
        CHECK(host.iov_base == kWidePointer && host.iov_len == 15);
    }
    guest = {42, uint64_t{UINT32_MAX} + 1};
    CHECK(write_iovec(mmu, address, guest) == WA_OK);
    CHECK(read_iovec(mmu, address, ilp32) == WA_ERR_OVERFLOW);
    CHECK(ilp32.iov_base == input.iov_base && ilp32.iov_len == input.iov_len);

    constexpr uint32_t events = UINT32_C(0x80000005);
    const Ilp32EpollEvent event{.events = events, .padding_4 = {}, .data = {.u64 = kWidePointer}};
    CHECK(write_epoll_event(mmu, address, event) == WA_OK);
    bytes = guest_bytes<16>(mmu, address);
    CHECK(little_word(bytes, 0, 4) == events);
    CHECK(little_word(bytes, 4, 4) == 0);
    CHECK(little_word(bytes, 8, 8) == kWidePointer);
    Ilp32EpollEvent event32{};
    X86_64EpollEvent packed{};
    CHECK(read_epoll_event(mmu, address, packed) == WA_OK);
    CHECK(packed.events == events && packed.data.u64 == kWidePointer);

    const uint32_t guest_padding = UINT32_MAX;
    CHECK(wa_softmmu_write(mmu, address + 4, &guest_padding, sizeof(guest_padding)) == WA_OK);
    CHECK(read_epoll_event(mmu, address, event32) == WA_OK);
    CHECK(event32.events == events && event32.data.u64 == kWidePointer);
    CHECK(write_epoll_event(mmu, address, packed) == WA_OK);
    CHECK(guest_bytes<16>(mmu, address) == bytes);
#ifdef WA_TEST_NATIVE_EPOLL
    struct epoll_event native {};
    CHECK(read_epoll_event(mmu, address, native) == WA_OK);
    CHECK(native.events == events && native.data.u64 == kWidePointer);
    CHECK(write_epoll_event(mmu, address, native) == WA_OK);
    CHECK(guest_bytes<16>(mmu, address) == bytes);
#endif
}

void test_native_method_array() {
    auto memory = make_memory();
    auto* mmu = memory.get();
    const uint64_t address = kBase + WA_PAGE_SIZE - 13;
    const std::array<Ilp32JNINativeMethod, 2> input{
        Ilp32JNINativeMethod{11, 22, 33},
        Ilp32JNINativeMethod{44, 55, UINT32_MAX},
    };
    CHECK(write_jni_native_methods(mmu, address, std::span(input)) == WA_OK);
    const auto bytes = guest_bytes<48>(mmu, address);
    CHECK(little_word(bytes, 0, 8) == 11 && little_word(bytes, 8, 8) == 22);
    CHECK(little_word(bytes, 16, 8) == 33 && little_word(bytes, 24, 8) == 44);
    CHECK(little_word(bytes, 32, 8) == 55 && little_word(bytes, 40, 8) == UINT32_MAX);
    std::array<HostJNINativeMethod, 2> host{};
    CHECK(read_jni_native_methods(mmu, address, std::span(host)) == WA_OK);
    CHECK(host[0].name == 11 && host[0].signature == 22 && host[0].fnPtr == 33);
    CHECK(host[1].name == 44 && host[1].signature == 55 && host[1].fnPtr == UINT32_MAX);

    const Aarch64JNINativeMethod wide{14, 15, kWidePointer};
    CHECK(write_jni_native_method(mmu, address + 24, wide) == WA_OK);
    std::array<Ilp32JNINativeMethod, 2> decoded{Ilp32JNINativeMethod{101, 102, 103},
                                               Ilp32JNINativeMethod{104, 105, 106}};
    CHECK(read_jni_native_methods(mmu, address, std::span(decoded)) == WA_ERR_OVERFLOW);
    CHECK(decoded[0].name == 101 && decoded[0].signature == 102 && decoded[0].fnPtr == 103);
    CHECK(decoded[1].name == 104 && decoded[1].signature == 105 && decoded[1].fnPtr == 106);
    Aarch64JNINativeMethod guest{};
    CHECK(read_jni_native_method(mmu, address + 24, guest) == WA_OK);
    CHECK(guest.name == 14 && guest.signature == 15 && guest.fnPtr == kWidePointer);

    if constexpr (sizeof(uintptr_t) == 4) {
        CHECK(read_jni_native_methods(mmu, address, std::span(host)) == WA_ERR_OVERFLOW);
        CHECK(host[1].fnPtr == UINT32_MAX);
    } else {
        CHECK(read_jni_native_methods(mmu, address, std::span(host)) == WA_OK);
        CHECK(host[1].fnPtr == kWidePointer);
    }
    CHECK(read_jni_native_methods(mmu, UINT64_MAX - 8, std::span(decoded)) == WA_ERR_OVERFLOW);
    CHECK(read_jni_native_methods(mmu, kGuard - 24, std::span(decoded)) == WA_ERR_FAULT);
    CHECK(decoded[0].name == 101 && decoded[1].name == 104);
    CHECK(read_jni_native_methods(mmu, 0, std::span<Ilp32JNINativeMethod>{}) == WA_OK);

    const auto tail = guest_bytes<24>(mmu, kGuard - 24);
    CHECK(write_jni_native_methods(mmu, kGuard - 24, std::span(input)) == WA_ERR_FAULT);
    CHECK(guest_bytes<24>(mmu, kGuard - 24) == tail);
}

void test_jvalue() {
    auto memory = make_memory();
    auto* mmu = memory.get();
    const uint64_t address = kBase + WA_PAGE_SIZE - 3;
    const auto round_trip = [&](JValue input, uint64_t expected) {
        CHECK(write_jvalue(mmu, address, input) == WA_OK);
        CHECK(little_word(guest_bytes<8>(mmu, address), 0, 8) == expected);
        JValue output;
        CHECK(read_jvalue(mmu, address, output) == WA_OK);
        CHECK(output.bits() == expected);
        return output;
    };
    JValue value;
    value.set_l(kWidePointer);
    const auto reference = round_trip(value, kWidePointer);
    CHECK(reference.get_l() == kWidePointer);
    uint32_t address32 = 17;
    CHECK(reference.get_l_as(address32) == WA_ERR_OVERFLOW && address32 == 17);
    value.set_l(UINT32_MAX);
    CHECK(round_trip(value, UINT32_MAX).get_l_as(address32) == WA_OK && address32 == UINT32_MAX);
    value.set_z(UINT8_MAX);
    CHECK(round_trip(value, UINT8_MAX).get_z() == UINT8_MAX);
    value.set_b(INT8_MIN);
    CHECK(round_trip(value, 0x80).get_b() == INT8_MIN);
    value.set_c(UINT16_MAX);
    CHECK(round_trip(value, UINT16_MAX).get_c() == UINT16_MAX);
    value.set_s(INT16_MIN);
    CHECK(round_trip(value, 0x8000).get_s() == INT16_MIN);
    value.set_i(INT32_MIN);
    CHECK(round_trip(value, UINT32_C(0x80000000)).get_i() == INT32_MIN);
    value.set_j(INT64_MIN);
    CHECK(round_trip(value, UINT64_C(0x8000000000000000)).get_j() == INT64_MIN);
    constexpr uint32_t float_bits = 0x7fc12345;
    value.set_f(std::bit_cast<float>(float_bits));
    CHECK(std::bit_cast<uint32_t>(round_trip(value, float_bits).get_f()) == float_bits);
    constexpr uint64_t double_bits = UINT64_C(0x8000000000000000);
    value.set_d(std::bit_cast<double>(double_bits));
    CHECK(std::bit_cast<uint64_t>(round_trip(value, double_bits).get_d()) == double_bits);
    CHECK(round_trip(JValue::from_bits(UINT64_C(0xaaaabbbbfffffff1)),
                     UINT64_C(0xaaaabbbbfffffff1)).get_i() == -15);
}

static_assert(static_cast<size_t>(JniSlot::GetVersion) == 4);
static_assert(static_cast<size_t>(JniSlot::FindClass) == 6);
static_assert(static_cast<size_t>(JniSlot::CallIntMethodA) == 51);
static_assert(static_cast<size_t>(JniSlot::NewStringUTF) == 167);
static_assert(static_cast<size_t>(JniSlot::RegisterNatives) == 215);
static_assert(static_cast<size_t>(JniSlot::GetObjectRefType) == 232);
static_assert(kJniTableEntries == 233);

constexpr uint64_t kEnv = kBase + WA_PAGE_SIZE - 128;
constexpr uint64_t kClass = kWidePointer;
constexpr uint64_t kString = UINT64_C(0x1234567887654321);
constexpr uint64_t kMethod = UINT64_C(0x100000012);
constexpr uint64_t kEmptyMethod = 13;
constexpr std::string_view kModifiedUtf8 = "A\xc0\x80Z";

struct JniState {
    std::string text;
    std::vector<JniNativeRegistration> registered;
    std::vector<JValue> arguments;
    JniSlot last_raw = JniSlot::reserved0;
    size_t raw_calls = 0;
    size_t string_calls = 0;
    size_t registration_calls = 0;
    size_t int_calls = 0;

    static int32_t find(void*, uint64_t env, std::string_view name, uint64_t& result) {
        CHECK(env == kEnv && name == "com/example/Contract");
        result = kClass;
        return WA_OK;
    }

    static int32_t new_string(void* user, uint64_t env, std::string_view text, uint64_t& result) {
        CHECK(env == kEnv);
        auto& state = *static_cast<JniState*>(user);
        ++state.string_calls;
        state.text = text;
        result = kString;
        return WA_OK;
    }

    static int32_t register_methods(void* user, uint64_t env, uint64_t clazz,
                                    std::span<const JniNativeRegistration> methods,
                                    int32_t& result) {
        CHECK(env == kEnv && clazz == kClass);
        auto& state = *static_cast<JniState*>(user);
        ++state.registration_calls;
        state.registered.assign(methods.begin(), methods.end());
        result = -7;
        return WA_OK;
    }

    static int32_t get_count(void*, uint64_t env, uint64_t method, size_t& count) {
        CHECK(env == kEnv);
        if (method != kMethod && method != kEmptyMethod)
            return WA_ERR_NOENT;
        count = method == kEmptyMethod ? 0 : 2;
        return WA_OK;
    }

    static int32_t call_int(void* user, uint64_t env, uint64_t object, uint64_t method,
                            std::span<const JValue> args, int32_t& result) {
        CHECK(env == kEnv && object == kString);
        CHECK(method == kMethod || (method == kEmptyMethod && args.empty()));
        auto& state = *static_cast<JniState*>(user);
        ++state.int_calls;
        state.arguments.assign(args.begin(), args.end());
        result = INT32_MIN;
        return WA_OK;
    }

    static int32_t raw(void* user, JniSlot slot, wa_host_call_ctx_t& context) {
        CHECK(context.args[0] == kEnv);
        auto& state = *static_cast<JniState*>(user);
        ++state.raw_calls;
        state.last_raw = slot;
        context.ret0 = static_cast<uint16_t>(slot);
        return WA_OK;
    }

    JniCallbacks callbacks() {
        return {.user = this,
                .find_class = find,
                .new_string_utf = new_string,
                .register_natives = register_methods,
                .argument_count = get_count,
                .call_int_method_a = call_int,
                .raw = raw};
    }
};

void test_jni_vtable() {
    auto memory = make_memory();
    auto* mmu = memory.get();
    JniStubPcs pcs{};
    for (size_t slot = kJniFirstFunctionSlot; slot < pcs.size(); ++slot)
        pcs[slot] = WA_HOST_STUB_BASE + (7 + (pcs.size() - slot) * 3) * WA_HOST_STUB_STRIDE;

    CHECK(build_jni_env(mmu, kEnv, pcs) == WA_OK);
    const auto table = guest_bytes<kJniGuestEnvBytes>(mmu, kEnv);
    CHECK(little_word(table, 0, 8) == kEnv + 8);
    CHECK(!find_jni_slot(pcs, 0) && !find_jni_slot(pcs, WA_EXIT_SENTINEL_PC));
    CHECK(!find_jni_slot(pcs, pcs[4] + 1) && !find_jni_slot(pcs, WA_HOST_STUB_BASE));

    JniState state;
    auto callbacks = state.callbacks();
    wa_host_call_ctx_t context{.mmu = mmu, .arch = WA_ARCH_ARM64};
    context.args[0] = kEnv;
    for (size_t slot = 0; slot < pcs.size(); ++slot) {
        const auto pc = little_word(table, (slot + 1) * 8, 8);
        CHECK(pc == pcs[slot]);
        const auto& info = kJniDispatchTable[slot];
        CHECK(static_cast<size_t>(info.slot) == slot && info.name[0] != '\0');
        if (slot < kJniFirstFunctionSlot) {
            CHECK(pc == 0 && !info.host_call);
            continue;
        }
        CHECK(pc && info.host_call);
        CHECK(find_jni_slot(pcs, pc) == info.slot);
        if (info.slot == JniSlot::FindClass || info.slot == JniSlot::NewStringUTF ||
            info.slot == JniSlot::RegisterNatives || info.slot == JniSlot::CallIntMethodA)
            continue;
        CHECK(info.host_call(&context, &callbacks) == WA_OK);
        CHECK(context.ret0 == slot && state.last_raw == info.slot);
    }
    CHECK(state.raw_calls == kJniFunctionCount - 4);
    CHECK(std::string_view(kJniDispatchTable[232].name) == "GetObjectRefType");

    auto invalid = pcs;
    invalid[8] = invalid[9];
    CHECK(build_jni_env(mmu, kEnv, invalid) == WA_ERR_INVALID);
    invalid = pcs;
    invalid[100] = WA_HOST_STUB_LIMIT;
    CHECK(build_jni_env(mmu, kEnv, invalid) == WA_ERR_INVALID);
    invalid = pcs;
    invalid[3] = pcs[4];
    CHECK(build_jni_env(mmu, kEnv, invalid) == WA_ERR_INVALID);
    CHECK(guest_bytes<kJniGuestEnvBytes>(mmu, kEnv) == table);
    CHECK(build_jni_env(mmu, kEnv + 1, pcs) == WA_ERR_INVALID);
    CHECK(build_jni_env(mmu, UINT64_MAX - 7, pcs) == WA_ERR_OVERFLOW);
    const auto tail = guest_bytes<16>(mmu, kGuard - 16);
    CHECK(build_jni_env(mmu, kGuard - 16, pcs) == WA_ERR_FAULT);
    CHECK(guest_bytes<16>(mmu, kGuard - 16) == tail);

    const auto call = [&](JniSlot expected) {
        const auto pc = little_word(table, (static_cast<size_t>(expected) + 1) * 8, 8);
        const auto slot = find_jni_slot(pcs, pc);
        CHECK(slot && *slot == expected);
        return kJniDispatchTable[static_cast<size_t>(*slot)].host_call(&context, &callbacks);
    };

    constexpr uint64_t class_name = kBase + 16;
    put_text(mmu, class_name, "com/example/Contract");
    context.args[1] = class_name;
    CHECK(call(JniSlot::FindClass) == WA_OK && context.ret0 == kClass);

    constexpr uint64_t text = kBase + 3 * WA_PAGE_SIZE - 2;
    put_text(mmu, text, "A\xc0\x80Z");
    context.args[1] = text;
    context.ret1 = 111;
    context.fret0 = 22;
    context.has_float_ret = 1;
    CHECK(call(JniSlot::NewStringUTF) == WA_OK && context.ret0 == kString);
    CHECK(context.ret1 == 0 && context.fret0 == 0 && context.has_float_ret == 0);
    CHECK(state.text == kModifiedUtf8 && state.string_calls == 1);
    put_text(mmu, kGuard - 2, "A");
    context.args[1] = kGuard - 2;
    CHECK(call(JniSlot::NewStringUTF) == WA_OK && state.text == "A");
    const std::array<char, 2> unterminated{'A', 'B'};
    CHECK(wa_softmmu_write(mmu, kGuard - 2, unterminated.data(), unterminated.size()) == WA_OK);
    CHECK(call(JniSlot::NewStringUTF) == WA_ERR_FAULT);
    callbacks.limits.string_bytes = 2;
    CHECK(call(JniSlot::NewStringUTF) == WA_ERR_OVERFLOW);
    context.args[1] = (uint64_t{1} << 32) | class_name;
    CHECK(call(JniSlot::NewStringUTF) == WA_ERR_FAULT);
    context.args[1] = 0;
    CHECK(call(JniSlot::NewStringUTF) == WA_ERR_FAULT);
    CHECK(state.string_calls == 2 && context.ret0 == kString);
    callbacks.limits = {};

    constexpr uint64_t method_address = kBase + 2 * WA_PAGE_SIZE - 7;
    constexpr uint64_t names = kBase + 2 * WA_PAGE_SIZE + 100;
    put_text(mmu, names, "first");
    put_text(mmu, names + 16, "(I)I");
    put_text(mmu, names + 32, "second");
    put_text(mmu, names + 48, "()V");
    std::array<Aarch64JNINativeMethod, 2> methods{
        Aarch64JNINativeMethod{names, names + 16, kWidePointer},
        Aarch64JNINativeMethod{names + 32, names + 48, 0x80000100},
    };
    CHECK(write_jni_native_methods(mmu, method_address, std::span(methods)) == WA_OK);
    context.args[1] = kClass;
    context.args[2] = method_address;
    context.args[3] = 2;
    CHECK(call(JniSlot::RegisterNatives) == WA_OK && context.ret0 == static_cast<uint32_t>(-7));
    CHECK(state.registration_calls == 1 && state.registered.size() == 2);
    CHECK(state.registered[0].name == "first" && state.registered[0].signature == "(I)I");
    CHECK(state.registered[0].guest_function == kWidePointer);
    CHECK(state.registered[1].name == "second" && state.registered[1].signature == "()V");
    CHECK(state.registered[1].guest_function == 0x80000100);
    context.args[3] = UINT32_MAX;
    CHECK(call(JniSlot::RegisterNatives) == WA_ERR_INVALID);
    context.args[3] = callbacks.limits.native_methods + 1;
    CHECK(call(JniSlot::RegisterNatives) == WA_ERR_OVERFLOW);
    context.args[3] = 2;
    callbacks.limits.string_bytes = 20;
    CHECK(call(JniSlot::RegisterNatives) == WA_ERR_OVERFLOW);
    callbacks.limits = {};
    methods[1].signature = kGuard;
    CHECK(write_jni_native_methods(mmu, method_address, std::span(methods)) == WA_OK);
    CHECK(call(JniSlot::RegisterNatives) == WA_ERR_FAULT);
    CHECK(state.registration_calls == 1);
    context.args[2] = 0;
    context.args[3] = 0;
    CHECK(call(JniSlot::RegisterNatives) == WA_OK);
    CHECK(state.registration_calls == 2 && state.registered.empty());

    constexpr uint64_t arguments_address = kBase + 2 * WA_PAGE_SIZE - 6;
    std::array<JValue, 2> arguments{};
    arguments[0].set_i(-123);
    arguments[1].set_l(kWidePointer);
    CHECK(write_jvalues(mmu, arguments_address, arguments) == WA_OK);
    context.args[1] = kString;
    context.args[2] = kMethod;
    context.args[3] = arguments_address;
    CHECK(call(JniSlot::CallIntMethodA) == WA_OK && context.ret0 == 0x80000000);
    CHECK(state.arguments.size() == 2 && state.arguments[0].get_i() == -123);
    CHECK(state.arguments[1].get_l() == kWidePointer);
    callbacks.limits.arguments = 1;
    CHECK(call(JniSlot::CallIntMethodA) == WA_ERR_OVERFLOW);
    callbacks.limits = {};
    context.args[3] = kGuard - 8;
    CHECK(call(JniSlot::CallIntMethodA) == WA_ERR_FAULT);
    context.args[3] = 0;
    CHECK(call(JniSlot::CallIntMethodA) == WA_ERR_FAULT);
    CHECK(state.int_calls == 1);
    context.args[2] = kEmptyMethod;
    CHECK(call(JniSlot::CallIntMethodA) == WA_OK && state.arguments.empty());
    context.args[2] = 0;
    CHECK(call(JniSlot::CallIntMethodA) == WA_ERR_NOENT);

    const JniCallbacks absent;
    CHECK(dispatch_jni(JniSlot::GetVersion, context, absent) == WA_ERR_NOSYS);
    CHECK(dispatch_jni(JniSlot::reserved3, context, callbacks) == WA_ERR_INVALID);
    context.arch = WA_ARCH_ARM32;
    CHECK(call(JniSlot::NewStringUTF) == WA_ERR_INVALID);
}

} // namespace

int main() {
    test_timespec();
    test_stat();
    test_iovec_and_epoll();
    test_native_method_array();
    test_jvalue();
    test_jni_vtable();
    std::puts("LP64 layouts, SoftMMU codecs and JNI dispatch passed");
}
