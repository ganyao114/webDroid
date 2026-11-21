cmake_minimum_required(VERSION 3.24)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(WasmAndroidAosp17Interpreter LANGUAGES C CXX)
    enable_testing()
endif()

get_filename_component(wa_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT "$ENV{ANDROID_SDK_ROOT}" STREQUAL "")
    set(sdk_default "$ENV{ANDROID_SDK_ROOT}")
elseif(NOT "$ENV{ANDROID_HOME}" STREQUAL "")
    set(sdk_default "$ENV{ANDROID_HOME}")
elseif(CMAKE_HOST_APPLE)
    set(sdk_default "$ENV{HOME}/Library/Android/sdk")
else()
    set(sdk_default "$ENV{HOME}/Android/Sdk")
endif()
set(WA_ANDROID_SDK "${sdk_default}" CACHE PATH "Android SDK directory")
include("${wa_root}/cmake/Aosp17ArtInterp.cmake")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(WA_NODE_EXECUTABLE node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_program(WA_JAVAC_EXECUTABLE javac NO_CMAKE_FIND_ROOT_PATH REQUIRED)
set(WA_D8_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/d8" CACHE FILEPATH "SDK 36 d8")
set(WA_AAPT2_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/aapt2" CACHE FILEPATH "SDK 36 aapt2")
set(WA_ANDROID_JAR "${WA_ANDROID_SDK}/platforms/android-36/android.jar" CACHE FILEPATH "SDK 36 API")

set(out "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
set(pm_dir "${out}/pm")
set(pm_dex "${pm_dir}/aosp17_pm_core.dex")
set(pm_api "${pm_dir}/pm_compile_api.jar")
set(pm_classes "${pm_dir}/pm_classes.jar")
set(pm_compiler "${wa_root}/tools/aosp/build_pm_dex.py")
add_custom_command(OUTPUT "${pm_dex}" "${pm_classes}" "${pm_api}" "${pm_dir}/pm_boot_profile.txt"
    COMMAND "${Python3_EXECUTABLE}" "${pm_compiler}" --aosp-root "${AOSP17_SOURCE_DIR}"
        --out-dir "${pm_dir}" --javac "${WA_JAVAC_EXECUTABLE}" --android-jar "${WA_ANDROID_JAR}"
        --d8 "${WA_D8_EXECUTABLE}" --depfile "${pm_dir}/pm.d"
    DEPENDS "${pm_compiler}" "${WA_JAVAC_EXECUTABLE}" "${WA_ANDROID_JAR}" "${WA_D8_EXECUTABLE}"
    DEPFILE "${pm_dir}/pm.d" VERBATIM)

set(bridge_dex "${out}/pm_res_bridge.dex")
set(asset_manager "${out}/generated/android/content/res/AssetManager.java")
set(asset_manager_jni "${AOSP17_SOURCE_DIR}/frameworks/base/core/jni/android_util_AssetManager.cpp")
set(jni_generator "${wa_root}/runtime/aosp17_shims/art_interp/generate_jni.py")
set(asset_manager_template "${CMAKE_CURRENT_SOURCE_DIR}/AssetManager.java.in")
set(bridge_compiler "${CMAKE_CURRENT_SOURCE_DIR}/build_res_bridge.py")
set(framework "${AOSP17_SOURCE_DIR}/frameworks/base/core/java")
set(upstream_java
    "${framework}/android/content/res/ApkAssets.java"
    "${framework}/android/content/res/XmlBlock.java"
    "${framework}/android/content/res/XmlResourceParser.java"
    "${framework}/android/content/res/loader/AssetsProvider.java")
set(wave6 "${wa_root}/tests/m6_aosp17_pm_java")
set(reused_glue
    android/annotation/AnyRes.java
    android/content/om/OverlayableInfo.java
    android/content/pm/SigningDetails.java
    android/content/res/AssetFileDescriptor.java
    android/content/res/StringBlock.java
    android/content/res/Validator.java
    android/os/Build.java
    android/os/FileUtils.java
    android/os/Trace.java
    android/ravenwood/annotation/RavenwoodKeepWholeClass.java
    android/system/OsConstants.java
    android/text/TextUtils.java
    android/util/ArrayMap.java
    android/util/Pair.java
    com/android/internal/annotations/GuardedBy.java
    com/android/internal/pm/pkg/component/AconfigFlags.java
    com/android/internal/util/ArrayUtils.java
    com/android/internal/util/CollectionUtils.java
    dalvik/annotation/optimization/CriticalNative.java
    dalvik/annotation/optimization/FastNative.java
    libcore/io/IoUtils.java)
list(TRANSFORM reused_glue PREPEND "${wave6}/jvm/")
file(GLOB_RECURSE bridge_glue CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/java/*.java")
add_custom_command(OUTPUT "${asset_manager}"
    COMMAND "${Python3_EXECUTABLE}" "${jni_generator}" --source "${asset_manager_jni}"
        --asset-manager "${asset_manager}" --template "${asset_manager_template}"
    DEPENDS "${jni_generator}" "${asset_manager_jni}" "${asset_manager_template}" VERBATIM)
add_custom_command(OUTPUT "${bridge_dex}"
    COMMAND "${Python3_EXECUTABLE}" "${bridge_compiler}" --out "${bridge_dex}"
        --javac "${WA_JAVAC_EXECUTABLE}" --d8 "${WA_D8_EXECUTABLE}" --android-jar "${WA_ANDROID_JAR}"
        --pm-api "${pm_api}" --pm-classes "${pm_classes}"
        "${asset_manager}" ${upstream_java} ${reused_glue} ${bridge_glue}
    DEPENDS "${bridge_compiler}" "${asset_manager}" ${upstream_java} ${reused_glue} ${bridge_glue}
        "${pm_api}" "${pm_classes}" "${WA_JAVAC_EXECUTABLE}" "${WA_ANDROID_JAR}" "${WA_D8_EXECUTABLE}"
    VERBATIM)

set(apks)
foreach(fixture Base Split FutureSdk)
    set(manifest "${wave6}/fixture/${fixture}.xml")
    set(apk "${out}/${fixture}.apk")
    add_custom_command(OUTPUT "${apk}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
        COMMAND "${WA_AAPT2_EXECUTABLE}" link -I "${WA_ANDROID_JAR}" --manifest "${manifest}" -o "${apk}"
        DEPENDS "${manifest}" "${WA_AAPT2_EXECUTABLE}" "${WA_ANDROID_JAR}" VERBATIM)
    list(APPEND apks "${apk}")
endforeach()
add_custom_target(aosp17_art_interp_fixtures DEPENDS "${pm_dex}" "${bridge_dex}" ${apks})

add_executable(test_aosp17_art_interp test_interp.cpp execute_wasm.cpp)
add_dependencies(test_aosp17_art_interp aosp17_art_interp_fixtures)
target_link_libraries(test_aosp17_art_interp PRIVATE aosp17_art_interp)
if(AOSP17_ART_INTERP_HGRAPH)
    target_compile_definitions(test_aosp17_art_interp PRIVATE WA_TEST_HGRAPH=1)
endif()
if(EMSCRIPTEN)
    set_target_properties(test_aosp17_art_interp PROPERTIES SUFFIX ".mjs")
    target_compile_options(test_aosp17_art_interp PRIVATE -O3 -fwasm-exceptions)
    target_link_options(test_aosp17_art_interp PRIVATE -O3 -sMODULARIZE=1 -sEXPORT_ES6=1
        -sENVIRONMENT=node -sINVOKE_RUN=0 -sFORCE_FILESYSTEM=1 -sALLOW_MEMORY_GROWTH=1
        -sSTACK_SIZE=1048576 "-sEXPORTED_RUNTIME_METHODS=FS,callMain")
    add_test(NAME test_aosp17_art_interp COMMAND "${WA_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/run_wasm.mjs" "$<TARGET_FILE:test_aosp17_art_interp>"
        "${pm_dex}" "${bridge_dex}" ${apks})
else()
    if(APPLE AND AOSP17_ART_INTERP_HGRAPH)
        find_library(javascriptcore JavaScriptCore REQUIRED)
        target_link_libraries(test_aosp17_art_interp PRIVATE "${javascriptcore}")
    endif()
    add_test(NAME test_aosp17_art_interp COMMAND test_aosp17_art_interp "${pm_dex}" "${bridge_dex}" ${apks})
endif()
