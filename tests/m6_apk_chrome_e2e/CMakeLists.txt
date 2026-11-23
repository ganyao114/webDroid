set(sdk_tools "${WA_ANDROID_SDK}/build-tools/36.0.0")
set(WA_AAPT2_EXECUTABLE "${sdk_tools}/aapt2" CACHE FILEPATH "SDK 36 aapt2")
set(WA_D8_EXECUTABLE "${sdk_tools}/d8" CACHE FILEPATH "SDK 36 d8")
set(WA_ZIPALIGN_EXECUTABLE "${sdk_tools}/zipalign" CACHE FILEPATH "SDK 36 zipalign")
set(WA_ANDROID_JAR "${WA_ANDROID_SDK}/platforms/android-36/android.jar"
    CACHE FILEPATH "SDK 36 Android API")
set(WA_JAVAC_EXECUTABLE "/usr/bin/javac" CACHE FILEPATH "Java compiler")
set(WA_ANDROID_NDK_BIN
    "$ENV{HOME}/Library/Android/sdk/ndk/29.0.13846066/toolchains/llvm/prebuilt/darwin-x86_64/bin"
    CACHE PATH "Directory containing the Android NDK 29 clang drivers")
find_program(WA_NODE_EXECUTABLE node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_program(WA_JAR_EXECUTABLE jar NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_program(WA_ZIP_EXECUTABLE zip NO_CMAKE_FIND_ROOT_PATH REQUIRED)

set(fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixture")
set(demo "${CMAKE_CURRENT_SOURCE_DIR}/../../demos/ndk_gles3_cube")
set(ndk_script "${CMAKE_CURRENT_SOURCE_DIR}/../m1_chrome_ndk_cube/build_ndk.cmake")
set(ndk "${CMAKE_CURRENT_BINARY_DIR}/ndk")
set(arm32 "${ndk}/libndk_gles3_cube_armv7a.so")
set(arm64 "${ndk}/libndk_gles3_cube_arm64.so")
add_custom_command(OUTPUT "${arm32}" "${arm64}"
    COMMAND "${CMAKE_COMMAND}" "-DDEMO=${demo}" "-DOUT=${ndk}"
        "-DNDK_BIN=${WA_ANDROID_NDK_BIN}" -P "${ndk_script}"
    DEPENDS "${ndk_script}" "${demo}/build_ndk.sh" "${demo}/cube_demo.c" "${demo}/cube_demo.h"
    VERBATIM)

set(resources "${CMAKE_CURRENT_BINARY_DIR}/resources.zip")
set(package_dir "${CMAKE_CURRENT_BINARY_DIR}/package")
set(java "${CMAKE_CURRENT_BINARY_DIR}/java")
set(classes "${CMAKE_CURRENT_BINARY_DIR}/classes")
set(resources_apk "${CMAKE_CURRENT_BINARY_DIR}/resources.apk")
set(r_java "${java}/org/wasmandroid/cube/R.java")
set(dex "${package_dir}/classes.dex")
set(unaligned "${CMAKE_CURRENT_BINARY_DIR}/wa_cube_app-unaligned.apk")
set(apk "${CMAKE_CURRENT_BINARY_DIR}/wa_cube_app.apk")
add_custom_command(OUTPUT "${resources}"
    COMMAND "${WA_AAPT2_EXECUTABLE}" compile --dir "${fixture}/res" -o "${resources}"
    DEPENDS "${WA_AAPT2_EXECUTABLE}" "${fixture}/res/values/strings.xml"
        "${fixture}/res/values-zh-rCN/strings.xml" "${fixture}/res/layout/activity_main.xml"
    VERBATIM)
add_custom_command(OUTPUT "${resources_apk}" "${r_java}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${java}"
    COMMAND "${WA_AAPT2_EXECUTABLE}" link -I "${WA_ANDROID_JAR}"
        --manifest "${fixture}/AndroidManifest.xml" --min-sdk-version 23 --target-sdk-version 36
        --java "${java}" -o "${resources_apk}" "${resources}"
    DEPENDS "${resources}" "${fixture}/AndroidManifest.xml" "${WA_AAPT2_EXECUTABLE}" "${WA_ANDROID_JAR}"
    VERBATIM)
add_custom_command(OUTPUT "${dex}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${classes}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${classes}" "${package_dir}"
    COMMAND "${WA_JAVAC_EXECUTABLE}" --release 8 -g -classpath "${WA_ANDROID_JAR}" -d "${classes}"
        "${r_java}" "${fixture}/CubeActivity.java"
    COMMAND "${WA_JAR_EXECUTABLE}" --create --file "${CMAKE_CURRENT_BINARY_DIR}/cube.jar" -C "${classes}" .
    COMMAND "${WA_D8_EXECUTABLE}" --debug --min-api 23 --lib "${WA_ANDROID_JAR}"
        --output "${package_dir}" "${CMAKE_CURRENT_BINARY_DIR}/cube.jar"
    DEPENDS "${r_java}" "${fixture}/CubeActivity.java" "${WA_JAVAC_EXECUTABLE}"
        "${WA_JAR_EXECUTABLE}" "${WA_D8_EXECUTABLE}" "${WA_ANDROID_JAR}"
    VERBATIM)
add_custom_command(OUTPUT "${apk}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${package_dir}/lib/armeabi-v7a" "${package_dir}/lib/arm64-v8a"
    COMMAND "${CMAKE_COMMAND}" -E copy "${arm32}" "${package_dir}/lib/armeabi-v7a/libndk_gles3_cube.so"
    COMMAND "${CMAKE_COMMAND}" -E copy "${arm64}" "${package_dir}/lib/arm64-v8a/libndk_gles3_cube.so"
    COMMAND "${CMAKE_COMMAND}" -E copy "${resources_apk}" "${unaligned}"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${package_dir}" "${WA_ZIP_EXECUTABLE}" -q -0 "${unaligned}"
        classes.dex lib/armeabi-v7a/libndk_gles3_cube.so lib/arm64-v8a/libndk_gles3_cube.so
    COMMAND "${WA_ZIPALIGN_EXECUTABLE}" -f -P 16 4 "${unaligned}" "${apk}"
    COMMAND "${WA_ZIPALIGN_EXECUTABLE}" -c -P 16 4 "${apk}"
    DEPENDS "${dex}" "${resources_apk}" "${arm32}" "${arm64}" "${WA_ZIPALIGN_EXECUTABLE}" "${WA_ZIP_EXECUTABLE}"
    COMMENT "Packaging and verifying wa_cube_app.apk for both Android ABIs"
    VERBATIM)
add_custom_target(wa_cube_app_apk ALL DEPENDS "${apk}")

set(verify "${CMAKE_CURRENT_SOURCE_DIR}/verify_m6_apk_node.mjs")
if(EMSCRIPTEN)
    add_test(NAME test_m6_apk_node COMMAND "${WA_NODE_EXECUTABLE}" "${verify}"
        --wasm-build "${CMAKE_BINARY_DIR}" --apk "${apk}")
    set_tests_properties(test_m6_apk_node PROPERTIES TIMEOUT 120)
else()
    find_path(WA_EGL_INCLUDE_DIR EGL/egl.h HINTS /opt/homebrew/opt/mesa PATH_SUFFIXES include REQUIRED)
    find_path(WA_GLES3_INCLUDE_DIR GLES3/gl3.h HINTS /opt/homebrew/opt/mesa PATH_SUFFIXES include REQUIRED)
    find_library(WA_EGL_LIBRARY EGL HINTS /opt/homebrew/opt/mesa PATH_SUFFIXES lib REQUIRED)
    find_library(WA_GLES3_LIBRARY NAMES GLESv2 GLESv3 HINTS /opt/homebrew/opt/mesa PATH_SUFFIXES lib REQUIRED)

    add_executable(test_m6_apk_host test_m6_apk_host.cpp)
    target_include_directories(test_m6_apk_host SYSTEM PRIVATE "${WA_EGL_INCLUDE_DIR}" "${WA_GLES3_INCLUDE_DIR}")
    target_link_libraries(test_m6_apk_host PRIVATE wa_android_system aosp17_libandroidfw
        "${WA_EGL_LIBRARY}" "${WA_GLES3_LIBRARY}")
    add_dependencies(test_m6_apk_host wa_cube_app_apk)
    add_test(NAME test_m6_apk_host COMMAND test_m6_apk_host "${apk}"
        "${WA_NODE_EXECUTABLE}" "${verify}" "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(test_m6_apk_host PROPERTIES TIMEOUT 120)
endif()
