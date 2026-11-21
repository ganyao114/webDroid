#include "jni_dbt.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>

using namespace wa::ffi;

namespace {

constexpr uint64_t kEnv = 0x10000;
constexpr uint64_t kClass = UINT64_C(0x7654321087654321);
constexpr uint64_t kString = UINT64_C(0x1234567887654321);
constexpr uint64_t kMethod = UINT64_C(0x100000012);
constexpr uint32_t kFileId = 19;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

void check_status(int32_t status, const char* message) {
    if (status != WA_OK) {
        std::fprintf(stderr, "%s returned %d\n", message, status);
        std::exit(EXIT_FAILURE);
    }
}

int64_t read_page(void* user, uint32_t file, void* out, size_t size, uint64_t offset) {
    const auto& input = *static_cast<const std::vector<uint8_t>*>(user);
    if (file != kFileId || offset > input.size() || size > input.size() - offset)
        return WA_ERR_FAULT;
    std::memcpy(out, input.data() + offset, size);
    return static_cast<int64_t>(size);
}

int32_t find_class(void*, uint64_t env, std::string_view name, uint64_t& result) {
    check(env == kEnv && name == "com/example/Contract", "FindClass marshalling");
    result = kClass;
    return WA_OK;
}

int32_t new_string(void*, uint64_t env, std::string_view text, uint64_t& result) {
    check(env == kEnv && text == "A\xc0\x80Z", "NewStringUTF marshalling");
    result = kString;
    return WA_OK;
}

int32_t register_methods(void* user, uint64_t env, uint64_t clazz,
                         std::span<const JniNativeRegistration> methods, int32_t& result) {
    check(env == kEnv && clazz == kClass && methods.size() == 2, "RegisterNatives arguments");
    check(methods[0].name == "increment" && methods[0].signature == "(I)I" &&
              methods[1].name == "finish" && methods[1].signature == "()V" &&
              methods[0].guest_function != methods[1].guest_function,
          "RegisterNatives descriptors");
    auto* dbt = *static_cast<wa_dbt_t**>(user);
    const uint64_t args[] = {env, clazz, 41};
    uint64_t returned = 0;
    check_status(wa_dbt_call(dbt, methods[0].guest_function, args, std::size(args), &returned),
                 "Call registered native");
    check(returned == 42, "Registered native return value");
    result = 0;
    return WA_OK;
}

int32_t argument_count(void*, uint64_t env, uint64_t method, size_t& count) {
    check(env == kEnv && method == kMethod, "CallIntMethodA method identity");
    count = 2;
    return WA_OK;
}

int32_t call_int(void*, uint64_t env, uint64_t object, uint64_t method,
                 std::span<const JValue> args, int32_t& result) {
    check(env == kEnv && object == kString && method == kMethod && args.size() == 2,
          "CallIntMethodA arguments");
    check(args[0].get_i() == -123 && args[1].get_l() == kString, "CallIntMethodA jvalue array");
    result = -197;
    return WA_OK;
}

} // namespace

int main(int argc, char** argv) {
    check(argc == 2, "Expected the NDK-built JNI guest .so");
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<uint8_t> elf{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>{}};
    check(!elf.empty(), "Could not read the JNI guest .so");

    std::unique_ptr<wa_softmmu_t, decltype(&wa_softmmu_destroy)>
        mmu(wa_softmmu_create(read_page, &elf), wa_softmmu_destroy);
    check(static_cast<bool>(mmu), "Create SoftMMU");
    check_status(wa_softmmu_map_anon(mmu.get(), kEnv, WA_PAGE_SIZE, WA_PROT_READ | WA_PROT_WRITE),
                 "Map JNIEnv");

    wa_dbt_t* callback_dbt = nullptr;
    JniCallbacks callbacks{.user = &callback_dbt,
                           .find_class = find_class,
                           .new_string_utf = new_string,
                           .register_natives = register_methods,
                           .argument_count = argument_count,
                           .call_int_method_a = call_int};
    std::unique_ptr<wa_dbt_t, decltype(&wa_dbt_destroy)> dbt(
        wa_dbt_create(WA_ARCH_ARM64, mmu.get()), wa_dbt_destroy);
    check(static_cast<bool>(dbt), "Create ARM64 DBT");
    callback_dbt = dbt.get();
    JniStubPcs pcs{}, repeated{};
    check_status(bind_jni_stubs(dbt.get(), callbacks, pcs), "Bind JNI stubs");
    check_status(bind_jni_stubs(dbt.get(), callbacks, repeated), "Rebind JNI stubs");
    check(pcs == repeated, "JNI stub PCs changed on retry");
    JniCallbacks other_callbacks;
    check(bind_jni_stubs(dbt.get(), other_callbacks, repeated) == WA_ERR_EXIST && pcs == repeated,
          "JNI binding accepted another provider");
    check_status(build_jni_env(mmu.get(), kEnv, pcs), "Build guest JNIEnv");

    uint64_t image = 0;
    check_status(wa_dbt_load_elf(dbt.get(), kFileId, elf.data(), elf.size(), 0x1000000, &image),
                 "Load JNI guest .so");
    uint64_t entry = 0, result = 0;
    check_status(wa_dbt_find_symbol(dbt.get(), "wa_jni_probe", &entry), "Find JNI guest probe");
    check_status(wa_dbt_call(dbt.get(), entry, &kEnv, 1, &result), "Run JNI guest probe");
    check(static_cast<uint32_t>(result) == static_cast<uint32_t>(-197), "JNI guest return value");

    wa_dbt_stats_t stats{};
    wa_dbt_get_stats(dbt.get(), &stats);
    check(stats.host_calls == 4 && stats.reverse_callbacks == 1, "JNI calls did not all trap");
    std::puts("NDK JNI guest called four host thunks and a registered guest native");
}
