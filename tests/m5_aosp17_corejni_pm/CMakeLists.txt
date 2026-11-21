cmake_minimum_required(VERSION 3.20)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(WasmAndroidM5CoreJni LANGUAGES C CXX)
    enable_testing()
endif()

get_filename_component(wa_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${wa_root}/cmake/Aosp17CoreJni.cmake")

set(WA_ANDROID_SDK "$ENV{HOME}/Library/Android/sdk" CACHE PATH "Android SDK directory")
set(WA_AAPT2_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/aapt2"
    CACHE FILEPATH "SDK 36 aapt2")
set(WA_D8_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/d8" CACHE FILEPATH "SDK 36 d8")
set(WA_ZIPALIGN_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/zipalign"
    CACHE FILEPATH "SDK 36 zipalign")
set(WA_ANDROID_JAR "${WA_ANDROID_SDK}/platforms/android-36/android.jar"
    CACHE FILEPATH "SDK 36 Android API")
set(WA_JAVAC_EXECUTABLE "/usr/bin/javac" CACHE FILEPATH "Java compiler")
find_program(WA_ZIP_EXECUTABLE zip NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_program(WA_TOUCH_EXECUTABLE touch NO_CMAKE_FIND_ROOT_PATH REQUIRED)

set(ndk_home "$ENV{ANDROID_NDK_HOME}")
if(NOT ndk_home)
    set(ndk_home "${WA_ANDROID_SDK}/ndk/29.0.13846066")
endif()
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(ndk_host darwin-x86_64)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(ndk_host linux-x86_64)
else()
    message(FATAL_ERROR "The cube's NDK build supports macOS and Linux")
endif()
set(WA_ANDROID_NDK_BIN "${ndk_home}/toolchains/llvm/prebuilt/${ndk_host}/bin"
    CACHE PATH "NDK 29 toolchain bin directory")

set(fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixture")
set(apk_stage "${CMAKE_CURRENT_BINARY_DIR}/apk-stage")
set(resources "${CMAKE_CURRENT_BINARY_DIR}/cube_resources.zip")
set(class_dir "${CMAKE_CURRENT_BINARY_DIR}/classes")
set(class_file "${class_dir}/org/wasmandroid/cube/CubeActivity.class")
set(dex_dir "${CMAKE_CURRENT_BINARY_DIR}/dex")
set(dex "${dex_dir}/classes.dex")
set(apk "${CMAKE_CURRENT_BINARY_DIR}/wa_cube_app.apk")

add_custom_command(OUTPUT "${resources}"
    COMMAND "${WA_AAPT2_EXECUTABLE}" compile --dir "${fixture}/res" -o "${resources}"
    DEPENDS "${WA_AAPT2_EXECUTABLE}" "${fixture}/res/layout/activity_main.xml"
        "${fixture}/res/values/strings.xml" "${fixture}/res/values-zh-rCN/strings.xml"
    COMMENT "Compiling cube resources with SDK 36 aapt2"
    VERBATIM)
add_custom_command(OUTPUT "${class_file}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${class_dir}"
    COMMAND "${WA_JAVAC_EXECUTABLE}" --release 8 -encoding UTF-8 -classpath "${WA_ANDROID_JAR}"
        -d "${class_dir}" "${fixture}/CubeActivity.java"
    DEPENDS "${WA_JAVAC_EXECUTABLE}" "${WA_ANDROID_JAR}" "${fixture}/CubeActivity.java"
    COMMENT "Compiling the Android cube activity with javac"
    VERBATIM)
add_custom_command(OUTPUT "${dex}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${dex_dir}"
    COMMAND "${WA_D8_EXECUTABLE}" --min-api 23 --lib "${WA_ANDROID_JAR}"
        --output "${dex_dir}" "${class_file}"
    DEPENDS "${class_file}" "${WA_D8_EXECUTABLE}" "${WA_ANDROID_JAR}"
    COMMENT "Building classes.dex with SDK 36 d8"
    VERBATIM)

set(demo "${wa_root}/demos/ndk_gles3_cube")
set(ndk_out "${CMAKE_CURRENT_BINARY_DIR}/ndk")
set(cube_so "${ndk_out}/libndk_gles3_cube_arm64.so")
set(ndk_build "${wa_root}/tests/m1_chrome_ndk_cube/build_ndk.cmake")
add_custom_command(OUTPUT "${cube_so}"
    BYPRODUCTS "${ndk_out}/libndk_gles3_cube_armv7a.so" "${ndk_out}/build_ndk.sh"
    COMMAND "${CMAKE_COMMAND}" "-DDEMO=${demo}" "-DOUT=${ndk_out}"
        "-DNDK_BIN=${WA_ANDROID_NDK_BIN}" -P "${ndk_build}"
    DEPENDS "${demo}/build_ndk.sh" "${demo}/cube_demo.c" "${demo}/cube_demo.h" "${ndk_build}"
        "${WA_ANDROID_NDK_BIN}/aarch64-linux-android21-clang"
    COMMENT "Building the cube's Android shared libraries with NDK 29"
    VERBATIM)

set(cube_entry "lib/arm64-v8a/libndk_gles3_cube.so")
add_custom_command(OUTPUT "${apk}"
    BYPRODUCTS "${apk_stage}/classes.dex" "${apk_stage}/${cube_entry}"
        "${apk_stage}/cube-resources.apk"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${apk_stage}/lib/arm64-v8a"
    COMMAND "${CMAKE_COMMAND}" -E copy "${cube_so}" "${apk_stage}/${cube_entry}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${dex}" "${apk_stage}/classes.dex"
    # Give ZIP's extra entries the same timestamp as the aapt2 entries.
    COMMAND "${CMAKE_COMMAND}" -E env TZ=UTC "${WA_TOUCH_EXECUTABLE}" -t 198001010000
        "${apk_stage}/classes.dex" "${apk_stage}/${cube_entry}"
    COMMAND "${WA_AAPT2_EXECUTABLE}" link -I "${WA_ANDROID_JAR}"
        --manifest "${fixture}/AndroidManifest.xml" --min-sdk-version 23 --target-sdk-version 36
        -o "${apk_stage}/cube-resources.apk" "${resources}"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${apk_stage}" "${CMAKE_COMMAND}" -E env TZ=UTC
        "${WA_ZIP_EXECUTABLE}" -q -X -0 cube-resources.apk classes.dex "${cube_entry}"
    COMMAND "${WA_ZIPALIGN_EXECUTABLE}" -f 4 "${apk_stage}/cube-resources.apk" "${apk}.tmp"
    COMMAND "${CMAKE_COMMAND}" -E rename "${apk}.tmp" "${apk}"
    DEPENDS "${resources}" "${dex}" "${cube_so}" "${fixture}/AndroidManifest.xml"
        "${WA_AAPT2_EXECUTABLE}" "${WA_ANDROID_JAR}" "${WA_ZIP_EXECUTABLE}"
        "${WA_ZIPALIGN_EXECUTABLE}" "${WA_TOUCH_EXECUTABLE}"
    COMMENT "Assembling wa_cube_app.apk with compiled resources, DEX and arm64 ELF"
    VERBATIM)
add_custom_target(m5_aosp17_cube_apk DEPENDS "${apk}")

add_executable(test_aosp17_corejni_pm test_aosp17_corejni_pm.cpp)
target_link_libraries(test_aosp17_corejni_pm PRIVATE aosp17_core_jni)
add_dependencies(test_aosp17_corejni_pm m5_aosp17_cube_apk)

if(EMSCRIPTEN)
    find_program(WA_NODE_EXECUTABLE node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    set(apk_in_memfs "/wa_cube_app.apk")
    set_target_properties(test_aosp17_corejni_pm PROPERTIES SUFFIX ".js" LINK_DEPENDS "${apk}")
    target_compile_options(test_aosp17_corejni_pm PRIVATE -O3 -flto=thin -msimd128 -matomics
        -mbulk-memory -fwasm-exceptions)
    target_link_options(test_aosp17_corejni_pm PRIVATE -O3 -flto=thin -sENVIRONMENT=node
        "SHELL:--embed-file '${apk}@${apk_in_memfs}'")
    add_test(NAME test_aosp17_corejni_pm
        COMMAND "${WA_NODE_EXECUTABLE}" "$<TARGET_FILE:test_aosp17_corejni_pm>" "${apk_in_memfs}")
else()
    add_test(NAME test_aosp17_corejni_pm COMMAND test_aosp17_corejni_pm "${apk}")
endif()
set_tests_properties(test_aosp17_corejni_pm PROPERTIES TIMEOUT 30)
