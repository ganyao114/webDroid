include_guard(GLOBAL)

get_filename_component(wa_launcher_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(wa_aosp_tools "${wa_launcher_repo}/tools/aosp")
set(wa_aosp_patches "${wa_launcher_repo}/patches/aosp17")
if(NOT DEFINED wa_launcher_output)
    set(wa_launcher_output "${CMAKE_CURRENT_BINARY_DIR}/launcher")
endif()
set(wa_upstream_framework_output "${CMAKE_CURRENT_BINARY_DIR}/upstream-framework")
if(NOT DEFINED wa_settings_output)
    set(wa_settings_output "${CMAKE_CURRENT_BINARY_DIR}/upstream-settings")
endif()
set(WA_BUILTIN_APPS_DIR "${CMAKE_CURRENT_BINARY_DIR}/upstream-apps")

set(WA_SYSTEMUI_DEPS_DIR "${wa_launcher_repo}/third_party/aosp17_deps" CACHE PATH
    "Offline Launcher3 and SystemUI dependencies")
set(WA_SETTINGS_DEPS_DIR "${WA_SYSTEMUI_DEPS_DIR}" CACHE PATH "Offline Settings dependencies")
set(wa_sdk_build_tools "${WA_ANDROID_SDK}/build-tools/36.0.0")
set(WA_AAPT2_EXECUTABLE "${wa_sdk_build_tools}/aapt2" CACHE FILEPATH "SDK 36 aapt2")
set(WA_ZIPALIGN_EXECUTABLE "${wa_sdk_build_tools}/zipalign" CACHE FILEPATH "SDK 36 zipalign")
set(WA_APKSIGNER_EXECUTABLE "${wa_sdk_build_tools}/apksigner" CACHE FILEPATH "SDK 36 apksigner")
find_program(WA_JAVA_EXECUTABLE java NO_CMAKE_FIND_ROOT_PATH REQUIRED)

set(WA_LAUNCHER_APK "${wa_launcher_output}/Launcher3-upstream.apk")
set(WA_SYSTEMUI_APK "${wa_launcher_output}/SystemUI-upstream.apk")
set(WA_SETTINGS_APK "${wa_settings_output}/Settings-upstream.apk")
set(WA_BUILTIN_APKS)
foreach(name IN ITEMS DeskClock Browser2 Gallery2 Music)
    list(APPEND WA_BUILTIN_APKS "${WA_BUILTIN_APPS_DIR}/${name}-upstream.apk")
endforeach()
set(WA_SYSTEM_APP_APKS "${WA_SETTINGS_APK}" ${WA_BUILTIN_APKS})
set(WA_LAUNCHER_FRAMEWORK_DEX "${wa_upstream_framework_output}/aosp17_upstream_framework.dex")
set(WA_LAUNCHER_FRAMEWORK_RES "${wa_upstream_framework_output}/framework-res-upstream.apk")

file(GLOB_RECURSE wa_launcher_patches CONFIGURE_DEPENDS "${wa_aosp_patches}/launcher3/*")
add_custom_command(OUTPUT "${WA_LAUNCHER_APK}" "${wa_launcher_output}/upstream_provenance.json"
    COMMAND "${Python3_EXECUTABLE}" "${wa_aosp_tools}/build_upstream_launcher3.py"
        --aosp-root "${AOSP17_SOURCE_DIR}" --deps "${WA_SYSTEMUI_DEPS_DIR}"
        --sdk "${WA_ANDROID_SDK}" --java "${WA_JAVA_EXECUTABLE}" --javac "${WA_JAVAC_EXECUTABLE}"
        --out-dir "${wa_launcher_output}" --depfile "${wa_launcher_output}/launcher3.d"
    DEPENDS "${wa_aosp_tools}/build_upstream_launcher3.py" ${wa_launcher_patches}
    DEPFILE "${wa_launcher_output}/launcher3.d"
    COMMENT "Compiling and signing the upstream AOSP 17 Launcher3 APK"
    VERBATIM)
add_custom_target(wa_upstream_launcher_apk DEPENDS "${WA_LAUNCHER_APK}"
    "${wa_launcher_output}/upstream_provenance.json")

file(GLOB wa_systemui_patches CONFIGURE_DEPENDS "${wa_aosp_patches}/systemui/*.patch")
file(GLOB wa_systemui_kotlinc CONFIGURE_DEPENDS "${WA_SYSTEMUI_DEPS_DIR}/kotlinc/*.jar")
set(wa_build_systemui "${Python3_EXECUTABLE}" -B "${wa_aosp_tools}/build_upstream_systemui.py"
    --aosp-root "${AOSP17_SOURCE_DIR}" --deps-dir "${WA_SYSTEMUI_DEPS_DIR}"
    --java "${WA_JAVA_EXECUTABLE}" --javac "${WA_JAVAC_EXECUTABLE}"
    --aapt2 "${WA_AAPT2_EXECUTABLE}" --d8 "${WA_D8_EXECUTABLE}"
    --zipalign "${WA_ZIPALIGN_EXECUTABLE}" --apksigner "${WA_APKSIGNER_EXECUTABLE}"
    --android-jar "${WA_ANDROID_JAR}" --out-dir "${wa_launcher_output}")
add_custom_command(OUTPUT "${WA_SYSTEMUI_APK}" "${wa_launcher_output}/systemui_provenance.json"
    COMMAND ${wa_build_systemui} --depfile "${wa_launcher_output}/systemui.d"
    DEPENDS "${wa_aosp_tools}/build_upstream_systemui.py"
        "${wa_aosp_patches}/systemui/sources.txt" "${wa_aosp_patches}/systemui/resources.txt"
        ${wa_systemui_patches} ${wa_systemui_kotlinc}
    DEPFILE "${wa_launcher_output}/systemui.d"
    COMMENT "Compiling and signing the upstream AOSP 17 SystemUI APK"
    VERBATIM)
add_custom_target(wa_upstream_systemui_apk DEPENDS "${WA_SYSTEMUI_APK}"
    "${wa_launcher_output}/systemui_provenance.json")

set(wa_settings_profile "${wa_aosp_patches}/settings")
file(GLOB wa_settings_patches CONFIGURE_DEPENDS "${wa_settings_profile}/*.patch")
set(wa_settings_provenance "${wa_settings_output}/upstream_provenance.json")
set(wa_settings_driver "${wa_aosp_tools}/build_upstream_settings.py")
add_custom_command(OUTPUT "${WA_SETTINGS_APK}" "${wa_settings_provenance}"
    COMMAND "${Python3_EXECUTABLE}" -B "${wa_settings_driver}"
        --aosp-root "${AOSP17_SOURCE_DIR}" --deps-dir "${WA_SETTINGS_DEPS_DIR}"
        --sdk "${WA_ANDROID_SDK}" --android-jar "${WA_ANDROID_JAR}"
        --java "${WA_JAVA_EXECUTABLE}" --javac "${WA_JAVAC_EXECUTABLE}"
        --aapt2 "${WA_AAPT2_EXECUTABLE}" --d8 "${WA_D8_EXECUTABLE}"
        --zipalign "${WA_ZIPALIGN_EXECUTABLE}" --apksigner "${WA_APKSIGNER_EXECUTABLE}"
        --out-dir "${wa_settings_output}" --depfile "${wa_settings_output}/settings.d"
    DEPENDS "${wa_settings_driver}" "${wa_settings_profile}/sources.txt"
        "${wa_settings_profile}/resources.txt" "${wa_settings_profile}/PreferenceLink.java"
        ${wa_settings_patches}
    DEPFILE "${wa_settings_output}/settings.d"
    COMMENT "Compiling and signing the upstream AOSP 17 Settings APK"
    VERBATIM)
add_custom_target(wa_upstream_settings_apk DEPENDS "${WA_SETTINGS_APK}" "${wa_settings_provenance}")

file(GLOB wa_builtin_patches CONFIGURE_DEPENDS
    "${wa_aosp_patches}/deskclock/*.patch" "${wa_aosp_patches}/browser2/*.patch"
    "${wa_aosp_patches}/music/*.patch" "${wa_aosp_patches}/gallery2/*.patch")
set(wa_builtin_provenance "${WA_BUILTIN_APPS_DIR}/upstream_apps_provenance.json")
set(wa_builtin_driver "${wa_aosp_tools}/build_upstream_apps.py")
add_custom_command(OUTPUT "${wa_builtin_provenance}" ${WA_BUILTIN_APKS}
    COMMAND "${Python3_EXECUTABLE}" -B "${wa_builtin_driver}"
        --aosp-root "${AOSP17_SOURCE_DIR}" --deps "${WA_SYSTEMUI_DEPS_DIR}"
        --sdk "${WA_ANDROID_SDK}" --java "${WA_JAVA_EXECUTABLE}" --javac "${WA_JAVAC_EXECUTABLE}"
        --out-dir "${WA_BUILTIN_APPS_DIR}" --depfile "${WA_BUILTIN_APPS_DIR}/apps.d"
    DEPENDS "${wa_builtin_driver}" ${wa_builtin_patches}
    DEPFILE "${WA_BUILTIN_APPS_DIR}/apps.d"
    COMMENT "Compiling and signing the upstream AOSP 17 DeskClock, Browser2, Gallery2 and Music APKs"
    VERBATIM)
add_custom_target(wa_upstream_builtin_apps DEPENDS "${wa_builtin_provenance}" ${WA_BUILTIN_APKS})
set_target_properties(wa_upstream_builtin_apps PROPERTIES WA_BUILTIN_APPS_DIR "${WA_BUILTIN_APPS_DIR}")

file(GLOB wa_upstream_framework_patches CONFIGURE_DEPENDS "${wa_aosp_patches}/frameworks_base/*.patch")
add_custom_command(OUTPUT "${WA_LAUNCHER_FRAMEWORK_DEX}" "${WA_LAUNCHER_FRAMEWORK_RES}"
    BYPRODUCTS "${wa_upstream_framework_output}/aosp17_upstream_framework_classes.jar"
        "${wa_upstream_framework_output}/aosp17_upstream_services.dex"
        "${wa_upstream_framework_output}/framework_provenance.json"
    COMMAND "${Python3_EXECUTABLE}" "${wa_aosp_tools}/build_upstream_framework_dex.py"
        --aosp-root "${AOSP17_SOURCE_DIR}" --android-sdk "${WA_ANDROID_SDK}"
        --javac "${WA_JAVAC_EXECUTABLE}" --android-jar "${WA_ANDROID_JAR}"
        --d8 "${WA_D8_EXECUTABLE}" --aapt2 "${WA_AAPT2_EXECUTABLE}"
        --out-dir "${wa_upstream_framework_output}"
        --depfile "${wa_upstream_framework_output}/framework.d"
    DEPENDS "${wa_aosp_tools}/build_upstream_framework_dex.py"
        "${wa_aosp_patches}/frameworks_base/sources.txt" ${wa_upstream_framework_patches}
    DEPFILE "${wa_upstream_framework_output}/framework.d"
    COMMENT "Building the upstream framework DEX and resource APK for Launcher3"
    VERBATIM)

add_custom_target(wa_foreground_framework_assets
    DEPENDS "${WA_LAUNCHER_FRAMEWORK_DEX}" "${WA_LAUNCHER_FRAMEWORK_RES}")
add_dependencies(wa_foreground_framework_assets aosp17_framework_core)

add_custom_target(wa_upstream_launcher_assets)
add_dependencies(wa_upstream_launcher_assets wa_upstream_launcher_apk wa_upstream_systemui_apk
    wa_upstream_settings_apk wa_upstream_builtin_apps wa_foreground_framework_assets)
