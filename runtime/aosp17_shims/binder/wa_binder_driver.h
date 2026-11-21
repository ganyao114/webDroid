#pragma once

#include <new>

#include <utils/StrongPointer.h>
#include <wasmandroid/libkern.h>

namespace android {
class BBinder;
}

namespace wa::aosp_binder {

struct Endpoint {
    wa_pid_t pid;
    uint32_t node;

    bool operator==(const Endpoint&) const = default;
};

// Attach before opening /dev/binder. Native threads use the same ring storage.
// A null base on Wasm selects the vproc's M0 host bridge.
int32_t attach(void* base, size_t bytes, wa_pid_t pid);

// Publish a local Binder node and import its endpoint as a ProcessState handle.
// The device owns published objects for the lifetime of the vproc.
Endpoint publish(const android::sp<android::BBinder>& binder);
int32_t handleFor(Endpoint endpoint);

// Leave the current looper after completing the active transaction and reply.
void stopLooper();

} // namespace wa::aosp_binder
