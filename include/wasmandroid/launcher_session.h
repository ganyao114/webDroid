#pragma once

#include "wasmandroid/abi.h"
#include "wasmandroid/surfaceflinger.h"

#ifdef __cplusplus
extern "C" {
#endif

// C ABI for booting upstream AOSP 17 Launcher3 and SystemUI inside unified
// android_system.wasm and driving it at 60 FPS through wa_hal_surfaceflinger.
//
// Typical lifecycle in a browser worker or native host:
//   1. wa_system_launcher_create(width, height, density_dpi, "en-US")
//   2. wa_system_launcher_load_framework_dex(fw_dex, fw_len)
//      wa_system_launcher_load_framework_fallback_dex(upstream_dex, upstream_len)
//   3. wa_system_launcher_set_framework_res_apk(framework_res_bytes, res_len)
//   4. wa_system_launcher_install_apk("wa_cube_app.apk", cube_bytes, cube_len) [1..N installed apps]
//   5. wa_system_launcher_load_systemui_apk("SystemUI-upstream.apk", systemui_bytes, systemui_len)
//   6. wa_system_launcher_stage_apk("Launcher3-upstream.apk", launcher_bytes, launcher_len)
//      Release the APK upload buffer, then call wa_system_launcher_boot_staged_apk().
//   7. Per Choreographer vsync (60 Hz):
//        wa_system_launcher_tick(frame_time_ns)
//      Compositor reads wa_sf_frame_acquire(wa_system_launcher_sf_ctx(), ...).
//      Browser input pushes MotionEvent/KeyEvent/IME into wa_sf_input(wa_system_launcher_sf_ctx()).
//   8. Poll wa_system_launcher_consume_launch(json, cap) to detect when the user
//      tapped an app icon in Launcher3 (e.g. "org.wasmandroid.cube/.CubeActivity").
//   9. Call wa_system_launcher_send_home() or wa_system_launcher_resume() when
//      returning from a launched app to Launcher3.

int32_t wa_system_launcher_create(int32_t width, int32_t height, int32_t density_dpi,
                                  const char* locale);
void wa_system_launcher_destroy(void);

// Returns the SurfaceFlinger context owned by the active Launcher3 session
// (compatible with all wa_sf_* / wa_bitmap_* accessors in surfaceflinger.h).
wa_sf_t* wa_system_launcher_sf_ctx(void);

int32_t wa_system_launcher_load_framework_dex(const uint8_t* dex_bytes, size_t dex_len);
int32_t wa_system_launcher_load_framework_fallback_dex(const uint8_t* dex_bytes, size_t dex_len);
int32_t wa_system_launcher_set_framework_res_apk(const uint8_t* apk_bytes, size_t apk_len);

// Mounts and indexes an installed app APK (e.g. wa_cube_app.apk or system app)
// via real upstream aosp17_libandroidfw so Launcher3 discovers it through
// PackageManager / LauncherApps. Optional label/icon fallback overrides are
// ignored when the APK manifest supplies real strings/icons.
int32_t wa_system_launcher_install_apk(const char* name, const uint8_t* apk_bytes, size_t apk_len);
int32_t wa_system_launcher_install_apk_file(const char* name, const char* path);

// Mounts the upstream com.android.systemui package before boot. Its DEX and
// resources are used to inflate the real phone bars into the Launcher window.
int32_t wa_system_launcher_load_systemui_apk(const char* name, const uint8_t* apk_bytes, size_t apk_len);

// Mounts Launcher3-upstream.apk. The caller can release apk_bytes after this returns.
int32_t wa_system_launcher_stage_apk(const char* name, const uint8_t* apk_bytes, size_t apk_len);
// Loads all staged DEX files, registers aosp17_core_jni +
// aosp17_ui_jni natives, and launches com.android.launcher3.Launcher through
// real onCreate -> onStart -> onResume + initial measure/layout/draw frame.
int32_t wa_system_launcher_boot_staged_apk(void);
// Convenience entry point for native callers that already own the APK bytes.
int32_t wa_system_launcher_boot_apk(const char* name, const uint8_t* apk_bytes, size_t apk_len);

// Drains wa_sf_input() events, advances Looper/Handler/Choreographer/ValueAnimator,
// runs measure/layout/draw with GC paused across frame emission, and commits an
// atomic SurfaceFlinger frame packet. Returns WA_OK or negative error code.
int32_t wa_system_launcher_tick(int64_t frame_time_ns);

// Lifecycle / navigation helpers.
int32_t wa_system_launcher_resize(int32_t width, int32_t height, int32_t density_dpi);
int32_t wa_system_launcher_pause(void);
int32_t wa_system_launcher_resume(void);
int32_t wa_system_launcher_send_home(void);
int32_t wa_system_launcher_send_back(void);
int32_t wa_system_launcher_open_all_apps(void);

// Switches SystemUI between three-button navigation (0, default) and the
// upstream gestural navigation handle (1).
int32_t wa_system_launcher_set_gesture_navigation(int32_t enabled);
// Returns 1 once per pending SystemUI Recents request, or 0 when idle.
int32_t wa_system_launcher_consume_recents(void);
// Sends WA_ACTION_DOWN/MOVE/UP/CANCEL to the SystemUI overlay while the
// Launcher activity is paused. Returns the resulting Android key code or 0.
int32_t wa_system_launcher_touch_navigation(int64_t time_ms, int32_t action, float x, float y);

// Copies JSON describing the latest Launcher3 state into out:
//   {"booted":true,"package":"com.android.launcher3","activity":"com.android.launcher3.Launcher",
//    "frames":120,"lastFrameMs":1.42,"avgFrameMs":1.35,"p95FrameMs":1.80,
//    "launcherUpstream":true,"launcherDexCount":4,"launcherClassCount":37050,
//    "installedApps":6,"workspacePage":0,"pageCount":2,"drawerOpen":false,
//    "drawerProgress":0.0,"searchQuery":"","lastDrawCommands":184,
//    "systemUiMounted":true,"systemUiClass":"com.android.systemui.statusbar.phone.PhoneStatusBarView",
//    "gcLive":412,"gcCollections":3,"inputEventsProcessed":14,"launchCount":0}
int32_t wa_system_launcher_state_json(char* out, size_t cap);

// If Launcher3 invoked startActivity(Intent) since the last poll, writes JSON:
//   {"pending":true,"package":"org.wasmandroid.cube","activity":"org.wasmandroid.cube.CubeActivity",
//    "apkName":"wa_cube_app.apk","label":"WasmAndroid 3D Cube"}
// and clears the pending launch slot, returning positive byte length.
// Returns 0 when no launch is pending.
int32_t wa_system_launcher_consume_launch(char* out, size_t cap);

int32_t wa_system_launcher_last_error(char* out, size_t cap);

#ifdef __cplusplus
}  // extern "C"
#endif
