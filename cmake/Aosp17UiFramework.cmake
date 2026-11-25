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
set(WA_AAPT2_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/aapt2" CACHE FILEPATH "SDK 36 aapt2")
set(_wa_upstream_output "${CMAKE_CURRENT_BINARY_DIR}/upstream-framework")
set(_wa_upstream_profile "${_wa_ui_root}/patches/aosp17/frameworks_base")
set(_wa_upstream_builder "${_wa_ui_root}/tools/aosp/build_upstream_framework_dex.py")
set(_wa_upstream_legacy_dex "${_wa_upstream_output}/aosp17_upstream_framework.dex")
set(_wa_upstream_dex "${_wa_upstream_output}/aosp17_upstream_framework.jar")
set(_wa_upstream_classes "${_wa_upstream_output}/aosp17_upstream_framework_classes.jar")
set(_wa_upstream_res "${_wa_upstream_output}/framework-res-upstream.apk")
file(GLOB _wa_upstream_patches CONFIGURE_DEPENDS
    "${_wa_upstream_profile}/*.patch" "${_wa_upstream_profile}/patches/*.patch")
# Keep the legacy filename first because standalone framework builds use it as the depfile target.
add_custom_command(OUTPUT "${_wa_upstream_legacy_dex}" "${_wa_upstream_dex}"
    "${_wa_upstream_classes}" "${_wa_upstream_res}"
    BYPRODUCTS "${_wa_upstream_output}/aosp17_upstream_services.dex"
        "${_wa_upstream_output}/framework_provenance.json"
    COMMAND "${Python3_EXECUTABLE}" "${_wa_upstream_builder}"
        --aosp-root "${AOSP17_SOURCE_DIR}" --android-sdk "${WA_ANDROID_SDK}"
        --javac "${WA_JAVAC_EXECUTABLE}" --android-jar "${WA_ANDROID_JAR}"
        --d8 "${WA_D8_EXECUTABLE}" --aapt2 "${WA_AAPT2_EXECUTABLE}"
        --out-dir "${_wa_upstream_output}" --depfile "${_wa_upstream_output}/framework.d"
    DEPENDS "${_wa_upstream_builder}" "${_wa_upstream_profile}/sources.txt" ${_wa_upstream_patches}
    DEPFILE "${_wa_upstream_output}/framework.d"
    COMMENT "Building the upstream framework DEX archive and resource APK"
    VERBATIM)
add_custom_target(aosp17_upstream_framework
    DEPENDS "${_wa_upstream_dex}" "${_wa_upstream_legacy_dex}" "${_wa_upstream_classes}" "${_wa_upstream_res}")
set_target_properties(aosp17_upstream_framework PROPERTIES WA_FRAMEWORK_DEX "${_wa_upstream_dex}"
    WA_FRAMEWORK_CLASSES "${_wa_upstream_classes}" WA_FRAMEWORK_RES "${_wa_upstream_res}")

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
        --android-jar "${WA_ANDROID_JAR}" --upstream-classes "${_wa_upstream_classes}"
        --upstream-dex "${_wa_upstream_dex}"
        --depfile "${_wa_ui_output}/framework.d"
    DEPENDS "${_wa_ui_script}" "${_wa_ui_sources}/upstream.txt" "${_wa_upstream_classes}"
        "${_wa_upstream_dex}" ${_wa_ui_java}
        "${WA_JAVAC_EXECUTABLE}" "${WA_D8_EXECUTABLE}" "${WA_ANDROID_JAR}"
    DEPFILE "${_wa_ui_output}/framework.d" VERBATIM)
add_custom_target(aosp17_framework_core ALL DEPENDS "${AOSP17_FRAMEWORK_DEX}" "${AOSP17_FRAMEWORK_CLASSES}")
add_dependencies(aosp17_framework_core aosp17_upstream_framework)
set_target_properties(aosp17_framework_core PROPERTIES
    WA_FRAMEWORK_DEX "${AOSP17_FRAMEWORK_DEX}" WA_FRAMEWORK_CLASSES "${AOSP17_FRAMEWORK_CLASSES}")

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
# Populate the release separately so only the decoder target participates in the build.
FetchContent_Declare(wa_libwebp
    URL https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz
    URL_HASH SHA512=5c159d9760efcb92749092536daada22c0a73c20926c76097a5f0448ddbf874cf761324ca97925ca5f578b30477564b2b072b47667e504673797128b31cafcbf
    SOURCE_SUBDIR cmake)
FetchContent_MakeAvailable(wa_libwebp)
function(_wa_add_webp_decoder)
    set(BUILD_SHARED_LIBS OFF)
    set(WEBP_LINK_STATIC ON)
    foreach(tool IN ITEMS ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO LIBWEBPMUX WEBPMUX EXTRAS)
        set(WEBP_BUILD_${tool} OFF)
    endforeach()
    add_subdirectory("${wa_libwebp_SOURCE_DIR}" "${wa_libwebp_BINARY_DIR}" EXCLUDE_FROM_ALL)
endfunction()
_wa_add_webp_decoder()

set(_wa_ui_native "${_wa_ui_root}/runtime/aosp17_shims/ui_jni")
_wa_art_static(aosp17_ui_jni "${_wa_ui_native}" ui_bridge.cpp canvas.cpp path_data.cpp bitmap.cpp apk_signing.cpp)
target_include_directories(aosp17_ui_jni PUBLIC "${_wa_ui_native}")
target_include_directories(aosp17_ui_jni SYSTEM PRIVATE "${AOSP17_SOURCE_DIR}/frameworks/native/include"
    "${wa_libwebp_SOURCE_DIR}/src")
target_link_libraries(aosp17_ui_jni PUBLIC aosp17_art_interp wa_hal_surfaceflinger
    PRIVATE aosp17_core_jni aosp17_zlib webpdecoder)
