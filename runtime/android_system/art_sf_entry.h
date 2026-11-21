#pragma once

#include "wasmandroid/abi.h"
#include "wasmandroid/surfaceflinger.h"

#ifdef __cplusplus
extern "C" {
#endif

// One ART and SurfaceFlinger context per module. Use them from the owning worker.
int32_t wa_system_art_create(uint32_t gc_interval);
void wa_system_art_destroy(void);
int32_t wa_system_art_load_dex(const char* location, const uint8_t* bytes, size_t len);
int32_t wa_system_art_register_resource_natives(void);

// Writes a NUL-terminated path to the APK in MEMFS (a temporary file on native).
// Load the resource bridge DEX first. The path remains valid until ART is destroyed.
int32_t wa_system_art_mount_apk(const uint8_t* bytes, size_t len, char* path, size_t cap);

// Calls take no Java arguments. The int variant returns zero on failure and sets
// the last error. The string and void variants return WA_OK or a negative status.
int32_t wa_system_art_call_static_int(const char* owner, const char* name, const char* sig);
int32_t wa_system_art_call_static_string(const char* owner, const char* name, const char* sig,
                                         char* out, size_t cap);
int32_t wa_system_art_call_static_void(const char* owner, const char* name, const char* sig);
int32_t wa_system_art_last_error(char* out, size_t cap);
int32_t wa_system_art_stats_json(char* out, size_t cap);
int32_t wa_system_art_gc_collect(void);
int32_t wa_system_art_set_gc_paused(int32_t paused);
int32_t wa_system_art_set_inline_caches_enabled(int32_t enabled);

// density is a scale factor, where 1.0 is 160 dpi. Init publishes an empty scene.
int32_t wa_system_sf_init(int32_t width, int32_t height, float density);
wa_sf_t* wa_system_sf_ctx(void);
void wa_system_sf_destroy(void);

#ifdef __cplusplus
}  // extern "C"
#endif
