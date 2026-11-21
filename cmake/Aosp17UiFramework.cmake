include_guard(GLOBAL)

get_filename_component(_wa_ui_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(AOSP17_SOURCE_DIR "${_wa_ui_root}/third_party/aosp17" CACHE PATH "Read-only AOSP 17 source tree")
if(NOT "$ENV{ANDROID_SDK_ROOT}" STREQUAL "")
    set(_wa_ui_sdk "$ENV{ANDROID_SDK_ROOT}")
elseif(NOT "$ENV{ANDROID_HOME}" STREQUAL "")
    set(_wa_ui_sdk "$ENV{ANDROID_HOME}")
elseif(CMAKE_HOST_APPLE)
    set(_wa_ui_sdk "$ENV{HOME}/Library/Android/sdk")
else()
    set(_wa_ui_sdk "$ENV{HOME}/Android/Sdk")
endif()
set(WA_ANDROID_SDK "${_wa_ui_sdk}" CACHE PATH "Android SDK directory")
include("${CMAKE_CURRENT_LIST_DIR}/Aosp17ArtInterp.cmake")
if(NOT TARGET wa_hal_surfaceflinger)
    add_subdirectory("${_wa_ui_root}/runtime/hal_surfaceflinger" "${CMAKE_CURRENT_BINARY_DIR}/surfaceflinger")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(WA_JAVAC_EXECUTABLE "/usr/bin/javac" CACHE FILEPATH "JDK javac")
set(WA_D8_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/d8" CACHE FILEPATH "SDK 36 d8")
set(WA_ANDROID_JAR "${WA_ANDROID_SDK}/platforms/android-36/android.jar" CACHE FILEPATH "SDK 36 API")
set(_wa_ui_output "${CMAKE_CURRENT_BINARY_DIR}/aosp17-ui")
set(_wa_ui_script "${_wa_ui_root}/tools/aosp/build_framework_dex.py")
set(_wa_ui_sources "${_wa_ui_root}/tools/aosp/framework_build")
file(GLOB_RECURSE _wa_ui_java CONFIGURE_DEPENDS "${_wa_ui_sources}/java/*.java" "${_wa_ui_sources}/java_base/*.java"
    "${_wa_ui_sources}/boot_api/*.java")
set(AOSP17_FRAMEWORK_DEX "${_wa_ui_output}/aosp17_framework_core.dex")
set(AOSP17_FRAMEWORK_CLASSES "${_wa_ui_output}/aosp17_framework_classes.jar")
add_custom_command(OUTPUT "${AOSP17_FRAMEWORK_DEX}" "${AOSP17_FRAMEWORK_CLASSES}"
    COMMAND "${Python3_EXECUTABLE}" "${_wa_ui_script}"
        --aosp-root "${AOSP17_SOURCE_DIR}" --out-dir "${_wa_ui_output}"
        --javac "${WA_JAVAC_EXECUTABLE}" --d8 "${WA_D8_EXECUTABLE}"
        --android-jar "${WA_ANDROID_JAR}" --depfile "${_wa_ui_output}/framework.d"
    DEPENDS "${_wa_ui_script}" "${_wa_ui_sources}/upstream.txt" "${_wa_ui_sources}/AssetManager.java.in" ${_wa_ui_java}
        "${WA_JAVAC_EXECUTABLE}" "${WA_D8_EXECUTABLE}" "${WA_ANDROID_JAR}"
    DEPFILE "${_wa_ui_output}/framework.d" VERBATIM)
add_custom_target(aosp17_framework_core ALL DEPENDS "${AOSP17_FRAMEWORK_DEX}" "${AOSP17_FRAMEWORK_CLASSES}")
set_target_properties(aosp17_framework_core PROPERTIES
    WA_FRAMEWORK_DEX "${AOSP17_FRAMEWORK_DEX}" WA_FRAMEWORK_CLASSES "${AOSP17_FRAMEWORK_CLASSES}")

set(_wa_ui_native "${_wa_ui_root}/runtime/aosp17_shims/ui_jni")
_wa_art_static(aosp17_ui_jni "${_wa_ui_native}" ui_bridge.cpp canvas.cpp path_data.cpp bitmap.cpp)
target_include_directories(aosp17_ui_jni PUBLIC "${_wa_ui_native}")
target_link_libraries(aosp17_ui_jni PUBLIC aosp17_art_interp wa_hal_surfaceflinger PRIVATE aosp17_core_jni aosp17_zlib)
