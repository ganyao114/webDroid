include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/Aosp17Core.cmake")

set(_aosp17_nativehelper "${AOSP17_SOURCE_DIR}/libnativehelper")
set(_aosp17_framework_jni "${AOSP17_SOURCE_DIR}/frameworks/base/core/jni")
set(_aosp17_jni_shims "${CMAKE_CURRENT_LIST_DIR}/../runtime/aosp17_shims")

if(NOT TARGET aosp17_libnativehelper)
    # libnativehelper and its libnativehelper_any_vm dependency from Android.bp.
    _aosp17_add_static(aosp17_libnativehelper "${_aosp17_nativehelper}"
        DlHelp.c ExpandableString.c JNIHelp.c JNIPlatformHelp.c JniConstants.c
        JniInvocation.c file_descriptor_jni.c utf.cpp)
    target_include_directories(aosp17_libnativehelper SYSTEM PUBLIC
        "${_aosp17_nativehelper}/header_only_include"
        "${_aosp17_nativehelper}/include"
        "${_aosp17_nativehelper}/include_jni"
        "${_aosp17_nativehelper}/include_platform"
        "${_aosp17_nativehelper}/include_platform_header_only"
        PRIVATE "${AOSP17_SOURCE_DIR}/art/libdexfile")
    set_target_properties(aosp17_libnativehelper PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_features(aosp17_libnativehelper PRIVATE cxx_std_17)
    target_link_libraries(aosp17_libnativehelper PUBLIC aosp17_liblog PRIVATE ${CMAKE_DL_LIBS})
    if(NOT EMSCRIPTEN)
        find_package(Threads REQUIRED)
        target_link_libraries(aosp17_libnativehelper PRIVATE Threads::Threads)
    endif()
endif()

_aosp17_add_static(aosp17_core_jni "${_aosp17_framework_jni}"
    android_content_res_ApkAssets.cpp android_util_XmlBlock.cpp
    android_util_StringBlock.cpp android_util_AssetManager.cpp)
target_sources(aosp17_core_jni PRIVATE "${_aosp17_jni_shims}/core_jni/android_runtime.cpp")
target_include_directories(aosp17_core_jni PRIVATE
    "${_aosp17_framework_jni}" "${_aosp17_framework_jni}/include"
    "${_aosp17_jni_shims}/core/androidfw")
target_link_libraries(aosp17_core_jni PUBLIC aosp17_libandroidfw aosp17_libnativehelper)
