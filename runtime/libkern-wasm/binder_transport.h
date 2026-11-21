#pragma once

#include "wasmandroid/libkern.h"

#ifdef __cplusplus
extern "C" {
#endif

// Used by the worker's instance on the shared Binder memory (WA_MEM_BINDER).
// A nonnegative frame offset points at wa_binder_msg_t, immediately followed
// by payload_len bytes. Only the reservation or claim owner may access the
// frame. Commit, reply and release transfer ownership away from that caller.
int32_t wa_binder_ring_attach(void* base, size_t bytes);
int32_t wa_binder_ring_reserve(void* base, wa_pid_t src, wa_pid_t dst, uint32_t txn,
                               uint32_t code, uint32_t flags, uint32_t length);
int32_t wa_binder_ring_commit(void* base, uint32_t frame);
int32_t wa_binder_ring_claim(void* base, wa_pid_t dst, uint32_t max_payload, int32_t timeout_ms);
int32_t wa_binder_ring_reply(void* base, uint32_t frame, uint32_t code, uint32_t flags);
int32_t wa_binder_ring_release(void* base, uint32_t frame);

#ifdef __cplusplus
}
#endif
