#include "bionic_internal.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace wa::dbt {

GuestEvent::~GuestEvent() {
    if (notification >= 0)
        close(notification);
}

int HostFd(const wa_dbt& dbt, uint64_t descriptor) {
    const auto found = dbt.bionic->descriptors.find(static_cast<int32_t>(descriptor));
    if (found != dbt.bionic->descriptors.end())
        return found->second.host;
    errno = EBADF;
    return -1;
}

int AdoptFd(wa_dbt& dbt, int host, bool owned, int minimum) {
    if (host < 0)
        return -1;
    auto& descriptors = dbt.bionic->descriptors;
    auto candidate = descriptors.lower_bound(minimum);
    while (candidate != descriptors.end() && candidate->first == minimum) {
        if (minimum == INT32_MAX) {
            if (owned)
                close(host);
            errno = EMFILE;
            return -1;
        }
        ++minimum;
        ++candidate;
    }
    try {
        descriptors.emplace_hint(candidate, minimum, GuestDescriptor{host, owned, {}});
    } catch (...) {
        if (owned)
            close(host);
        throw;
    }
    return minimum;
}

int CloseFd(wa_dbt& dbt, int descriptor) {
    auto& descriptors = dbt.bionic->descriptors;
    auto found = descriptors.find(descriptor);
    if (found == descriptors.end()) {
        errno = EBADF;
        return -1;
    }
    const auto result = found->second.owned ? close(found->second.host) : 0;
    descriptors.erase(found);
    RemoveEpollInterests(dbt, descriptor);
    DropDescriptorFromLoopers(dbt, descriptor);
    return result;
}

void StorePipe(wa_host_call_ctx_t& ctx, int read_end, int write_end, uint64_t destination) {
    auto& dbt = *ctx.dbt;
    int first = -1, second = -1;
    try {
        first = AdoptFd(dbt, read_end);
        if (first >= 0)
            second = AdoptFd(dbt, write_end);
        else
            close(write_end);
        if (first >= 0 && second >= 0) {
            const int32_t result[] = {first, second};
            GuestMemory(ctx).Write(destination, result, sizeof(result));
            return;
        }
    } catch (...) {
        if (first < 0)
            close(write_end);
        if (first >= 0)
            CloseFd(dbt, first);
        if (second >= 0)
            CloseFd(dbt, second);
        throw;
    }
    const int error = errno;
    if (first >= 0)
        CloseFd(dbt, first);
    Fail(ctx, error);
}

std::shared_ptr<GuestEvent> EventFor(const wa_dbt& dbt, int descriptor) {
    const auto found = dbt.bionic->descriptors.find(descriptor);
    return found == dbt.bionic->descriptors.end() ? nullptr : found->second.event;
}

std::shared_ptr<GuestEpoll> EpollFor(const wa_dbt& dbt, int descriptor) {
    const auto found = dbt.bionic->descriptors.find(descriptor);
    return found == dbt.bionic->descriptors.end() ? nullptr : found->second.epoll;
}

void RemoveEpollInterests(wa_dbt& dbt, int descriptor) {
    for (const auto& [id, entry] : dbt.bionic->descriptors) {
        if (entry.epoll) entry.epoll->interests.erase(descriptor);
    }
}

} // namespace wa::dbt
