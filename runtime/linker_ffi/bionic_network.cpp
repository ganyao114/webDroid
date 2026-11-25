#include "bionic_internal.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace wa::dbt {
namespace {

constexpr size_t kPacketLimit = 16 << 20;

int HostFamily(int family) {
    switch (family) {
    case 0:
        return AF_UNSPEC;
    case 1:
        return AF_UNIX;
    case 2:
        return AF_INET;
    case 10:
        return AF_INET6;
    default:
        return -1;
    }
}

int GuestFamily(int family) {
    if (family == AF_INET6)
        return 10;
    if (family == AF_INET)
        return 2;
    if (family == AF_UNIX)
        return 1;
    return 0;
}

int HostType(uint64_t type) {
    switch (type & 0xf) {
    case 0:
        return 0;
    case 1:
        return SOCK_STREAM;
    case 2:
        return SOCK_DGRAM;
    case 3:
        return SOCK_RAW;
    case 5:
        return SOCK_SEQPACKET;
    default:
        return -1;
    }
}

int GuestType(int type) {
    if (type == SOCK_STREAM)
        return 1;
    if (type == SOCK_DGRAM)
        return 2;
    if (type == SOCK_RAW)
        return 3;
    if (type == SOCK_SEQPACKET)
        return 5;
    return 0;
}

void SocketFlags(int fd, unsigned flags) {
    if (fd < 0)
        return;
    if (flags & 0x80000)
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (flags & 0x800)
        fcntl(fd, F_SETFL, O_NONBLOCK);
#if defined(__APPLE__)
    const int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
}

int MessageFlags(unsigned flags) {
    int result = 0;
    if (flags & 1)
        result |= MSG_OOB;
    if (flags & 2)
        result |= MSG_PEEK;
    if (flags & 4)
        result |= MSG_DONTROUTE;
    if (flags & 0x20)
        result |= MSG_TRUNC;
    if (flags & 0x40)
        result |= MSG_DONTWAIT;
    if (flags & 0x80)
        result |= MSG_EOR;
    if (flags & 0x100)
        result |= MSG_WAITALL;
#ifdef MSG_NOSIGNAL
    if (flags & 0x4000)
        result |= MSG_NOSIGNAL;
#endif
    return result;
}

struct Address {
    sockaddr_storage value{};
    socklen_t size = 0;
    sockaddr* data() { return reinterpret_cast<sockaddr*>(&value); }
    const sockaddr* data() const { return reinterpret_cast<const sockaddr*>(&value); }

    bool Read(GuestMemory mem, uint64_t address, uint64_t length) {
        if (length < 2 || length > sizeof(value)) {
            errno = EINVAL;
            return false;
        }
        const auto family = HostFamily(mem.Read<uint16_t>(address));
        if (family < 0) {
            errno = EAFNOSUPPORT;
            return false;
        }
        size = length;
        mem.Read(address + 2, reinterpret_cast<uint8_t*>(&value) + 2, length - 2);
        data()->sa_family = family;
#if defined(__APPLE__)
        data()->sa_len = size;
#endif
        return true;
    }

    static void Write(GuestMemory mem, uint64_t address, const sockaddr* value, size_t size,
                      size_t limit) {
        const auto actual = std::min(limit, size);
        const uint16_t family = GuestFamily(value->sa_family);
        mem.Write(address, &family, std::min(actual, sizeof(family)));
        if (actual > 2)
            mem.Write(address + 2, reinterpret_cast<const uint8_t*>(value) + 2, actual - 2);
    }

    void Write(GuestMemory mem, uint64_t address, uint64_t length) const {
        if (!address)
            return;
        const auto limit = mem.Read<uint32_t>(length);
        Write(mem, address, data(), size, std::min<size_t>(limit, sizeof(value)));
        mem.Write<uint32_t>(length, size);
    }
};

void GetInterfaces(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto result = ctx.args[0];
    if (!result) return Fail(ctx, EFAULT);
    mem.WriteWord(result, 0);
    ifaddrs* first = nullptr;
    if (getifaddrs(&first)) return PosixResult(ctx, -1);
    const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> addresses(first, freeifaddrs);
    std::vector<const ifaddrs*> list;
    for (auto* item = first; item; item = item->ifa_next) {
        const auto* addr = item->ifa_addr;
        if (item->ifa_name && addr && (addr->sa_family == AF_INET || addr->sa_family == AF_INET6))
            list.push_back(item);
    }
    if (list.empty()) return;

    const auto word = mem.width();
    constexpr auto address_size = sizeof(sockaddr_storage);
    const auto stride = AlignUp(7 * word + IFNAMSIZ + 3 * address_size, word);
    GuestBuffer storage(*ctx.dbt, list.size() * stride);
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& item = *list[i];
        const auto base = storage.address + i * stride;
        const auto name = base + 7 * word;
        const auto length = strnlen(item.ifa_name, IFNAMSIZ - 1);
        mem.Write(name, item.ifa_name, length);
        mem.Write<uint8_t>(name + length, 0);
        mem.WriteWord(base, i + 1 < list.size() ? base + stride : 0);
        mem.WriteWord(base + word, name);
        uint32_t flags = 0;
        constexpr std::pair<unsigned, uint32_t> flag_bits[]{
            {IFF_UP, 1}, {IFF_BROADCAST, 2}, {IFF_LOOPBACK, 8}, {IFF_POINTOPOINT, 16},
            {IFF_RUNNING, 64}, {IFF_NOARP, 128}, {IFF_PROMISC, 256}, {IFF_ALLMULTI, 512},
            {IFF_MULTICAST, 4096},
        };
        for (const auto& [host, guest] : flag_bits)
            if (item.ifa_flags & host) flags |= guest;
        mem.Write<uint32_t>(base + 2 * word, flags);

        const auto write_address = [&](size_t slot, const sockaddr* address) {
            const auto at = name + IFNAMSIZ + (slot - 3) * address_size;
            if (address && (address->sa_family == AF_INET || address->sa_family == AF_INET6)) {
                const auto bytes = address->sa_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
                Address::Write(mem, at, address, bytes, address_size);
                mem.WriteWord(base + slot * word, at);
            } else {
                mem.WriteWord(base + slot * word, 0);
            }
        };
        write_address(3, item.ifa_addr);
        write_address(4, item.ifa_netmask);
        write_address(5, item.ifa_flags & IFF_POINTOPOINT ? item.ifa_dstaddr :
                     item.ifa_flags & IFF_BROADCAST ? item.ifa_broadaddr : nullptr);
        mem.WriteWord(base + 6 * word, 0);
    }
    mem.WriteWord(result, std::exchange(storage.address, 0));
}

void FreeInterfaces(wa_host_call_ctx_t& ctx) { FreeGuest(*ctx.dbt, ctx.args[0]); }

template <bool Pair> void Socket(wa_host_call_ctx_t& ctx) {
    const auto family = HostFamily(ctx.args[0]);
    const auto type = HostType(ctx.args[1]);
    const auto protocol = static_cast<int>(ctx.args[2]);
    if (family < 0)
        return Fail(ctx, EAFNOSUPPORT);
    if (type <= 0 || (ctx.args[1] & ~UINT64_C(0x8080f)))
        return Fail(ctx, EINVAL);
    if constexpr (Pair) {
        int descriptors[2];
        if (socketpair(family, type, protocol, descriptors))
            return PosixResult(ctx, -1);
        SocketFlags(descriptors[0], ctx.args[1]);
        SocketFlags(descriptors[1], ctx.args[1]);
        StorePipe(ctx, descriptors[0], descriptors[1], ctx.args[3]);
    } else {
        const auto fd = socket(family, type, protocol);
        SocketFlags(fd, ctx.args[1]);
        PosixResult(ctx, AdoptFd(*ctx.dbt, fd));
    }
}

template <bool Bind> void SetAddress(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    Address address;
    if (!address.Read(mem, ctx.args[1], ctx.args[2]))
        return PosixResult(ctx, -1);
    const auto fd = HostFd(*ctx.dbt, ctx.args[0]);
    if (fd < 0)
        return PosixResult(ctx, -1);
    const auto result =
        Bind ? bind(fd, address.data(), address.size) : connect(fd, address.data(), address.size);
    PosixResult(ctx, result);
}

enum class Addr { Accept, Accept4, Peer, Local };
template <Addr op> void GetAddress(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto fd = HostFd(*ctx.dbt, ctx.args[0]);
    if (fd < 0)
        return PosixResult(ctx, -1);
    Address address;
    address.size = sizeof(address.value);
    int result;
    if constexpr (op == Addr::Accept || op == Addr::Accept4) {
        const auto flags = op == Addr::Accept4 ? ctx.args[3] : 0;
        if (flags & ~UINT64_C(0x80800))
            return Fail(ctx, EINVAL);
        result = accept(fd, address.data(), &address.size);
        SocketFlags(result, flags);
    } else if constexpr (op == Addr::Peer) {
        result = getpeername(fd, address.data(), &address.size);
    } else {
        result = getsockname(fd, address.data(), &address.size);
    }
    if constexpr (op == Addr::Accept || op == Addr::Accept4) {
        if (result >= 0) {
            try {
                address.Write(mem, ctx.args[1], ctx.args[2]);
            } catch (...) {
                close(result);
                throw;
            }
            result = AdoptFd(*ctx.dbt, result);
        }
    } else if (result == 0) {
        address.Write(mem, ctx.args[1], ctx.args[2]);
    }
    PosixResult(ctx, result);
}

enum class Packet { Send, Receive, SendTo, ReceiveFrom };
template <Packet op> void Transfer(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    CallReader args(ctx);
    const auto fd = HostFd(*ctx.dbt, args.Next());
    if (fd < 0)
        return PosixResult(ctx, -1);
    const auto buffer = args.Next(), size = args.Next(), flags = args.Next();
    if (size > kPacketLimit)
        return Fail(ctx, EMSGSIZE);
    std::vector<uint8_t> bytes(size);
    ssize_t result;
    if constexpr (op == Packet::Send || op == Packet::SendTo) {
        mem.Read(buffer, bytes.data(), bytes.size());
        Address to;
        if constexpr (op == Packet::SendTo) {
            const auto address = args.Next(), count = args.Next();
            if (address && !to.Read(mem, address, count))
                return PosixResult(ctx, -1);
        }
        result = sendto(fd, bytes.data(), bytes.size(), MessageFlags(flags),
                        to.size ? to.data() : nullptr, to.size);
    } else {
        Address from;
        uint64_t address = 0, count = 0;
        if constexpr (op == Packet::ReceiveFrom) {
            address = args.Next();
            count = args.Next();
        }
        from.size = sizeof(from.value);
        result = recvfrom(fd, bytes.data(), bytes.size(), MessageFlags(flags),
                          address ? from.data() : nullptr, address ? &from.size : nullptr);
        if (result >= 0) {
            mem.Write(buffer, bytes.data(), std::min<uint64_t>(size, result));
            from.Write(mem, address, count);
        }
    }
    PosixResult(ctx, result);
}

struct Option {
    int level, name;
    bool timeval = false;
};

std::optional<Option> SocketOption(int level, int name) {
    if (level == 1) {
        const std::pair<int, int> options[] = {
            {1, SO_DEBUG},       {2, SO_REUSEADDR},  {3, SO_TYPE},      {4, SO_ERROR},
            {5, SO_DONTROUTE},   {6, SO_BROADCAST},  {7, SO_SNDBUF},    {8, SO_RCVBUF},
            {9, SO_KEEPALIVE},   {10, SO_OOBINLINE}, {13, SO_LINGER},   {15, SO_REUSEPORT},
            {18, SO_RCVLOWAT},   {19, SO_SNDLOWAT},  {20, SO_RCVTIMEO}, {21, SO_SNDTIMEO},
            {30, SO_ACCEPTCONN},
        };
        for (const auto& [guest, host] : options)
            if (guest == name)
                return Option{SOL_SOCKET, host, guest == 20 || guest == 21};
    } else if (level == IPPROTO_TCP) {
        if (name == 1)
            return Option{level, TCP_NODELAY};
        if (name == 2)
            return Option{level, TCP_MAXSEG};
    } else if (level == IPPROTO_IP) {
        const std::pair<int, int> options[] = {
            {1, IP_TOS},
            {2, IP_TTL},
            {32, IP_MULTICAST_IF},
            {33, IP_MULTICAST_TTL},
            {34, IP_MULTICAST_LOOP},
            {35, IP_ADD_MEMBERSHIP},
            {36, IP_DROP_MEMBERSHIP},
        };
        for (const auto& [guest, host] : options)
            if (guest == name)
                return Option{level, host};
    } else if (level == IPPROTO_IPV6) {
        const std::pair<int, int> options[] = {
            {16, IPV6_UNICAST_HOPS},   {17, IPV6_MULTICAST_IF}, {18, IPV6_MULTICAST_HOPS},
            {19, IPV6_MULTICAST_LOOP}, {20, IPV6_JOIN_GROUP},   {21, IPV6_LEAVE_GROUP},
            {26, IPV6_V6ONLY},
        };
        for (const auto& [guest, host] : options)
            if (guest == name)
                return Option{level, host};
    }
    return std::nullopt;
}

template <bool Set> void Options(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    CallReader args(ctx);
    const auto fd = HostFd(*ctx.dbt, args.Next());
    if (fd < 0)
        return PosixResult(ctx, -1);
    const auto level = static_cast<int>(args.Next()), name = static_cast<int>(args.Next());
    const auto option = SocketOption(level, name);
    if (!option)
        return Fail(ctx, ENOPROTOOPT);
    const auto address = args.Next(), size_arg = args.Next();
    const auto length = Set ? size_arg : mem.Read<uint32_t>(size_arg);
    if (length > 4096)
        return Fail(ctx, EINVAL);
    if (option->timeval) {
        timeval value{};
        if constexpr (Set) {
            if (length < 2 * mem.width())
                return Fail(ctx, EINVAL);
            value.tv_sec = SignedWord(mem, mem.ReadWord(address));
            value.tv_usec = SignedWord(mem, mem.ReadWord(address + mem.width()));
            PosixResult(ctx, setsockopt(fd, option->level, option->name, &value, sizeof(value)));
        } else {
            socklen_t size = sizeof(value);
            if (getsockopt(fd, option->level, option->name, &value, &size))
                return PosixResult(ctx, -1);
            if (length < 2 * mem.width())
                return Fail(ctx, EINVAL);
            mem.WriteWord(address, value.tv_sec);
            mem.WriteWord(address + mem.width(), value.tv_usec);
            mem.Write<uint32_t>(size_arg, 2 * mem.width());
        }
    } else {
        std::array<uint8_t, 4096> data{};
        if constexpr (Set) {
            mem.Read(address, data.data(), length);
            PosixResult(ctx, setsockopt(fd, option->level, option->name, data.data(), length));
        } else {
            socklen_t size = length;
            if (getsockopt(fd, option->level, option->name, data.data(), &size))
                return PosixResult(ctx, -1);
            if (level == 1 && (name == 3 || name == 4) && size == 4) {
                int32_t native;
                std::memcpy(&native, data.data(), sizeof(native));
                const int32_t value = name == 3 ? GuestType(native) : GuestErrno(native);
                std::memcpy(data.data(), &value, sizeof(value));
            }
            mem.Write(address, data.data(), size);
            mem.Write<uint32_t>(size_arg, size);
        }
    }
}

int GuestGai(int error) {
    if (!error)
        return 0;
    if (error == EAI_AGAIN)
        return 2;
    if (error == EAI_BADFLAGS)
        return 3;
    if (error == EAI_FAIL)
        return 4;
    if (error == EAI_FAMILY)
        return 5;
    if (error == EAI_MEMORY)
        return 6;
    if (error == EAI_NONAME)
        return 8;
    if (error == EAI_SERVICE)
        return 9;
    if (error == EAI_SOCKTYPE)
        return 10;
    if (error == EAI_SYSTEM)
        return 11;
    if (error == EAI_OVERFLOW)
        return 14;
    return 4;
}

int AiFlags(int flags) {
    int result = 0;
    if (flags & 1)
        result |= AI_PASSIVE;
    if (flags & 2)
        result |= AI_CANONNAME;
    if (flags & 4)
        result |= AI_NUMERICHOST;
    if (flags & 8)
        result |= AI_NUMERICSERV;
    if (flags & 0x400)
        result |= AI_ADDRCONFIG;
    if (flags & 0x800)
        result |= AI_V4MAPPED;
    if (flags & 0x100)
        result |= AI_ALL;
    return result;
}

void GetAddrInfo(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    const auto node = ctx.args[0] ? mem.String(ctx.args[0]) : "";
    const auto service = ctx.args[1] ? mem.String(ctx.args[1]) : "";
    const auto hints_pointer = ctx.args[2];
    const auto result = ctx.args[3];
    const auto pointers = AlignUp(20, mem.width());
    addrinfo hints{};
    if (hints_pointer) {
        hints.ai_flags = AiFlags(mem.Read<int32_t>(hints_pointer));
        hints.ai_family = HostFamily(mem.Read<int32_t>(hints_pointer + 4));
        hints.ai_socktype = HostType(mem.Read<int32_t>(hints_pointer + 8));
        hints.ai_protocol = mem.Read<int32_t>(hints_pointer + 12);
    }
    addrinfo* first = nullptr;
    const auto code =
        getaddrinfo(ctx.args[0] ? node.c_str() : nullptr, ctx.args[1] ? service.c_str() : nullptr,
                    hints_pointer ? &hints : nullptr, &first);
    ctx.ret0 = GuestGai(code);
    if (code == EAI_SYSTEM)
        SetErrno(dbt, GuestErrno(errno));
    mem.WriteWord(result, 0);
    if (code)
        return;
    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> release(first, freeaddrinfo);
    const auto struct_size = pointers + 3 * mem.width();
    auto link = result;
    std::vector<uint64_t> allocations;
    try {
        for (auto* item = first; item; item = item->ai_next) {
            const auto name_size = item->ai_canonname ? std::strlen(item->ai_canonname) + 1 : 0;
            GuestBuffer storage(dbt, struct_size + item->ai_addrlen + name_size);
            const auto address = storage.address;
            const auto sockaddr = address + struct_size;
            const auto canon = name_size ? sockaddr + item->ai_addrlen : 0;
            mem.Write<int32_t>(address, hints_pointer ? mem.Read<int32_t>(hints_pointer) : 0);
            mem.Write<int32_t>(address + 4, GuestFamily(item->ai_family));
            mem.Write<int32_t>(address + 8, GuestType(item->ai_socktype));
            mem.Write<int32_t>(address + 12, item->ai_protocol);
            mem.Write<uint32_t>(address + 16, item->ai_addrlen);
            mem.WriteWord(address + pointers, canon);
            mem.WriteWord(address + pointers + mem.width(), sockaddr);
            if (canon)
                mem.Write(canon, item->ai_canonname, name_size);
            Address::Write(mem, sockaddr, item->ai_addr, item->ai_addrlen, item->ai_addrlen);
            mem.WriteWord(link, address);
            link = address + pointers + 2 * mem.width();
            allocations.push_back(address);
            storage.address = 0;
        }
    } catch (...) {
        for (const auto address : allocations)
            FreeGuest(dbt, address);
        mem.WriteWord(result, 0);
        throw;
    }
}

uint64_t HostErrnoAddress(wa_dbt& dbt, const GuestMemory& mem) {
    auto& address = dbt.running->host_errno_address;
    if (!address) {
        address = AllocateGuest(dbt, sizeof(int32_t));
        if (!address) throw GuestFault{WA_ERR_NOMEM};
        mem.Write<int32_t>(address, 0);
    }
    return address;
}

void HostErrno(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = HostErrnoAddress(*ctx.dbt, GuestMemory(ctx));
}

void GetHostByName(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    const auto name = mem.String(ctx.args[0]);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;
    addrinfo* results = nullptr;
    const auto error = getaddrinfo(name.c_str(), nullptr, &hints, &results);
    if (error) {
        int32_t host_error = 3; // NO_RECOVERY
        if (error == EAI_AGAIN) host_error = 2;
        if (error == EAI_NONAME) host_error = 1;
#if defined(EAI_NODATA)
        if (error == EAI_NODATA) host_error = 4;
#endif
        if (error == EAI_SYSTEM || error == EAI_MEMORY) {
            SetErrno(dbt, GuestErrno(error == EAI_MEMORY ? ENOMEM : errno));
            host_error = -1; // NETDB_INTERNAL
        }
        mem.Write<int32_t>(HostErrnoAddress(dbt, mem), host_error);
        return;
    }
    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> resolved(results, freeaddrinfo);
    std::vector<in_addr> addresses;
    for (auto* item = results; item; item = item->ai_next) {
        if (item->ai_family == AF_INET && item->ai_addrlen >= sizeof(sockaddr_in))
            addresses.push_back(reinterpret_cast<const sockaddr_in*>(item->ai_addr)->sin_addr);
    }
    if (addresses.empty()) {
        mem.Write<int32_t>(HostErrnoAddress(dbt, mem), 4); // NO_DATA
        return;
    }

    const std::string_view canon = results->ai_canonname ? results->ai_canonname : name.c_str();
    const auto word = mem.width();
    const auto header = 3 * word + 2 * sizeof(int32_t);
    const auto pointers = (2 + addresses.size()) * word;
    GuestBuffer entry(dbt, header + pointers + addresses.size() * sizeof(in_addr) + canon.size() + 1);
    const auto aliases = entry.address + header;
    const auto list = aliases + word;
    const auto bytes = list + (addresses.size() + 1) * word;
    const auto label = bytes + addresses.size() * sizeof(in_addr);
    mem.WriteWord(entry.address, label);
    mem.WriteWord(entry.address + word, aliases);
    mem.Write<int32_t>(entry.address + 2 * word, GuestFamily(AF_INET));
    mem.Write<int32_t>(entry.address + 2 * word + sizeof(int32_t), sizeof(in_addr));
    mem.WriteWord(entry.address + header - word, list);
    mem.WriteWord(aliases, 0);
    for (size_t i = 0; i < addresses.size(); ++i)
        mem.WriteWord(list + i * word, bytes + i * sizeof(in_addr));
    mem.WriteWord(list + addresses.size() * word, 0);
    mem.Write(bytes, addresses.data(), addresses.size() * sizeof(in_addr));
    mem.Write(label, canon.data(), canon.size() + 1);
    FreeGuest(dbt, std::exchange(dbt.running->host_entry, entry.address));
    ctx.ret0 = std::exchange(entry.address, 0);
}

void FreeAddrInfo(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto next = AlignUp(20, mem.width()) + 2 * mem.width();
    for (auto address = ctx.args[0]; address && ctx.dbt->allocations.contains(address);) {
        const auto link = mem.ReadWord(address + next);
        FreeGuest(*ctx.dbt, address);
        address = link;
    }
}

void GaiError(wa_host_call_ctx_t& ctx) {
    const char* errors[] = {"Success",
                            "Address family for hostname not supported",
                            "Temporary failure in name resolution",
                            "Invalid flags",
                            "Non-recoverable failure in name resolution",
                            "Address family not supported",
                            "Memory allocation failure",
                            "No address associated with hostname",
                            "Name or service not known",
                            "Service not supported for socket type",
                            "Socket type not supported",
                            "System error",
                            "Invalid hints",
                            "Protocol not supported",
                            "Buffer overflow"};
    ctx.ret0 = InternString(*ctx.dbt, ctx.args[0] < std::size(errors) ? errors[ctx.args[0]]
                                                                      : "Unknown address error");
}

void GetNameInfo(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    CallReader args(ctx);
    const auto addr = args.Next(), length = args.Next(), host = args.Next(), hostlen = args.Next();
    const auto service = args.Next(), servlen = args.Next(), flags = args.Next();
    std::array<char, NI_MAXHOST> host_text;
    std::array<char, NI_MAXSERV> service_text;
    Address address;
    if (!address.Read(mem, addr, length)) {
        ctx.ret0 = 5;
        return;
    }
    int translated = 0;
    if (flags & 1)
        translated |= NI_NOFQDN;
    if (flags & 2)
        translated |= NI_NUMERICHOST;
    if (flags & 4)
        translated |= NI_NAMEREQD;
    if (flags & 8)
        translated |= NI_NUMERICSERV;
    if (flags & 16)
        translated |= NI_DGRAM;
    const int result = getnameinfo(address.data(), address.size, host ? host_text.data() : nullptr,
                                   std::min<uint64_t>(hostlen, host_text.size()),
                                   service ? service_text.data() : nullptr,
                                   std::min<uint64_t>(servlen, service_text.size()), translated);
    ctx.ret0 = GuestGai(result);
    if (result == EAI_SYSTEM)
        SetErrno(*ctx.dbt, GuestErrno(errno));
    if (!result) {
        if (host)
            mem.Write(host, host_text.data(), std::strlen(host_text.data()) + 1);
        if (service)
            mem.Write(service, service_text.data(), std::strlen(service_text.data()) + 1);
    }
}

template <bool Parse> void Inet(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto family = HostFamily(ctx.args[0]);
    if (family != AF_INET && family != AF_INET6) {
        SetErrno(*ctx.dbt, GuestErrno(EAFNOSUPPORT));
        if constexpr (Parse)
            ctx.ret0 = UINT64_MAX;
        return;
    }
    std::array<uint8_t, 16> numeric{};
    const auto size = family == AF_INET ? 4 : 16;
    if constexpr (Parse) {
        const auto text = mem.String(ctx.args[1]);
        const auto result = inet_pton(family, text.c_str(), numeric.data());
        if (result == 1)
            mem.Write(ctx.args[2], numeric.data(), size);
        PosixResult(ctx, result);
    } else {
        std::array<char, INET6_ADDRSTRLEN> text;
        mem.Read(ctx.args[1], numeric.data(), size);
        if (!inet_ntop(family, numeric.data(), text.data(),
                       std::min<uint64_t>(ctx.args[3], text.size()))) {
            SetErrno(*ctx.dbt, GuestErrno(errno));
            return;
        }
        mem.Write(ctx.args[2], text.data(), std::strlen(text.data()) + 1);
        ctx.ret0 = ctx.args[2];
    }
}

void InetAddr(wa_host_call_ctx_t& ctx) {
    const auto text = GuestMemory(ctx).String(ctx.args[0]);
    ctx.ret0 = inet_addr(text.c_str());
}

void InetAton(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto text = mem.String(ctx.args[0]);
    in_addr address{};
    ctx.ret0 = inet_aton(text.c_str(), &address);
    if (ctx.ret0 && ctx.args[1]) mem.Write<uint32_t>(ctx.args[1], address.s_addr);
}

void InetNtoa(wa_host_call_ctx_t& ctx) {
    in_addr address{.s_addr = static_cast<uint32_t>(ctx.args[0])};
    std::array<char, INET_ADDRSTRLEN> text;
    if (!inet_ntop(AF_INET, &address, text.data(), text.size()))
        SetErrno(*ctx.dbt, GuestErrno(errno));
    else
        ctx.ret0 = InternString(*ctx.dbt, text.data());
}

void ServiceByPort(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto protocol = ctx.args[1] ? mem.String(ctx.args[1]) : "";
    const auto* item = getservbyport(ctx.args[0], ctx.args[1] ? protocol.c_str() : nullptr);
    if (!item)
        return;
    auto& slot = ctx.dbt->bionic->servent;
    if (!slot)
        slot = AllocateGuest(*ctx.dbt, 32 * mem.width());
    if (!slot)
        throw GuestFault{WA_ERR_NOMEM};
    const auto width = mem.width();
    const auto aliases = slot + 4 * width;
    mem.WriteWord(slot, InternString(*ctx.dbt, item->s_name));
    mem.WriteWord(slot + width, aliases);
    mem.Write<int32_t>(slot + 2 * width, item->s_port);
    mem.WriteWord(slot + 3 * width, InternString(*ctx.dbt, item->s_proto));
    size_t count = 0;
    if (item->s_aliases)
        for (; count < 27 && item->s_aliases[count]; ++count)
            mem.WriteWord(aliases + count * width, InternString(*ctx.dbt, item->s_aliases[count]));
    mem.WriteWord(aliases + count * width, 0);
    ctx.ret0 = slot;
}

void InterfaceIndex(wa_host_call_ctx_t& ctx) {
    const auto name = GuestMemory(ctx).String(ctx.args[0]);
    const auto result = if_nametoindex(name.c_str());
    if (!result)
        SetErrno(*ctx.dbt, GuestErrno(errno));
    ctx.ret0 = result;
}

void InterfaceName(wa_host_call_ctx_t& ctx) {
    std::array<char, IF_NAMESIZE> name{};
    if (!if_indextoname(ctx.args[0], name.data())) {
        SetErrno(*ctx.dbt, GuestErrno(errno));
        return;
    }
    GuestMemory(ctx).Write(ctx.args[1], name.data(), std::strlen(name.data()) + 1);
    ctx.ret0 = ctx.args[1];
}

struct GuestMessage {
    wa_host_call_ctx_t& ctx;
    GuestMemory mem;
    uint64_t header;
    Address address;
    std::vector<iovec> vectors;
    std::vector<std::vector<uint8_t>> buffers;
    std::vector<uint64_t> locations;
    msghdr host{};
    uint64_t name, capacity;
    size_t size;

    GuestMessage(wa_host_call_ctx_t& ctx, uint64_t header) : ctx(ctx), mem(ctx), header(header) {}

    bool Load(bool write) {
        const auto width = mem.width();
        size = width == 8 ? 56 : 28;
        name = mem.ReadWord(header);
        capacity = mem.Read<uint32_t>(header + width);
        const auto iov = mem.ReadWord(header + 2 * width), count = mem.ReadWord(header + 3 * width);
        const auto control = mem.ReadWord(header + 4 * width),
                   length = mem.ReadWord(header + 5 * width);
        if (count > 1024 || (write && control && length)) {
            errno = count > 1024 ? EINVAL : EOPNOTSUPP;
            return false;
        }
        if (name) {
            if (write && !address.Read(mem, name, capacity))
                return false;
            host.msg_name = address.data();
            host.msg_namelen = write ? address.size : sizeof(address.value);
        }
        vectors.resize(count);
        buffers.resize(count);
        locations.resize(count);
        size_t remaining = kPacketLimit;
        for (size_t i = 0; i < count; ++i) {
            const auto entry = iov + i * 2 * width;
            const auto length = mem.ReadWord(entry + width);
            if (length > remaining) {
                errno = EMSGSIZE;
                return false;
            }
            remaining -= length;
            locations[i] = mem.ReadWord(entry);
            buffers[i].resize(length);
            vectors[i] = {buffers[i].data(), buffers[i].size()};
            if (write)
                mem.Read(locations[i], vectors[i].iov_base, length);
        }
        host.msg_iov = vectors.data();
        host.msg_iovlen = vectors.size();
        return true;
    }

    void Store(size_t received) {
        if (name) {
            Address::Write(mem, name, address.data(), host.msg_namelen,
                           std::min<uint64_t>(capacity, sizeof(address.value)));
            mem.Write<uint32_t>(header + mem.width(), host.msg_namelen);
        }
        mem.WriteWord(header + 5 * mem.width(), 0);
        int flags = 0;
        if (host.msg_flags & MSG_TRUNC)
            flags |= 0x20;
        if (host.msg_flags & MSG_CTRUNC)
            flags |= 0x8;
        if (host.msg_flags & MSG_EOR)
            flags |= 0x80;
        mem.Write<int32_t>(header + 6 * mem.width(), flags);
        for (size_t i = 0; i < vectors.size() && received; ++i) {
            const auto count = std::min(received, vectors[i].iov_len);
            mem.Write(locations[i], vectors[i].iov_base, count);
            received -= count;
        }
    }
};

template <bool Send, bool Multiple = false> void Message(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto fd = HostFd(*ctx.dbt, ctx.args[0]);
    if (fd < 0)
        return PosixResult(ctx, -1);
    const auto flags = MessageFlags(ctx.args[Multiple ? 3 : 2]);
    const auto count = Multiple ? ctx.args[2] : 1;
    const size_t stride = mem.width() == 8 ? 64 : 32;
    if (count > 1024)
        return Fail(ctx, EINVAL);
    for (uint64_t i = 0; i < count; ++i) {
        const auto header = ctx.args[1] + i * stride;
        GuestMessage message(ctx, header);
        if (!message.Load(Send)) {
            if (!i)
                PosixResult(ctx, -1);
            return;
        }
        const auto received = Send ? sendmsg(fd, &message.host, flags)
                                   : recvmsg(fd, &message.host, flags | (i ? MSG_DONTWAIT : 0));
        if (received < 0) {
            if (!i)
                PosixResult(ctx, received);
            return;
        }
        if constexpr (!Send)
            message.Store(received);
        if constexpr (Multiple) {
            mem.Write<uint32_t>(header + message.size, received);
            ++ctx.ret0;
        } else {
            ctx.ret0 = received;
        }
    }
}

void CmsgNext(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto header = ctx.args[0], current = ctx.args[1];
    const auto width = mem.width();
    const auto control = mem.ReadWord(header + 4 * width);
    const auto size = mem.ReadWord(header + 5 * width);
    const auto minimum = width + 8;
    if (!control || current < control || size > UINT32_MAX || current - control > size ||
        minimum > size - (current - control))
        return;
    const auto length = mem.ReadWord(current);
    if (length < minimum || length > size - (current - control))
        return;
    const auto next = current + AlignUp(length, width);
    if (next - control <= size && minimum <= size - (next - control))
        ctx.ret0 = next;
}

template <auto F> void PairInts(wa_host_call_ctx_t& ctx) {
    PosixResult(ctx, F(HostFd(*ctx.dbt, ctx.args[0]), ctx.args[1]));
}

} // namespace

void RegisterNetwork(wa_dbt& dbt) {
    const Builtin c[] = {
        {"socket", Dispatch<Socket<false>>},
        {"socketpair", Dispatch<Socket<true>>},
        {"connect", Dispatch<SetAddress<false>>},
        {"bind", Dispatch<SetAddress<true>>},
        {"listen", Dispatch<PairInts<::listen>>},
        {"shutdown", Dispatch<PairInts<::shutdown>>},
        {"accept", Dispatch<GetAddress<Addr::Accept>>},
        {"accept4", Dispatch<GetAddress<Addr::Accept4>>},
        {"getsockname", Dispatch<GetAddress<Addr::Local>>},
        {"getpeername", Dispatch<GetAddress<Addr::Peer>>},
        {"send", Dispatch<Transfer<Packet::Send>>},
        {"recv", Dispatch<Transfer<Packet::Receive>>},
        {"sendto", Dispatch<Transfer<Packet::SendTo>>},
        {"recvfrom", Dispatch<Transfer<Packet::ReceiveFrom>>},
        {"setsockopt", Dispatch<Options<true>>},
        {"getsockopt", Dispatch<Options<false>>},
        {"getaddrinfo", Dispatch<GetAddrInfo>},
        {"gethostbyname", Dispatch<GetHostByName>},
        {"__get_h_errno", Dispatch<HostErrno>},
        {"getifaddrs", Dispatch<GetInterfaces>},
        {"freeifaddrs", Dispatch<FreeInterfaces>},
        {"freeaddrinfo", Dispatch<FreeAddrInfo>},
        {"getnameinfo", Dispatch<GetNameInfo>},
        {"gai_strerror", Dispatch<GaiError>},
        {"inet_addr", Dispatch<InetAddr>},
        {"inet_aton", Dispatch<InetAton>},
        {"inet_ntoa", Dispatch<InetNtoa>},
        {"inet_pton", Dispatch<Inet<true>>},
        {"inet_ntop", Dispatch<Inet<false>>},
        {"getservbyport", Dispatch<ServiceByPort>},
        {"if_nametoindex", Dispatch<InterfaceIndex>},
        {"if_indextoname", Dispatch<InterfaceName>},
        {"sendmsg", Dispatch<Message<true>>},
        {"sendmmsg", Dispatch<Message<true, true>>},
        {"recvmsg", Dispatch<Message<false>>},
        {"recvmmsg", Dispatch<Message<false, true>>},
        {"__cmsg_nxthdr", Dispatch<CmsgNext>},
    };
    Register(dbt, "libc.so", c);
    const auto any = AllocateGuest(dbt, 32);
    if (!any)
        throw GuestFault{WA_ERR_NOMEM};
    const std::array<uint8_t, 16> loopback{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    GuestMemory(dbt.mmu, dbt.arch).Write(any + 16, loopback.data(), loopback.size());
    CheckGuest(RegisterData(dbt, "libc.so", "in6addr_any", any));
    CheckGuest(RegisterData(dbt, "libc.so", "in6addr_loopback", any + 16));
}

} // namespace wa::dbt
