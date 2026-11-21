#pragma once

#include "wasmandroid/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Select the Linux syscall numbering for the current guest. The default is ARM64.
// A Wasm vproc configures its guest architecture before running any syscalls.
int32_t wa_syscall_set_arch(wa_guest_arch_t arch);

#ifdef __cplusplus
}
#endif
