#pragma once

#include <cerrno>
#include <sys/socket.h>

#include <android-base/macros.h>

// Darwin configures close-on-exec and nonblocking mode after socket creation.
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0x80000
#define WA_DARWIN_SOCKET_FLAGS
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x800
#endif
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0x40000000
#define WA_DARWIN_MSG_CMSG_CLOEXEC
#endif

#ifdef WA_DARWIN_SOCKET_FLAGS
int wa_binder_socket(int domain, int type, int protocol);
int wa_binder_socketpair(int domain, int type, int protocol, int sockets[2]);
int wa_binder_accept4(int fd, sockaddr* address, socklen_t* length, int flags);
#define socket(...) wa_binder_socket(__VA_ARGS__)
#define socketpair(...) wa_binder_socketpair(__VA_ARGS__)
#define accept4(...) wa_binder_accept4(__VA_ARGS__)
#endif

#ifdef WA_DARWIN_MSG_CMSG_CLOEXEC
ssize_t wa_binder_recvmsg(int fd, msghdr* message, int flags);
#define recvmsg(...) wa_binder_recvmsg(__VA_ARGS__)
#endif
