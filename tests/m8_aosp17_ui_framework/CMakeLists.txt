cmake_minimum_required(VERSION 3.24)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(WasmAndroidAosp17UiFramework LANGUAGES C CXX)
    enable_testing()
    option(AOSP17_ART_INTERP_HGRAPH "Build the optional ART HGraph compiler" OFF)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Aosp17UiFramework.cmake")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(WA_AAPT2_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/aapt2" CACHE FILEPATH "SDK 36 aapt2")
set(WA_ZIPALIGN_EXECUTABLE "${WA_ANDROID_SDK}/build-tools/36.0.0/zipalign" CACHE FILEPATH "SDK 36 zipalign")
get_target_property(framework_dex aosp17_framework_core WA_FRAMEWORK_DEX)
get_target_property(framework_classes aosp17_framework_core WA_FRAMEWORK_CLASSES)
get_target_property(upstream_dex aosp17_upstream_framework WA_FRAMEWORK_DEX)
get_target_property(upstream_res aosp17_upstream_framework WA_FRAMEWORK_RES)
file(GLOB_RECURSE fixture_files CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${CMAKE_CURRENT_SOURCE_DIR}/fixture/*")
set(apk "${CMAKE_CURRENT_BINARY_DIR}/ui_smoke_app.apk")
add_custom_command(OUTPUT "${apk}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_smoke_apk.py"
        --out "${apk}" --aapt2 "${WA_AAPT2_EXECUTABLE}" --javac "${WA_JAVAC_EXECUTABLE}"
        --d8 "${WA_D8_EXECUTABLE}" --android-jar "${WA_ANDROID_JAR}" --zipalign "${WA_ZIPALIGN_EXECUTABLE}"
        --framework-jar "${framework_classes}"
    DEPENDS aosp17_framework_core "${framework_classes}" "${CMAKE_CURRENT_SOURCE_DIR}/build_smoke_apk.py"
        "${WA_AAPT2_EXECUTABLE}" "${WA_ZIPALIGN_EXECUTABLE}" ${fixture_files}
    VERBATIM)
add_custom_target(aosp17_ui_smoke_apk DEPENDS "${apk}")
add_executable(test_aosp17_ui_framework test_ui.cpp)
add_dependencies(test_aosp17_ui_framework aosp17_ui_smoke_apk)
target_link_libraries(test_aosp17_ui_framework PRIVATE aosp17_ui_jni)
if(EMSCRIPTEN)
    find_program(WA_NODE_EXECUTABLE node HINTS /opt/homebrew/bin NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    set_target_properties(test_aosp17_ui_framework PROPERTIES SUFFIX ".mjs")
    target_compile_options(test_aosp17_ui_framework PRIVATE -fwasm-exceptions)
    target_link_options(test_aosp17_ui_framework PRIVATE -sMODULARIZE=1 -sEXPORT_ES6=1
        -sENVIRONMENT=node -sINVOKE_RUN=0 -sFORCE_FILESYSTEM=1 -sALLOW_MEMORY_GROWTH=1
        -sSTACK_SIZE=2097152 "-sEXPORTED_RUNTIME_METHODS=FS,callMain")
    add_test(NAME m8_aosp17_ui_framework COMMAND "${WA_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/run_wasm.mjs" "$<TARGET_FILE:test_aosp17_ui_framework>"
        "${upstream_dex}" "${framework_dex}" "${apk}" "${upstream_res}")
else()
    add_test(NAME m8_aosp17_ui_framework COMMAND test_aosp17_ui_framework
        "${upstream_dex}" "${framework_dex}" "${apk}" "${upstream_res}")
endif()
