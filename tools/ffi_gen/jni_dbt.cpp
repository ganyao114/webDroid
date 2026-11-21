#include "jni_dbt.h"

#include "../../runtime/linker_ffi/dbt_internal.h"

namespace wa::ffi {

int32_t bind_jni_stubs(wa_dbt_t* dbt, JniCallbacks& callbacks, JniStubPcs& out) {
    if (!dbt || dbt->arch != WA_ARCH_ARM64)
        return WA_ERR_INVALID;

    try {
        std::lock_guard lock(dbt->mutex);
        struct Rollback {
            std::vector<wa::dbt::HostSymbol>& hosts;
            const size_t original_size;
            bool committed = false;

            ~Rollback() {
                if (!committed)
                    hosts.resize(original_size);
            }
        } registration{dbt->hosts, dbt->hosts.size()};

        JniStubPcs pcs{};
        for (size_t slot = kJniFirstFunctionSlot; slot < kJniTableEntries; ++slot) {
            const auto& info = kJniDispatchTable[slot];
            const std::string name = std::string("wa_jni_") + info.name;
            uint64_t pc = 0;
            if (wa::dbt::ResolveImport(*dbt, {}, name, pc)) {
                const auto& host = dbt->hosts[(pc - WA_HOST_STUB_BASE) / WA_HOST_STUB_STRIDE];
                if (host.function != info.host_call || host.user != &callbacks)
                    return WA_ERR_EXIST;
            } else {
                const auto status = wa::dbt::RegisterHost(*dbt, {}, name, info.host_call, &callbacks);
                if (status != WA_OK)
                    return status;
                pc = WA_HOST_STUB_BASE + (dbt->hosts.size() - 1) * WA_HOST_STUB_STRIDE;
            }
            pcs[slot] = pc;
        }
        out = pcs;
        registration.committed = true;
        return WA_OK;
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
}

} // namespace wa::ffi
