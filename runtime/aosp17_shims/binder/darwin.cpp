#include "darwin.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#undef socket
#undef socketpair
#undef accept4
#undef recvmsg

#ifdef WA_DARWIN_SOCKET_FLAGS
namespace {

constexpr int kSocketFlags = SOCK_CLOEXEC | SOCK_NONBLOCK;

bool configureSocket(int fd, int flags) {
    if ((flags & SOCK_CLOEXEC) && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        return false;
    if (flags & SOCK_NONBLOCK) {
        const int oldFlags = fcntl(fd, F_GETFL);
        if (oldFlags < 0 || fcntl(fd, F_SETFL, oldFlags | O_NONBLOCK) < 0)
            return false;
    }
    return true;
}

int configureOrClose(int fd, int flags) {
    if (fd < 0 || configureSocket(fd, flags))
        return fd;
    const int error = errno;
    close(fd);
    errno = error;
    return -1;
}

} // namespace

int wa_binder_socket(int domain, int type, int protocol) {
    return configureOrClose(socket(domain, type & ~kSocketFlags, protocol), type);
}

int wa_binder_socketpair(int domain, int type, int protocol, int sockets[2]) {
    if (socketpair(domain, type & ~kSocketFlags, protocol, sockets) < 0)
        return -1;
    if (configureSocket(sockets[0], type) && configureSocket(sockets[1], type))
        return 0;
    const int error = errno;
    close(sockets[0]);
    close(sockets[1]);
    errno = error;
    return -1;
}

int wa_binder_accept4(int fd, sockaddr* address, socklen_t* length, int flags) {
    return configureOrClose(accept(fd, address, length), flags);
}
#endif

#ifdef WA_DARWIN_MSG_CMSG_CLOEXEC
ssize_t wa_binder_recvmsg(int fd, msghdr* message, int flags) {
    const ssize_t result = recvmsg(fd, message, flags & ~MSG_CMSG_CLOEXEC);
    if (result < 0 || !(flags & MSG_CMSG_CLOEXEC))
        return result;
    for (auto* control = CMSG_FIRSTHDR(message); control; control = CMSG_NXTHDR(message, control)) {
        if (control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_RIGHTS ||
            control->cmsg_len < CMSG_LEN(0))
            continue;
        const auto* received = CMSG_DATA(control);
        const size_t count = (control->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; ++i) {
            int receivedFd;
            std::memcpy(&receivedFd, received + i * sizeof(int), sizeof(int));
            TEMP_FAILURE_RETRY(fcntl(receivedFd, F_SETFD, FD_CLOEXEC));
        }
    }
    return result;
}
#endif
