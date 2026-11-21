cmake_minimum_required(VERSION 3.24)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(WasmAndroidAosp17PmJava LANGUAGES C CXX)
    enable_testing()
endif()

get_filename_component(wa_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT "$ENV{ANDROID_SDK_ROOT}" STREQUAL "")
    set(pm_sdk_default "$ENV{ANDROID_SDK_ROOT}")
elseif(NOT "$ENV{ANDROID_HOME}" STREQUAL "")
    set(pm_sdk_default "$ENV{ANDROID_HOME}")
elseif(CMAKE_HOST_APPLE)
    set(pm_sdk_default "$ENV{HOME}/Library/Android/sdk")
else()
    set(pm_sdk_default "$ENV{HOME}/Android/Sdk")
endif()
set(WA_ANDROID_SDK "${pm_sdk_default}" CACHE PATH "Android SDK directory")

if(NOT EMSCRIPTEN)
    # The same upstream JNI bindings used by the M5 test are loaded into HotSpot.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    include("${wa_root}/cmake/Aosp17CoreJni.cmake")
endif()
if(NOT TARGET aosp17_art_hgraph)
    include("${wa_root}/cmake/Aosp17ArtHGraph.cmake")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(WA_NODE_EXECUTABLE node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
set(WA_JAVAC_EXECUTABLE "/usr/bin/javac" CACHE FILEPATH "Host JDK 17 compiler")
set(WA_JAVA_EXECUTABLE "/usr/bin/java" CACHE FILEPATH "Host JVM")
set(WA_D8_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/d8" CACHE FILEPATH "SDK 36 d8")
set(WA_ANDROID_JAR "${WA_ANDROID_SDK}/platforms/android-36/android.jar"
    CACHE FILEPATH "SDK 36 Android API")

set(pm_compiler "${wa_root}/tools/aosp/build_pm_dex.py")
set(pm_out "${CMAKE_CURRENT_BINARY_DIR}/pm")
set(pm_dex "${pm_out}/aosp17_pm_core.dex")
set(pm_profile "${pm_out}/pm_boot_profile.txt")
set(pm_classes "${pm_out}/pm_classes.jar")
set(pm_api "${pm_out}/pm_compile_api.jar")
set(pm_command "${Python3_EXECUTABLE}" "${pm_compiler}"
    --aosp-root "${AOSP17_SOURCE_DIR}" --out-dir "${pm_out}"
    --javac "${WA_JAVAC_EXECUTABLE}" --android-jar "${WA_ANDROID_JAR}" --d8 "${WA_D8_EXECUTABLE}")
file(GLOB_RECURSE pm_api_sources CONFIGURE_DEPENDS "${wa_root}/tools/aosp/pm_build/*.java")
add_custom_command(OUTPUT "${pm_dex}" "${pm_profile}" "${pm_classes}" "${pm_api}"
    COMMAND ${pm_command} --depfile "${pm_out}/pm.d"
    DEPENDS "${pm_compiler}" ${pm_api_sources} "${WA_JAVAC_EXECUTABLE}"
        "${WA_ANDROID_JAR}" "${WA_D8_EXECUTABLE}"
    DEPFILE "${pm_out}/pm.d"
    COMMENT "Compiling upstream AOSP 17 Package Manager sources with javac and SDK 36 d8"
    VERBATIM)
add_custom_target(aosp17_pm_dex DEPENDS "${pm_dex}" "${pm_profile}" "${pm_classes}" "${pm_api}")

add_executable(test_aosp17_pm_dex test_aosp17_pm_dex.cpp)
target_link_libraries(test_aosp17_pm_dex PRIVATE aosp17_art_hgraph aosp17_libdexfile)
add_dependencies(test_aosp17_pm_dex aosp17_pm_dex)

if(EMSCRIPTEN)
    set_target_properties(test_aosp17_pm_dex PROPERTIES SUFFIX ".mjs")
    target_compile_options(test_aosp17_pm_dex PRIVATE
        -O3 -msimd128 -matomics -mbulk-memory -fwasm-exceptions)
    target_link_options(test_aosp17_pm_dex PRIVATE -O3 -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=node
        -sINVOKE_RUN=0 -sFORCE_FILESYSTEM=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576
        "-sEXPORTED_RUNTIME_METHODS=FS,callMain")
    set(pm_mode wasm)
else()
    set(pm_mode native)
endif()
add_test(NAME test_aosp17_pm_dex COMMAND "${WA_NODE_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/run_pm_probe.mjs" "${pm_mode}"
    "$<TARGET_FILE:test_aosp17_pm_dex>" "${pm_dex}" "${pm_profile}"
    "${pm_out}/pm_hgraph_probe.wasm")
add_test(NAME test_aosp17_pm_dex_reproducible COMMAND ${pm_command} --check)

if(NOT EMSCRIPTEN)
    find_package(JNI REQUIRED COMPONENTS JVM)
    set(WA_AAPT2_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/aapt2"
        CACHE FILEPATH "SDK 36 aapt2")
    set(pm_jvm "${CMAKE_CURRENT_BINARY_DIR}/jvm")
    set(pm_host_classes "${pm_jvm}/classes")
    set(res_src "${AOSP17_SOURCE_DIR}/frameworks/base/core/java/android/content/res")
    set(pm_xml_sources "${res_src}/ApkAssets.java" "${res_src}/XmlBlock.java"
        "${res_src}/XmlResourceParser.java")
    add_library(aosp17_pm_test_jni SHARED register_resource_natives.cpp)
    target_link_libraries(aosp17_pm_test_jni PRIVATE aosp17_core_jni JNI::JVM)

    file(GLOB_RECURSE pm_jvm_fixtures CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/jvm/*.java")
    set(pm_jvm_test "${CMAKE_CURRENT_SOURCE_DIR}/PmBehaviorTest.java")
    set(pm_jvm_class "${pm_host_classes}/PmBehaviorTest.class")
    add_custom_command(OUTPUT "${pm_jvm_class}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${pm_host_classes}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${pm_host_classes}"
        COMMAND "${WA_JAVAC_EXECUTABLE}" --release 17 -encoding UTF-8 -proc:none -sourcepath ""
            -classpath "${pm_classes}:${pm_api}:${WA_ANDROID_JAR}"
            -d "${pm_host_classes}" "${pm_jvm_test}" ${pm_jvm_fixtures} ${pm_xml_sources}
        DEPENDS "${pm_classes}" "${pm_api}" "${pm_jvm_test}" ${pm_jvm_fixtures} ${pm_xml_sources}
            "${WA_JAVAC_EXECUTABLE}"
        COMMENT "Compiling the host JVM test and upstream ApkAssets and XmlBlock"
        VERBATIM)

    set(pm_fixture_apks)
    foreach(fixture Base Split FutureSdk)
        set(manifest "${CMAKE_CURRENT_SOURCE_DIR}/fixture/${fixture}.xml")
        set(apk "${pm_jvm}/${fixture}.apk")
        add_custom_command(OUTPUT "${apk}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${pm_jvm}"
            COMMAND "${WA_AAPT2_EXECUTABLE}" link -I "${WA_ANDROID_JAR}" --manifest "${manifest}"
                -o "${apk}"
            DEPENDS "${manifest}" "${WA_AAPT2_EXECUTABLE}" "${WA_ANDROID_JAR}"
            COMMENT "Linking the ${fixture} binary AndroidManifest with aapt2"
            VERBATIM)
        list(APPEND pm_fixture_apks "${apk}")
    endforeach()
    add_custom_target(aosp17_pm_jvm_fixtures DEPENDS "${pm_jvm_class}" ${pm_fixture_apks})
    add_dependencies(test_aosp17_pm_dex aosp17_pm_test_jni aosp17_pm_jvm_fixtures)

    # The hidden API declarations are on javac's classpath only.
    add_test(NAME test_aosp17_pm_jvm COMMAND "${WA_JAVA_EXECUTABLE}"
        -cp "${pm_host_classes}:${pm_classes}:${WA_ANDROID_JAR}" PmBehaviorTest
        "$<TARGET_FILE:aosp17_pm_test_jni>" ${pm_fixture_apks})
endif()
