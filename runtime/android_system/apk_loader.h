#pragma once

#include "system_entry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wa_system_apk wa_system_apk_t;

typedef struct wa_system_apk_info {
    const char* package_name;
    const char* version_name;
    const char* launcher_activity;
    const char* native_library;
    const char* label_en_us;
    const char* label_zh_cn;
    uint32_t version_code;
    size_t arm32_library_size;
    size_t arm64_library_size;
} wa_system_apk_info_t;

typedef struct wa_system_apk_dex_stats {
    uint32_t methods;
    uint32_t instructions_before;
    uint32_t instructions_after_folding;
    uint32_t instructions_after_dce;
    uint32_t blocks_before;
    uint32_t blocks_after_dce;
} wa_system_apk_dex_stats_t;

// The handle belongs to one worker and owns a copy of the APK. A null return
// indicates an unreadable APK.
wa_system_apk_t* wa_system_open_apk(const uint8_t* apk_bytes, size_t apk_len);
void wa_system_close_apk(wa_system_apk_t* apk);
const wa_system_apk_info_t* wa_system_apk_info(const wa_system_apk_t* apk);

// Compiles the launcher's primitive static methods, using its nested Wasm class
// when present. The result and metadata pointers are owned by the APK handle.
int32_t wa_system_apk_compile_dex(wa_system_apk_t* apk, const uint8_t** out_wasm,
                                 size_t* out_wasm_len);
const wa_system_apk_dex_stats_t* wa_system_apk_dex_stats(const wa_system_apk_t* apk);

// Optionally page stored ELF entries from a VFS descriptor for the same APK.
// Keep the descriptor open until its guests are destroyed. A negative fd selects memory.
int32_t wa_system_apk_set_backing_file(wa_system_apk_t* apk, int32_t fd);
// Accepts "armeabi-v7a" or "arm64-v8a". Destroy the result with wa_system_destroy.
wa_system_guest_t* wa_system_apk_load_native(const wa_system_apk_t* apk, const char* abi);

#ifdef __cplusplus
} // extern "C"
#endif
