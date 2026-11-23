#pragma once

#include "wasmandroid/abi.h"
#include "wasmandroid/surfaceflinger.h"

#ifdef __cplusplus
extern "C" {
#endif

// One foreground app per android_system module. Call on the owning VM thread.
// A boot attempt consumes the prepared process. Destroy it before booting again.
int32_t wa_system_app_create(int32_t width, int32_t height, int32_t density_dpi,
                            const char* locale);
void wa_system_app_destroy(void);
wa_sf_t* wa_system_app_sf_ctx(void);

// Inputs are copied or mounted before these calls return. Load the core before
// the optional fallback. Register catalog packages before booting the activity.
int32_t wa_system_app_load_framework_dex(const uint8_t* bytes, size_t size);
int32_t wa_system_app_load_framework_fallback_dex(const uint8_t* bytes, size_t size);
int32_t wa_system_app_set_framework_res_apk(const uint8_t* bytes, size_t size);
int32_t wa_system_app_install_package_catalog_apk(const char* name, const uint8_t* bytes,
                                                size_t size);
int32_t wa_system_app_boot_apk(const char* name, const uint8_t* bytes, size_t size,
                               const char* activity_class_or_null);

// Input goes through wa_sf_input(). A busy scene can be retried on the next vsync.
int32_t wa_system_app_tick(int64_t frame_time_ns);
int32_t wa_system_app_resize(int32_t width, int32_t height, int32_t density_dpi);
// Returns 1 if the activity is still running, 0 after it finishes, or a WA_ERR_*.
int32_t wa_system_app_send_back(void);

// Read a file requested by a locally navigated WebView from guest storage or the app APK.
// A null buffer and zero capacity return the required byte count.
int32_t wa_system_app_read_webview_file(uint32_t view, const char* url, uint8_t* out, size_t cap);

// NUL-terminated UTF-8. Returns the byte length, or a negative WA_ERR_* status.
int32_t wa_system_app_state_json(char* out, size_t cap);
int32_t wa_system_app_last_error(char* out, size_t cap);

#ifdef __cplusplus
}
#endif
