#pragma once

#include "wasmandroid/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- Futex (maps to memory.atomic.wait32 / memory.atomic.notify in Wasm Workers) ----
int32_t wa_futex_wait(volatile uint32_t* addr, uint32_t expected, int64_t timeout_ns);
int32_t wa_futex_wake(volatile uint32_t* addr, uint32_t max_wake);

// ---- Eventing: eventfd, pipe, epoll ----
#define WA_EPOLLIN 0x001u
#define WA_EPOLLOUT 0x004u
#define WA_EPOLLERR 0x008u
#define WA_EPOLLHUP 0x010u

#define WA_EPOLL_CTL_ADD 1
#define WA_EPOLL_CTL_DEL 2
#define WA_EPOLL_CTL_MOD 3

typedef struct wa_epoll_event {
    uint32_t events;
    uint32_t _pad;
    uint64_t data;
} wa_epoll_event_t;

int32_t wa_eventfd(uint64_t initval, uint32_t flags);
int32_t wa_pipe(int32_t pipefd[2]);
int32_t wa_epoll_create(uint32_t flags);
int32_t wa_epoll_ctl(int32_t epfd, int32_t op, int32_t fd, const wa_epoll_event_t* event);
int32_t wa_epoll_wait(int32_t epfd, wa_epoll_event_t* events, int32_t maxevents, int32_t timeout_ms);

// ---- OPFS-backed VFS (SyncAccessHandle inside Web Worker) ----
#define WA_O_RDONLY 0x0000u
#define WA_O_WRONLY 0x0001u
#define WA_O_RDWR 0x0002u
#define WA_O_CREAT 0x0040u
#define WA_O_TRUNC 0x0200u
#define WA_O_APPEND 0x0400u

typedef struct wa_stat {
    uint64_t size;
    uint32_t mode;
    uint32_t _pad;
    int64_t mtime_ns;
} wa_stat_t;

int32_t wa_vfs_open(const char* path, uint32_t flags, uint32_t mode);
int64_t wa_vfs_read(int32_t fd, void* buf, size_t count);
int64_t wa_vfs_write(int32_t fd, const void* buf, size_t count);
int64_t wa_vfs_pread(int32_t fd, void* buf, size_t count, int64_t offset);
int64_t wa_vfs_pwrite(int32_t fd, const void* buf, size_t count, int64_t offset);
int32_t wa_vfs_ftruncate(int32_t fd, int64_t length);
int32_t wa_vfs_fsync(int32_t fd);
int32_t wa_vfs_fstat(int32_t fd, wa_stat_t* st);
int32_t wa_vfs_flock(int32_t fd, int32_t operation);
int32_t wa_vfs_close(int32_t fd);

// ---- Cross-vproc Binder Ring (zero-copy over shared Binder SAB / Memory 1) ----
#define WA_BINDER_MAX_PAYLOAD 65536u
#define WA_BINDER_RING_MAGIC 0x57414252u  // 'WABR'

typedef struct wa_binder_msg {
    wa_pid_t src_pid;
    wa_pid_t dst_pid;
    uint32_t txn_id;
    uint32_t code;
    uint32_t flags;
    uint32_t payload_len;
} wa_binder_msg_t;

int32_t wa_binder_init_ring(void* shared_ring_base, size_t ring_bytes, wa_pid_t self_pid);
int32_t wa_binder_send(wa_pid_t dst_pid, uint32_t txn_id, uint32_t code, uint32_t flags,
                       const void* payload, uint32_t payload_len);
int32_t wa_binder_recv(wa_binder_msg_t* out_hdr, void* out_payload, uint32_t max_payload,
                       int32_t timeout_ms);

// ---- Unified functional syscall gateway (0 svc traps) ----
int64_t wa_syscall(uint32_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                   uint64_t a5);

#ifdef __cplusplus
}  // extern "C"
#endif
