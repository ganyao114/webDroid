#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WA_ABI_VERSION 1u

// Standardized error codes (negative errno-compatible values).
typedef enum wa_status {
    WA_OK = 0,
    WA_ERR_INVALID = -22,    // EINVAL
    WA_ERR_NOMEM = -12,      // ENOMEM
    WA_ERR_NOENT = -2,       // ENOENT
    WA_ERR_AGAIN = -11,      // EAGAIN
    WA_ERR_BADF = -9,        // EBADF
    WA_ERR_FAULT = -14,      // EFAULT
    WA_ERR_BUSY = -16,       // EBUSY
    WA_ERR_EXIST = -17,      // EEXIST
    WA_ERR_NOSYS = -38,      // ENOSYS
    WA_ERR_TIMEDOUT = -110,  // ETIMEDOUT
    WA_ERR_NOTSUP = -95,     // EOPNOTSUPP
    WA_ERR_OVERFLOW = -75,   // EOVERFLOW
    WA_ERR_INTERNAL = -1000,
} wa_status_t;

typedef uint32_t wa_pid_t;
typedef uint32_t wa_tid_t;
typedef uint32_t wa_fd_t;

typedef enum wa_guest_arch {
    WA_ARCH_ARM32 = 1,  // armeabi-v7a (ILP32)
    WA_ARCH_ARM64 = 2,  // arm64-v8a (LP64)
} wa_guest_arch_t;

// Reserved synthetic PC range for Host-Call FFI stubs (never mapped as guest code).
#define WA_HOST_STUB_BASE 0xFFFF0000u
#define WA_HOST_STUB_STRIDE 4u
#define WA_HOST_STUB_LIMIT 0xFFFFFF00u
#define WA_EXIT_SENTINEL_PC 0xFFFFFFFCu

// Multi-Memory logical indices (or shared SAB handles when emulated).
#define WA_MEM_PRIVATE 0u
#define WA_MEM_BINDER 1u
#define WA_MEM_ASHMEM_RO 2u

#define WA_PAGE_SIZE 4096u
#define WA_PAGE_SHIFT 12u

#ifdef __cplusplus
}  // extern "C"
#endif
