#pragma once

#include "art_interp.h"
#include "wasmandroid/abi.h"

namespace wa::system {

std::unique_ptr<interp::GuestNativeLibraries> OpenGuestNativeApk(JNIEnv* env, const void* mounted_apk);

enum class NativeProbeAction { Lookup, CallString, CallJniOnLoad };
struct NativeProbeRequest {
    std::string_view library;
    std::string_view symbol;
    NativeProbeAction action;
};
struct NativeEntrypointProbe {
    std::string name;
    uint64_t address = 0;
    std::string value;
    std::string diagnostic;
};

struct NativeLibraryProbe {
    std::string name;
    int32_t elf_status = WA_OK;
    uint64_t mapped_base = 0;
    std::string load_error;
    std::vector<std::string> needed;
    std::vector<std::string> missing_libraries;
    std::vector<std::string> resolved;
    std::vector<std::string> missing;
    std::vector<std::string> weak_undefined;
    std::vector<NativeEntrypointProbe> entrypoints;
};

std::vector<NativeLibraryProbe> ProbeApkNativeLibraries(const std::string& path, wa_guest_arch_t arch,
    std::span<const NativeProbeRequest> requests = {}, JNIEnv* environment = nullptr);

} // namespace wa::system
