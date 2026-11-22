include_guard(GLOBAL)

get_filename_component(wa_launcher_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(wa_aosp_tools "${wa_launcher_repo}/tools/aosp")
set(wa_aosp_patches "${wa_launcher_repo}/patches/aosp17")
set(wa_launcher_output "${CMAKE_CURRENT_BINARY_DIR}/launcher")
set(wa_upstream_framework_output "${CMAKE_CURRENT_BINARY_DIR}/upstream-framework")

set(WA_SYSTEMUI_DEPS_DIR "${wa_launcher_repo}/third_party/aosp17_deps" CACHE PATH
    "Offline Launcher3 and SystemUI dependencies")
set(wa_sdk_build_tools "${WA_ANDROID_SDK}/build-tools/36.0.0")
set(WA_AAPT2_EXECUTABLE "${wa_sdk_build_tools}/aapt2" CACHE FILEPATH "SDK 36 aapt2")
set(WA_ZIPALIGN_EXECUTABLE "${wa_sdk_build_tools}/zipalign" CACHE FILEPATH "SDK 36 zipalign")
set(WA_APKSIGNER_EXECUTABLE "${wa_sdk_build_tools}/apksigner" CACHE FILEPATH "SDK 36 apksigner")
find_program(WA_JAVA_EXECUTABLE java NO_CMAKE_FIND_ROOT_PATH REQUIRED)

set(WA_LAUNCHER_APK "${wa_launcher_output}/Launcher3-upstream.apk")
set(WA_SYSTEMUI_APK "${wa_launcher_output}/SystemUI-upstream.apk")
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

file(GLOB wa_systemui_patches CONFIGURE_DEPENDS "${wa_aosp_patches}/systemui/*.patch")
file(GLOB wa_systemui_kotlinc CONFIGURE_DEPENDS "${WA_SYSTEMUI_DEPS_DIR}/kotlinc/*.jar")
add_custom_command(OUTPUT "${WA_SYSTEMUI_APK}" "${wa_launcher_output}/systemui_provenance.json"
    COMMAND "${Python3_EXECUTABLE}" -B "${wa_aosp_tools}/build_upstream_systemui.py"
        --aosp-root "${AOSP17_SOURCE_DIR}" --deps-dir "${WA_SYSTEMUI_DEPS_DIR}"
        --java "${WA_JAVA_EXECUTABLE}" --javac "${WA_JAVAC_EXECUTABLE}"
        --aapt2 "${WA_AAPT2_EXECUTABLE}" --d8 "${WA_D8_EXECUTABLE}"
        --zipalign "${WA_ZIPALIGN_EXECUTABLE}" --apksigner "${WA_APKSIGNER_EXECUTABLE}"
        --android-jar "${WA_ANDROID_JAR}" --out-dir "${wa_launcher_output}"
        --depfile "${wa_launcher_output}/systemui.d"
    DEPENDS "${wa_aosp_tools}/build_upstream_systemui.py"
        "${wa_aosp_patches}/systemui/sources.txt" "${wa_aosp_patches}/systemui/resources.txt"
        ${wa_systemui_patches} ${wa_systemui_kotlinc}
    DEPFILE "${wa_launcher_output}/systemui.d"
    COMMENT "Compiling and signing the upstream AOSP 17 SystemUI APK"
    VERBATIM)

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

add_custom_target(wa_upstream_launcher_assets DEPENDS
    "${WA_LAUNCHER_APK}" "${WA_SYSTEMUI_APK}" "${WA_LAUNCHER_FRAMEWORK_DEX}"
    "${WA_LAUNCHER_FRAMEWORK_RES}")
add_dependencies(wa_upstream_launcher_assets aosp17_framework_core)
