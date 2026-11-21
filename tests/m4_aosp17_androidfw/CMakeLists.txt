cmake_minimum_required(VERSION 3.20)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(WasmAndroidM4Androidfw LANGUAGES C CXX)
    enable_testing()
endif()

include(../../cmake/Aosp17Core.cmake)

set(WA_ANDROID_SDK "$ENV{HOME}/Library/Android/sdk" CACHE PATH "Android SDK directory")
set(WA_AAPT2_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/aapt2"
    CACHE FILEPATH "SDK 36 aapt2")
set(WA_ANDROID_JAR "${WA_ANDROID_SDK}/platforms/android-36/android.jar"
    CACHE FILEPATH "SDK 36 android.jar")
foreach(tool IN ITEMS WA_AAPT2_EXECUTABLE WA_ANDROID_JAR)
    if(NOT EXISTS "${${tool}}")
        message(FATAL_ERROR "${tool} was not found at ${${tool}}")
    endif()
endforeach()

set(fixture "${CMAKE_CURRENT_SOURCE_DIR}/sample")
set(resources "${CMAKE_CURRENT_BINARY_DIR}/sample_resources.zip")
set(apk "${CMAKE_CURRENT_BINARY_DIR}/sample_aosp17_app.apk")

add_custom_command(OUTPUT "${resources}"
    COMMAND "${WA_AAPT2_EXECUTABLE}" compile --dir "${fixture}/res" -o "${resources}"
    DEPENDS "${WA_AAPT2_EXECUTABLE}" "${fixture}/res/values/strings.xml"
        "${fixture}/res/values-zh-rCN/strings.xml" "${fixture}/res/values/colors.xml"
        "${fixture}/res/layout/activity_main.xml"
    COMMENT "Compiling the fixture resources with SDK 36 aapt2"
    VERBATIM)
add_custom_command(OUTPUT "${apk}"
    COMMAND "${WA_AAPT2_EXECUTABLE}" link -I "${WA_ANDROID_JAR}"
        --manifest "${fixture}/AndroidManifest.xml" --min-sdk-version 23 --target-sdk-version 36
        -o "${apk}" "${resources}"
    DEPENDS "${resources}" "${fixture}/AndroidManifest.xml" "${WA_AAPT2_EXECUTABLE}"
        "${WA_ANDROID_JAR}"
    COMMENT "Linking sample_aosp17_app.apk with SDK 36 aapt2"
    VERBATIM)
add_custom_target(m4_aosp17_sample_apk DEPENDS "${apk}")

add_executable(test_aosp17_androidfw test_aosp17_androidfw.cpp)
target_link_libraries(test_aosp17_androidfw PRIVATE aosp17_libandroidfw)
add_dependencies(test_aosp17_androidfw m4_aosp17_sample_apk)

if(EMSCRIPTEN)
    find_program(WA_NODE_EXECUTABLE NAMES node HINTS /opt/homebrew/bin REQUIRED)
    set(apk_in_memfs "/sample_aosp17_app.apk")
    set_target_properties(test_aosp17_androidfw PROPERTIES SUFFIX ".js" LINK_DEPENDS "${apk}")
    target_compile_options(test_aosp17_androidfw PRIVATE -O3 -flto=thin -msimd128 -matomics
        -mbulk-memory -fwasm-exceptions)
    target_link_options(test_aosp17_androidfw PRIVATE -O3 -flto=thin -sENVIRONMENT=node
        "SHELL:--embed-file '${apk}@${apk_in_memfs}'")
    set(run_test "${WA_NODE_EXECUTABLE}" "$<TARGET_FILE:test_aosp17_androidfw>" "${apk_in_memfs}")
else()
    set(run_test "$<TARGET_FILE:test_aosp17_androidfw>" "${apk}")
endif()
add_test(NAME test_aosp17_androidfw COMMAND ${run_test})
add_test(NAME test_aosp17_androidfw_memory COMMAND ${run_test} --memory)
