#include "bionic_errno.h"
#include "bionic_internal.h"

namespace wa::dbt {

int GuestErrno(int error) {
    if (error == 0)
        return 0;
#define MAP(host, guest)                                                                           \
    if (error == host)                                                                             \
        return guest;
    WA_ERRNO_MAP(MAP)
#undef MAP
    if (error == ENOTSUP)
        return 95;
    return 5;
}

int HostErrno(int error) {
    switch (error) {
    case 0:
        return 0;
#define MAP(host, guest)                                                                           \
    case guest:                                                                                    \
        return host;
        WA_ERRNO_MAP(MAP)
#undef MAP
    default:
        return EINVAL;
    }
}

uint64_t ErrnoAddress(wa_dbt& dbt) {
    auto& address = dbt.running->errno_address;
    if (!address) {
        address = AllocateGuest(dbt, sizeof(int32_t));
        if (!address)
            throw GuestFault{WA_ERR_NOMEM};
    }
    return address;
}

void SetErrno(wa_dbt& dbt, int error) {
    GuestMemory(dbt.mmu, dbt.arch).Write<int32_t>(ErrnoAddress(dbt), error);
}

void PosixResult(wa_host_call_ctx_t& ctx, int64_t result) {
    if (result == -1)
        SetErrno(*ctx.dbt, GuestErrno(errno));
    ctx.ret0 = result;
}

void Fail(wa_host_call_ctx_t& ctx, int error) {
    SetErrno(*ctx.dbt, GuestErrno(error));
    ctx.ret0 = UINT64_MAX;
}

} // namespace wa::dbt
