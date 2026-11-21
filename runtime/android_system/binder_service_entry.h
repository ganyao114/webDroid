#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One Binder PID per native process or Wasm instance. Pass NULL in Wasm to use
// the loader's binderHost imports for a ring in separate shared memory.
int32_t wa_system_binder_attach(void* ring_base, size_t ring_bytes, uint32_t pid);

// Start and pump on the same thread. Each pump waits up to timeout_ms for
// incoming work. Zero polls once. Binder dispatch runs on the calling thread.
int32_t wa_system_servicemanager_start(void);
int32_t wa_system_servicemanager_pump(int timeout_ms);
int32_t wa_system_server_start(void);
int32_t wa_system_server_pump(int timeout_ms);

// Binder client calls return WA_OK or a negative WA_ERR_* status. JSON is
// UTF-8 and NUL-terminated. WA_ERR_OVERFLOW leaves the output empty. A
// mutation may have completed before its result was serialized.
int32_t wa_system_client_install_apk(const uint8_t* apk_bytes, size_t apk_len, const char* apk_path,
                                    char* json_out, size_t json_cap);
int32_t wa_system_client_query_launcher(char* json_out, size_t json_cap);
int32_t wa_system_client_resolve_home_activity(char* json_out, size_t json_cap);
int32_t wa_system_client_get_package_info(const char* package_name, char* json_out, size_t json_cap);
int32_t wa_system_client_start_activity(const char* package_name, const char* activity_name,
                                       char* json_out, size_t json_cap);
int32_t wa_system_client_start_home_activity(char* json_out, size_t json_cap);
int32_t wa_system_client_get_top_activity(char* json_out, size_t json_cap);
int32_t wa_system_client_finish_top_activity(void);

#ifdef __cplusplus
} // extern "C"
#endif
